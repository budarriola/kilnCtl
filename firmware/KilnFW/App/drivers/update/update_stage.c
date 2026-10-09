// update_stage.c -- see update_stage.h. Pure C, flash and SHA injected.
#include "update_stage.h"

#include <string.h>

#include "ota_esp_image_header.h"
#include "update_semver.h"

static void lk(update_stage_t *st)
{
    if (st->io.lock) {
        st->io.lock(st->io.ctx);
    }
}
static void ul(update_stage_t *st)
{
    if (st->io.unlock) {
        st->io.unlock(st->io.ctx);
    }
}

// Claim the single operation slot. False (nothing changed) if one is active.
static bool claim(update_stage_t *st, update_stage_phase_t phase)
{
    bool won = false;
    lk(st);
    if (st->phase == UPDATE_STAGE_IDLE) {
        st->phase = phase;
        won = true;
    }
    ul(st);
    return won;
}

static void set_phase(update_stage_t *st, update_stage_phase_t phase)
{
    lk(st);
    st->phase = phase;
    ul(st);
}

static void sha_abort(update_stage_t *st)
{
    if (st->io.sha_abort) {
        st->io.sha_abort(st->io.ctx);
    }
}

void update_stage_init(update_stage_t *st, const update_stage_io_t *io)
{
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof(*st));
    if (io != NULL) {
        st->io = *io;
    }
}

uint32_t update_stage_capacity(const update_stage_t *st)
{
    if (st == NULL || st->io.partition_size <= STAGE_IMAGE_OFFSET) {
        return 0;
    }
    // Whole 4 KB sectors only, so every erase stays sector-aligned.
    return (st->io.partition_size - STAGE_IMAGE_OFFSET) & ~(STAGE_HEADER_SECTOR - 1u);
}

static bool io_ok(const update_stage_io_t *io)
{
    return io->erase && io->write && io->read && io->sha_start && io->sha_update && io->sha_finish &&
           io->partition_size > STAGE_IMAGE_OFFSET;
}

// Strip one leading v/V (the header stores the normalised form).
static const char *strip_v(const char *s)
{
    return (s[0] == 'v' || s[0] == 'V') ? s + 1 : s;
}

static update_stage_err_t map_hdr(stage_hdr_status_t s)
{
    switch (s) {
    case STAGE_HDR_OK: return UPDATE_STAGE_OK;
    case STAGE_HDR_BAD_SEMVER: return UPDATE_STAGE_ERR_BAD_VERSION;
    case STAGE_HDR_BAD_COMMIT: return UPDATE_STAGE_ERR_BAD_COMMIT;
    case STAGE_HDR_BAD_LENGTH: return UPDATE_STAGE_ERR_OVERSIZE;
    default: return UPDATE_STAGE_ERR_ARG;
    }
}

// Dry-run the header encode so a bad semver/commit is refused before any
// flash access (or before the first image byte is written).
static update_stage_err_t dry_run_header(const update_stage_t *st)
{
    stage_header_t h;
    memset(&h, 0, sizeof(h));
    h.state = STAGE_STATE_VERIFIED;
    h.image_length = st->total;
    memcpy(h.semver, st->semver, sizeof(h.semver));
    memcpy(h.commit, st->commit, sizeof(h.commit));
    h.source = st->source;
    uint8_t out[STAGE_HEADER_SIZE];
    return map_hdr(stage_header_encode(&h, update_stage_capacity(st), out));
}

update_stage_err_t update_stage_upload_begin(update_stage_t *st, uint8_t *scratch, size_t scratch_len,
                                             uint32_t total_len, const char *semver, const char *commit,
                                             stage_source_t source)
{
    if (st == NULL || scratch == NULL || scratch_len < UPDATE_STAGE_SCRATCH_MIN || !io_ok(&st->io)) {
        return UPDATE_STAGE_ERR_ARG;
    }
    if (total_len == 0) {
        return UPDATE_STAGE_ERR_EMPTY;
    }
    if (total_len > update_stage_capacity(st)) {
        return UPDATE_STAGE_ERR_OVERSIZE; // before any flash access
    }
    if (total_len < UPDATE_STAGE_HEAD_LEN) {
        return UPDATE_STAGE_ERR_BAD_IMAGE;
    }
    char sv[STAGE_SEMVER_FIELD_LEN];
    char cm[STAGE_COMMIT_HEX_LEN + 1];
    memset(sv, 0, sizeof(sv));
    memset(cm, 0, sizeof(cm));
    bool given = (semver != NULL && semver[0] != '\0');
    if (given) {
        const char *s = strip_v(semver);
        if (strlen(s) >= sizeof(sv)) {
            return UPDATE_STAGE_ERR_BAD_VERSION;
        }
        strcpy(sv, s);
    }
    if (commit != NULL && commit[0] != '\0') {
        if (strlen(commit) != STAGE_COMMIT_HEX_LEN) {
            return UPDATE_STAGE_ERR_BAD_COMMIT;
        }
        memcpy(cm, commit, STAGE_COMMIT_HEX_LEN);
    }

    if (!claim(st, UPDATE_STAGE_UPLOADING)) {
        return UPDATE_STAGE_ERR_BUSY;
    }
    st->scratch = scratch;
    st->scratch_len = scratch_len;
    st->total = total_len;
    st->received = 0;
    st->written = 0;
    st->erased_upto = 0;
    st->source = source;
    memcpy(st->semver, sv, sizeof(st->semver));
    memcpy(st->commit, cm, sizeof(st->commit));
    st->semver_given = given;
    st->gate = NULL;
    st->gate_ctx = NULL;
    st->head_len = 0;
    st->head_flushed = false;
    st->cache_valid = false;
    st->bad_valid = false;

    if (given) {
        update_stage_err_t e = dry_run_header(st);
        if (e != UPDATE_STAGE_OK) {
            set_phase(st, UPDATE_STAGE_IDLE);
            return e;
        }
    }
    // The header sector is NOT erased here: flush_head erases it only after the
    // identity/version/policy checks pass, so a refused upload leaves the stage intact.
    return UPDATE_STAGE_OK;
}

static update_stage_err_t fail(update_stage_t *st, update_stage_err_t e)
{
    sha_abort(st);
    set_phase(st, UPDATE_STAGE_IDLE); // stage blank only if flush_head already erased the header
    return e;
}

// Erase ahead, write, hash.
static update_stage_err_t put(update_stage_t *st, const uint8_t *data, size_t len)
{
    const uint32_t cap = update_stage_capacity(st);
    const uint64_t end = (uint64_t)st->written + len;
    while ((uint64_t)st->erased_upto < end) {
        uint32_t n = UPDATE_STAGE_ERASE_UNIT;
        if (n > cap - st->erased_upto) {
            n = cap - st->erased_upto;
        }
        if (n == 0 || st->io.erase(st->io.ctx, STAGE_IMAGE_OFFSET + st->erased_upto, n) != 0) {
            return UPDATE_STAGE_ERR_FLASH;
        }
        st->erased_upto += n;
        if (st->io.yield) {
            st->io.yield(st->io.ctx);
        }
    }
    if (len > 0 && st->io.write(st->io.ctx, STAGE_IMAGE_OFFSET + st->written, data, len) != 0) {
        return UPDATE_STAGE_ERR_FLASH;
    }
    if (len > 0 && st->io.sha_update(st->io.ctx, data, len) != 0) {
        return UPDATE_STAGE_ERR_HASH;
    }
    st->written += (uint32_t)len;
    return UPDATE_STAGE_OK;
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// The first UPDATE_STAGE_HEAD_LEN bytes are held back until they prove this is
// an ESP32-S3 app image with a usable version; only then does the image area
// get touched.
static update_stage_err_t flush_head(update_stage_t *st)
{
    if (ota_esp_image_header_check(st->head, OTA_ESP_IMAGE_HEADER_LEN, UPDATE_STAGE_ESP_IMAGE_MAGIC,
                                   UPDATE_STAGE_ESP32S3_CHIP_ID) != OTA_ESP_IMAGE_HEADER_OK) {
        return UPDATE_STAGE_ERR_BAD_IMAGE;
    }
    if (rd_u32(st->head + UPDATE_STAGE_APP_DESC_OFFSET) != UPDATE_STAGE_APP_DESC_MAGIC) {
        return UPDATE_STAGE_ERR_BAD_IMAGE;
    }
    // Project identity: an image built for another project is refused before any flash write.
    // project_name is NUL-padded; require the exact name, not a prefix.
    {
        const char *name = (const char *)(st->head + UPDATE_STAGE_APP_DESC_PROJECT_OFFSET);
        size_t n = 0;
        while (n < UPDATE_STAGE_APP_DESC_PROJECT_LEN && name[n] != '\0') {
            n++;
        }
        if (n != strlen(UPDATE_STAGE_EXPECTED_PROJECT) || memcmp(name, UPDATE_STAGE_EXPECTED_PROJECT, n) != 0) {
            return UPDATE_STAGE_ERR_WRONG_PROJECT;
        }
    }
    char v[STAGE_SEMVER_FIELD_LEN + 1];
    memcpy(v, st->head + UPDATE_STAGE_APP_DESC_VERSION_OFFSET, STAGE_SEMVER_FIELD_LEN);
    v[STAGE_SEMVER_FIELD_LEN] = '\0';
    if (st->semver_given) {
        // A declared version may not override a valid one in the descriptor (else 99.0.0 on an old image
        // would walk past the downgrade gate). An invalid/blank descriptor version leaves the declared one. Only gated (hand-upload) stagers
        // check: the release fetch's tag is not necessarily the build's descriptor string.
        update_semver_t dv;
        if (st->gate != NULL && update_semver_parse(strip_v(v), &dv) && strcmp(strip_v(v), st->semver) != 0) {
            return UPDATE_STAGE_ERR_VERSION_MISMATCH;
        }
    } else {
        const char *s = strip_v(v);
        if (strlen(s) >= sizeof(st->semver) || s[0] == '\0') {
            return UPDATE_STAGE_ERR_BAD_VERSION;
        }
        memset(st->semver, 0, sizeof(st->semver));
        strcpy(st->semver, s);
        if (dry_run_header(st) != UPDATE_STAGE_OK) {
            return UPDATE_STAGE_ERR_BAD_VERSION;
        }
    }
    if (st->gate != NULL) {
        update_image_id_t id;
        const bool have_id = update_image_id_find(st->head, UPDATE_STAGE_HEAD_LEN, UPDATE_STAGE_IMAGE_ID_FROM, &id);
        update_stage_err_t g = st->gate(st->gate_ctx, st->semver, st->commit, have_id ? &id : NULL);
        if (g != UPDATE_STAGE_OK) {
            return UPDATE_STAGE_ERR_POLICY;
        }
    }
    // Checks passed: from here the stage reads as blank until upload_finish writes a header.
    if (st->io.erase(st->io.ctx, 0, STAGE_HEADER_SECTOR) != 0) {
        return UPDATE_STAGE_ERR_FLASH;
    }
    st->cache_valid = false;
    st->bad_valid = false;
    if (st->io.sha_start(st->io.ctx) != 0) {
        return UPDATE_STAGE_ERR_HASH;
    }
    return put(st, st->head, UPDATE_STAGE_HEAD_LEN);
}

void update_stage_set_gate(update_stage_t *st, update_stage_gate_fn gate, void *ctx)
{
    if (st != NULL) {
        st->gate = gate;
        st->gate_ctx = ctx;
    }
}

update_stage_err_t update_stage_upload_write(update_stage_t *st, const uint8_t *data, size_t len)
{
    if (st == NULL || (data == NULL && len > 0)) {
        return UPDATE_STAGE_ERR_ARG;
    }
    if (st->phase != UPDATE_STAGE_UPLOADING) {
        return UPDATE_STAGE_ERR_STATE;
    }
    if ((uint64_t)st->received + len > st->total) {
        return fail(st, UPDATE_STAGE_ERR_OVERRUN);
    }
    st->received += (uint32_t)len;

    while (len > 0 && st->head_len < UPDATE_STAGE_HEAD_LEN) {
        size_t n = UPDATE_STAGE_HEAD_LEN - st->head_len;
        if (n > len) {
            n = len;
        }
        memcpy(st->head + st->head_len, data, n);
        st->head_len += (uint32_t)n;
        data += n;
        len -= n;
    }
    if (st->head_len == UPDATE_STAGE_HEAD_LEN && !st->head_flushed) {
        update_stage_err_t e = flush_head(st);
        if (e != UPDATE_STAGE_OK) {
            return fail(st, e);
        }
        st->head_flushed = true;
    }
    if (len > 0) {
        update_stage_err_t e = put(st, data, len);
        if (e != UPDATE_STAGE_OK) {
            return fail(st, e);
        }
    }
    return UPDATE_STAGE_OK;
}

// Re-reads `length` image bytes from flash through the scratch buffer and
// hashes them.
static update_stage_err_t hash_from_flash(update_stage_t *st, uint32_t length, uint8_t out[STAGE_SHA256_LEN])
{
    if (st->io.sha_start(st->io.ctx) != 0) {
        return UPDATE_STAGE_ERR_HASH;
    }
    uint32_t off = 0;
    while (off < length) {
        size_t n = st->scratch_len;
        if (n > length - off) {
            n = length - off;
        }
        if (st->io.read(st->io.ctx, STAGE_IMAGE_OFFSET + off, st->scratch, n) != 0) {
            sha_abort(st);
            return UPDATE_STAGE_ERR_FLASH;
        }
        if (st->io.sha_update(st->io.ctx, st->scratch, n) != 0) {
            sha_abort(st);
            return UPDATE_STAGE_ERR_HASH;
        }
        const uint32_t prev = off;
        off += (uint32_t)n;
        if (st->io.yield && (off / UPDATE_STAGE_ERASE_UNIT) != (prev / UPDATE_STAGE_ERASE_UNIT)) {
            st->io.yield(st->io.ctx);
        }
    }
    if (st->io.sha_finish(st->io.ctx, out) != 0) {
        sha_abort(st);
        return UPDATE_STAGE_ERR_HASH;
    }
    return UPDATE_STAGE_OK;
}

update_stage_err_t update_stage_manifest_gate(void *ctx, const char *semver, const char *commit,
                                              const update_image_id_t *id)
{
    (void)semver;
    (void)commit;
    const update_identity_t *want = ctx;
    if (want == NULL || id == NULL) {
        return UPDATE_STAGE_ERR_POLICY;
    }
    if (id->zones_cfg_version != want->zones_cfg_version || id->kilnlink_version != want->kilnlink_version ||
        id->uart_version != want->uart_version) {
        return UPDATE_STAGE_ERR_POLICY;
    }
    return UPDATE_STAGE_OK;
}

update_stage_err_t update_stage_upload_finish(update_stage_t *st)
{
    if (st == NULL) {
        return UPDATE_STAGE_ERR_ARG;
    }
    if (st->phase != UPDATE_STAGE_UPLOADING) {
        return UPDATE_STAGE_ERR_STATE;
    }
    if (st->received != st->total || !st->head_flushed || st->written != st->total) {
        return fail(st, UPDATE_STAGE_ERR_SHORT);
    }
    set_phase(st, UPDATE_STAGE_VERIFYING);

    uint8_t streamed[STAGE_SHA256_LEN];
    if (st->io.sha_finish(st->io.ctx, streamed) != 0) {
        return fail(st, UPDATE_STAGE_ERR_HASH);
    }
    uint8_t reread[STAGE_SHA256_LEN];
    update_stage_err_t e = hash_from_flash(st, st->total, reread);
    if (e != UPDATE_STAGE_OK) {
        return fail(st, e);
    }
    if (memcmp(streamed, reread, STAGE_SHA256_LEN) != 0) {
        return fail(st, UPDATE_STAGE_ERR_READBACK);
    }

    stage_header_t h;
    memset(&h, 0, sizeof(h));
    h.state = STAGE_STATE_VERIFIED;
    h.image_length = st->total;
    memcpy(h.sha256, streamed, STAGE_SHA256_LEN);
    memcpy(h.semver, st->semver, sizeof(h.semver));
    memcpy(h.commit, st->commit, sizeof(h.commit));
    h.source = st->source;
    uint8_t hb[STAGE_HEADER_SIZE];
    if (stage_header_encode(&h, update_stage_capacity(st), hb) != STAGE_HDR_OK) {
        return fail(st, UPDATE_STAGE_ERR_HEADER);
    }
    // The header is the last write. If it fails or reads back wrong, erase it
    // again (best effort): a stage without a valid header is "not staged".
    uint8_t back[STAGE_HEADER_SIZE];
    stage_header_t chk;
    if (st->io.write(st->io.ctx, 0, hb, sizeof(hb)) != 0 || st->io.read(st->io.ctx, 0, back, sizeof(back)) != 0 ||
        stage_header_decode(back, sizeof(back), update_stage_capacity(st), &chk) != STAGE_HDR_OK ||
        !stage_header_is_installable(&chk) || !stage_header_sha256_matches(&chk, streamed)) {
        (void)st->io.erase(st->io.ctx, 0, STAGE_HEADER_SECTOR);
        return fail(st, UPDATE_STAGE_ERR_HEADER);
    }
    memcpy(st->cache_sha, streamed, STAGE_SHA256_LEN);
    st->cache_len = st->total;
    st->cache_valid = true;
    st->bad_valid = false;
    set_phase(st, UPDATE_STAGE_IDLE);
    return UPDATE_STAGE_OK;
}

void update_stage_upload_abort(update_stage_t *st)
{
    if (st == NULL) {
        return;
    }
    if (st->phase == UPDATE_STAGE_UPLOADING || st->phase == UPDATE_STAGE_VERIFYING) {
        sha_abort(st);
        set_phase(st, UPDATE_STAGE_IDLE);
    }
}

update_stage_err_t update_stage_clear(update_stage_t *st)
{
    if (st == NULL || !io_ok(&st->io)) {
        return UPDATE_STAGE_ERR_ARG;
    }
    if (!claim(st, UPDATE_STAGE_CLEARING)) {
        return UPDATE_STAGE_ERR_BUSY;
    }
    st->cache_valid = false;
    st->bad_valid = false;
    int rc = st->io.erase(st->io.ctx, 0, STAGE_HEADER_SECTOR);
    set_phase(st, UPDATE_STAGE_IDLE);
    return rc == 0 ? UPDATE_STAGE_OK : UPDATE_STAGE_ERR_FLASH;
}

update_stage_err_t update_stage_get_status(update_stage_t *st, uint8_t *scratch, size_t scratch_len,
                                           update_stage_info_t *out)
{
    if (st == NULL || out == NULL || !io_ok(&st->io)) {
        return UPDATE_STAGE_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->hdr_status = STAGE_HDR_BLANK;
    if (!claim(st, UPDATE_STAGE_CHECKING)) {
        lk(st);
        out->phase = st->phase;
        out->bytes_done = st->received;
        out->bytes_total = st->total;
        ul(st);
        out->busy = true;
        out->reason = "busy";
        return UPDATE_STAGE_ERR_BUSY;
    }
    if (scratch == NULL || scratch_len < UPDATE_STAGE_SCRATCH_MIN) {
        set_phase(st, UPDATE_STAGE_IDLE);
        return UPDATE_STAGE_ERR_ARG;
    }
    st->scratch = scratch;
    st->scratch_len = scratch_len;

    update_stage_err_t ret = UPDATE_STAGE_OK;
    uint8_t hb[STAGE_HEADER_SIZE];
    stage_header_t h;
    uint8_t computed[STAGE_SHA256_LEN];
    if (st->io.read(st->io.ctx, 0, hb, sizeof(hb)) != 0) {
        out->reason = "read_error";
        ret = UPDATE_STAGE_ERR_FLASH;
        goto done;
    }
    out->hdr_status = stage_header_decode(hb, sizeof(hb), update_stage_capacity(st), &h);
    if (out->hdr_status == STAGE_HDR_BLANK) {
        out->reason = "blank";
        goto done;
    }
    if (out->hdr_status != STAGE_HDR_OK) {
        out->reason = "bad_header";
        goto done;
    }
    out->state = h.state;
    out->image_length = h.image_length;
    memcpy(out->sha256, h.sha256, STAGE_SHA256_LEN);
    memcpy(out->semver, h.semver, sizeof(out->semver));
    memcpy(out->commit, h.commit, sizeof(out->commit));
    out->source = h.source;
    if (!stage_header_is_installable(&h)) {
        out->reason = "not_verified";
        goto done;
    }
    if (st->cache_valid && st->cache_len == h.image_length && stage_header_sha256_matches(&h, st->cache_sha)) {
        out->staged = true;
        out->reason = "";
        goto done;
    }
    if (st->bad_valid && st->bad_len == h.image_length && memcmp(st->bad_sha, h.sha256, STAGE_SHA256_LEN) == 0) {
        out->reason = "sha_mismatch";
        goto done;
    }
    {
        update_stage_err_t e = hash_from_flash(st, h.image_length, computed);
        if (e != UPDATE_STAGE_OK) {
            out->reason = "read_error";
            ret = e;
            goto done;
        }
    }
    if (!stage_header_sha256_matches(&h, computed)) {
        memcpy(st->bad_sha, h.sha256, STAGE_SHA256_LEN);
        st->bad_len = h.image_length;
        st->bad_valid = true;
        out->reason = "sha_mismatch";
        goto done;
    }
    memcpy(st->cache_sha, computed, STAGE_SHA256_LEN);
    st->cache_len = h.image_length;
    st->cache_valid = true;
    out->staged = true;
    out->reason = "";
done:
    set_phase(st, UPDATE_STAGE_IDLE);
    return ret;
}

const char *update_stage_err_name(update_stage_err_t e)
{
    switch (e) {
    case UPDATE_STAGE_OK: return "ok";
    case UPDATE_STAGE_ERR_ARG: return "bad_argument";
    case UPDATE_STAGE_ERR_BUSY: return "busy";
    case UPDATE_STAGE_ERR_EMPTY: return "empty_upload";
    case UPDATE_STAGE_ERR_OVERSIZE: return "image_too_large";
    case UPDATE_STAGE_ERR_BAD_IMAGE: return "not_an_esp32s3_app_image";
    case UPDATE_STAGE_ERR_BAD_VERSION: return "bad_version";
    case UPDATE_STAGE_ERR_BAD_COMMIT: return "bad_commit";
    case UPDATE_STAGE_ERR_OVERRUN: return "more_bytes_than_declared";
    case UPDATE_STAGE_ERR_SHORT: return "upload_incomplete";
    case UPDATE_STAGE_ERR_FLASH: return "flash_error";
    case UPDATE_STAGE_ERR_HASH: return "hash_error";
    case UPDATE_STAGE_ERR_READBACK: return "readback_mismatch";
    case UPDATE_STAGE_ERR_HEADER: return "header_write_failed";
    case UPDATE_STAGE_ERR_STATE: return "out_of_sequence";
    case UPDATE_STAGE_ERR_WRONG_PROJECT: return "wrong_project";
    case UPDATE_STAGE_ERR_VERSION_MISMATCH: return "version_mismatch";
    case UPDATE_STAGE_ERR_POLICY: return "policy_refused";
    }
    return "unknown";
}

const char *update_stage_phase_name(update_stage_phase_t p)
{
    switch (p) {
    case UPDATE_STAGE_IDLE: return "idle";
    case UPDATE_STAGE_UPLOADING: return "uploading";
    case UPDATE_STAGE_VERIFYING: return "verifying";
    case UPDATE_STAGE_CLEARING: return "clearing";
    case UPDATE_STAGE_CHECKING: return "checking";
    }
    return "unknown";
}

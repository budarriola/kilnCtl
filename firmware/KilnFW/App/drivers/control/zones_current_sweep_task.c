#include "zones_http_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "MAX31856.h"
#include "autotune_engine.h"
#include "kiln_io.h"
#include "profile_executor.h"
#include "safety_trip_words.h"
#include "relay_authority.h"
#include "safety_cfg_store.h"
#include "uart_task_ids.h" /* SAFETY_FLAG_* for zones_get_safety_wiring() */

/* opus review finding (LOW): zone_sweep_task_record_ct_channels()'s summed-
 * topology unmeasured path packs zone index zi into a uint8_t bitmask
 * (s_sweep.summed_unmeasured_mask) via `1u << zi` -- silently dropping any
 * zone at index >= 8 with no compile-time signal at all if the zone count
 * ever grew past what a uint8_t mask can hold. Fail the build instead. */
_Static_assert(MAX31856_CHANNEL_COUNT <= 8, "summed_unmeasured_mask is a uint8_t bitmask, one bit per zone");

static uint8_t zone_sweep_task_relay_mask_for_zone(void *ctx, uint8_t zi)
{
    (void)ctx;
    return s_zones.cfg.zones[zi].relay_mask;
}

static void zone_sweep_task_set_zone_index(void *ctx, uint8_t zi)
{
    (void)ctx;
    s_sweep.zone_index = zi;
}

/* CT_COMMISSIONING_PLAN.md step 3. Sampled fresh at the start of every
 * zone_sweep_task() run (never persisted across runs -- a stale idle sample
 * from a previous sweep would silently misattribute today's baseline draw).
 * s_ct_topology_summed false (per_zone, the default and every board before
 * this field existed) leaves both callbacks below on their original,
 * unmodified per-zone-CT behaviour. */
static bool s_ct_topology_summed = false;
static float s_ct_summed_idle_a = 0.0f;

/* CT_COMMISSIONING_PLAN.md step 3 -- 0x031F, U8, 0=per_zone/1=summed. Same
 * committed-cache read as zone_cfg_committed_f32() above, u8 twin. Unset
 * (never fetched -- an older Pico, or an uncommissioned one) reads as
 * per_zone: the safe, silent default this field was designed to have
 * (CONFIG_REFERENCE.md). */
static uint8_t zone_cfg_committed_ct_topology(void)
{
    size_t count = safety_cfg_store_param_count();
    for (size_t i = 0; i < count; i++) {
        safety_cfg_param_t row;
        memset(&row, 0, sizeof(row));
        if (!safety_cfg_store_get_by_index(i, &row) || row.param_id != 0x031Fu) {
            continue;
        }
        return row.set ? row.value.u8_val : 0u;
    }
    return 0u;
}

static void zone_sweep_task_record_normal(void *ctx, uint8_t zi, float avg_a)
{
    (void)ctx;
    if (s_ct_topology_summed) {
        /* CT_COMMISSIONING_PLAN.md step 3: in summed mode the normal comes
         * from channel 3 alone (zone_sweep_task_record_ct_channels() below),
         * not from sample_current()'s ct_mask-based sum -- avg_a here is
         * simply not the right number in this topology. */
        return;
    }
    zone_normals_set(zi, avg_a);
}

/* ---- M12: the derived CT map, accumulated across one sweep run ------------
 * Written only by the sweep task (one at a time, enforced by s_sweep.active
 * and the heat claim), read only by zone_sweep_push_ct_channel_map() on that
 * same task, so unlike s_sweep this needs no volatile. Reset by
 * zone_sweep_task() before the loop starts. */
static struct {
    uint8_t zone_for_ch[ZONE_CT_CHANNEL_COUNT];
    uint8_t derived_mask;      /* channels resolved unambiguously this run */
    uint8_t conflict_mask;     /* channels TWO zones both claimed -- see below */
    uint8_t unresolved_zone_mask;
    /* M12b: the measured half of the k_ct calibration -- the sum, over every
     * zone this run resolved, of that zone's dominant CT channel reading.
     * Accumulated here (not recomputed later) because the per-zone
     * per-channel averages exist only for the length of one
     * zone_sweep_run_all_zones() iteration. */
    float   measured_total_a;
    /* M12b: whether zone_sweep_push_ct_channel_map() ended in a failure it
     * could not fully back out. The k_ct push must not run after one: both
     * write through the SAME staged-config buffer on the Pico, so committing
     * k_ct on top of a staged buffer known to hold ct_channel_map values
     * that were meant to be discarded would commit exactly the leftovers
     * that function's H3 repair exists to prevent. */
    bool    map_push_failed;
} s_ct_derive;

/* Forward declaration + identical redefinition (legal in C when the token
 * sequence matches exactly -- see the canonical definitions/doc comments
 * further down this file) so zone_sweep_task_record_ct_channels() below can
 * read the shared channel's live, committed k_ct_v_per_a for the 2026-09-10
 * noise-floor rescaling fix (finding C) without reordering the rest of this
 * file's M12b section. */
#define ZONE_KCT_PARAM_ID(ch) ((uint16_t)(0x0308u + (ch)))
static bool zone_cfg_committed_f32(uint16_t param_id, float *out);

static void zone_sweep_task_record_ct_channels(void *ctx, uint8_t zi, uint8_t relay_mask,
                                                const float *per_ch_avg_a)
{
    (void)ctx;
    if (s_ct_topology_summed) {
        /* CT_COMMISSIONING_PLAN.md step 3: summed topology has no per-relay
         * channel mapping to derive at all -- one shared CT (channel 3,
         * index ZONE_CT_CHANNEL_COUNT-1) reads every zone, so the one-relay-
         * one-channel check (GUARD_TEST_MATRIX.md sec 3.3) is skipped
         * entirely rather than attempted and refused: s_ct_derive is left
         * untouched (derived_mask stays 0), which is also what makes
         * zone_sweep_push_ct_channel_map()/zone_sweep_push_k_ct_v_per_a()
         * naturally no-op afterward -- neither of those Pico-side fields
         * means anything in this topology. Instead, this zone's normal is
         * derived straight from the shared channel and recorded here (the
         * only place with both per_ch_avg_a and zi in hand). */
        float with_on = per_ch_avg_a[ZONE_CT_CHANNEL_COUNT - 1];
        if (!isnan(with_on)) {
            float normal_a = 0.0f;
            /* 2026-09-10 fix (finding C): the shared channel's live,
             * committed k_ct_v_per_a -- 0.0f (zone_cfg_committed_f32
             * returns false) for "never committed", which
             * zone_sweep_summed_normal_a() treats as "use the reference
             * floor unchanged". Read the same way zone_sweep_derive_k_ct's
             * own `k_old` is, further down this file. */
            float live_k_ct = 0.0f;
            (void)zone_cfg_committed_f32(ZONE_KCT_PARAM_ID(ZONE_CT_CHANNEL_COUNT - 1), &live_k_ct);
            if (zone_sweep_summed_normal_a(with_on, s_ct_summed_idle_a, live_k_ct, &normal_a)) {
                zone_normals_set(zi, normal_a);
            } else {
                /* opus review finding (MEDIUM): the shared channel read
                 * LOWER with this zone on than idle -- a wiring/noise
                 * artifact, not a real measurement. Do NOT persist a zero
                 * (that would silently make S14/S15 inert for this zone
                 * forever); record it as unmeasured instead so the operator
                 * sees which zone needs a re-sweep.
                 *
                 * opus review finding (LOW): this used to gate on `zi < 8`
                 * to protect the uint8_t mask below from a wider zone count
                 * silently dropping high zones with no signal at all. The
                 * _Static_assert above makes that impossible at compile
                 * time instead -- MAX31856_CHANNEL_COUNT growing past 8
                 * without this file being revisited is now a build failure,
                 * not a silently dropped zone. */
                ESP_LOGW(ZONES_HTTP_TAG, "zone %u: summed-CT reading with relay on (%.3fA) was below the idle "
                              "baseline (%.3fA) -- treating as unmeasured, not persisting a normal",
                         zi, (double)with_on, (double)s_ct_summed_idle_a);
                s_sweep.summed_unmeasured_mask |= (uint8_t)(1u << zi);
            }
        }
        return;
    }
    uint8_t ch = 0;
    /* ct_channel_map[] is indexed into the Pico's relay_now_mask directly
     * (safety_core.c: `ctx.relay_now_mask & (1u << ct_channel_map[ch])`), so
     * the value written has to be a RELAY bit position, and the whole
     * mapping is only meaningful while zone id and relay id are the same
     * number -- which is what config_store.h's "zone/relay id" comment
     * assumes and what a stock three-zone board actually is. Refuse to
     * derive for any zone where that identity does not hold (more than one
     * relay, or a relay outside bits 0-2): the sweep genuinely cannot say
     * which single relay a channel's current belongs to, and writing the
     * zone index anyway would point S14 at some other zone's relay. */
    if (relay_mask != (uint8_t)(1u << zi) || zi >= 3u) {
        if (zi < 8) {
            s_ct_derive.unresolved_zone_mask |= (uint8_t)(1u << zi);
        }
        return;
    }
    if (!zone_sweep_derive_ct_channel(per_ch_avg_a, &ch)) {
        if (zi < 8) {
            s_ct_derive.unresolved_zone_mask |= (uint8_t)(1u << zi);
        }
        return;
    }
    /* Two zones dominating the SAME channel is not a mapping -- it is the
     * one-to-one inversion COMMISSIONING_UX.md sec 1.2 requires failing, and
     * it fails in a direction no per-zone check can see (each zone looked
     * perfectly unambiguous on its own). Drop the channel entirely rather
     * than letting whichever zone swept last win. */
    if ((s_ct_derive.derived_mask & (1u << ch)) != 0 && s_ct_derive.zone_for_ch[ch] != zi) {
        s_ct_derive.derived_mask &= (uint8_t)~(1u << ch);
        s_ct_derive.conflict_mask |= (uint8_t)(1u << ch);
        return;
    }
    if ((s_ct_derive.conflict_mask & (1u << ch)) != 0) {
        return; /* already disqualified by an earlier zone -- a third claimant changes nothing */
    }
    s_ct_derive.zone_for_ch[ch] = zi;
    s_ct_derive.derived_mask |= (uint8_t)(1u << ch);
    /* M12b: this zone's contribution to the whole-kiln total. Added only on
     * the unambiguous path -- a zone whose channel could not be resolved is
     * a hole in the total, and zone_sweep_push_k_ct_v_per_a() refuses to
     * calibrate from an incomplete one rather than under-counting the kiln
     * and scaling k_ct down to match. */
    s_ct_derive.measured_total_a += per_ch_avg_a[ch];
}

/* The wire id of ct_channel_map[ch] -- one place, so the staging, the
 * unstaging and the read-back can never drift apart. */
#define ZONE_CT_MAP_PARAM_ID(ch) ((uint16_t)(0x0106u + (ch)))

/* config_store.c seeds an uncommissioned record's ct_channel_map[] with
 * 0xFF, "a visibly implausible relay/zone id" -- S14 compares
 * `relay_now_mask & (1u << ct_channel_map[ch])`, and no relay bit 255
 * exists, so a channel left holding this can never point the over-current
 * guard at somebody else's relay. It is the only value this file is allowed
 * to invent, and only ever as a repair for a channel that had no committed
 * value to restore. */
#define ZONE_CT_MAP_IMPLAUSIBLE 0xFFu

/* The value the Pico has actually COMMITTED for ct_channel_map[ch],
 * according to the ESP's cache of its record. False when the cache has never
 * seen that field set (an uncommissioned board), in which case there is no
 * prior value to restore. */
static bool zone_ct_map_committed_value(uint8_t ch, uint8_t *out)
{
    size_t count = safety_cfg_store_param_count();
    for (size_t i = 0; i < count; i++) {
        safety_cfg_param_t row;
        memset(&row, 0, sizeof(row));
        if (!safety_cfg_store_get_by_index(i, &row) || row.param_id != ZONE_CT_MAP_PARAM_ID(ch)) {
            continue;
        }
        if (!row.set) {
            return false;
        }
        if (out) {
            *out = row.value.u8_val;
        }
        return true;
    }
    return false;
}

/* H3 fix (opus review, 2026-08-28): a SET_PARAM that fails PART WAY through
 * the three-channel push used to just `break`, which leaves the Pico's
 * link_task.c s_staged_config holding whatever channels DID stage. That
 * buffer is a persistent baseline -- seeded once from the committed record
 * and only re-seeded by a SUCCESSFUL COMMIT_CONFIG or a reboot
 * (link_task.c's s_staged_config_init, reset only at task start) -- so the
 * next unrelated COMMIT_CONFIG, e.g. the operator saving one field on the
 * commissioning page, would have carried this abandoned sweep's leftover
 * ct_channel_map[] into flash as though it had been commissioned.
 *
 * The link protocol has no "discard staged config" command (uart_task_ids.h
 * enumerates every subcommand on this wire; SET_CONFIG/SET_CT_CAL/COMMIT are
 * all it offers for the config record), and inventing a new wire command for
 * a failure path is not worth a protocol change. So the leftovers are
 * OVERWRITTEN instead: each channel that staged is re-staged back to the
 * value the Pico has actually committed, and, for a channel that has never
 * been committed at all, to config_store.c's own 0xFF placeholder. Either
 * way the staged buffer ends up holding a record that is safe to commit --
 * which is the only property that matters, since this code cannot stop
 * somebody else committing it.
 *
 * Best-effort by construction: this runs because the link already failed
 * once. A repair send that fails too is reported in `note` rather than
 * retried -- the caller has already decided this sweep derived nothing. */
static void zone_sweep_unstage_ct_channels(uint8_t staged_mask, char *note, size_t note_cap)
{
    if (staged_mask == 0 || !s_hw_safety) {
        return;
    }
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        if ((staged_mask & (1u << c)) == 0) {
            continue;
        }
        uint8_t restore = ZONE_CT_MAP_IMPLAUSIBLE;
        (void)zone_ct_map_committed_value(c, &restore);
        kilnlink_param_value_t v;
        memset(&v, 0, sizeof(v));
        v.u8_val = restore;
        if (safety_link_send_set_param(s_hw_safety, ZONE_CT_MAP_PARAM_ID(c),
                                       KILNLINK_PARAM_TYPE_U8, v) != ESP_OK) {
            snprintf(note, note_cap,
                     "CT map staging failed and could NOT be backed out -- re-run the sweep "
                     "before saving again");
            return;
        }
    }
}

/* H1 fix (opus review, 2026-08-28): an ACKed, un-rejected COMMIT_CONFIG is
 * NOT proof the Pico stored anything -- SET_PARAM/COMMIT_CONFIG are both
 * fire-and-forget broadcasts and a late REJECTED frame can miss the reply
 * window, which is exactly why safety_cfg_http.c's confirm_commit_landed()
 * exists for the hand-typed path. That function is static to its own file
 * (and takes safety_cfg_post_pair_t text pairs this caller has none of), so
 * its VERIFICATION is reproduced here rather than shared: force a live
 * re-fetch of the Pico's record -- never the ESP's own cache as it stands --
 * and require every channel this push claims to have written to read back
 * exactly the zone index that was sent. Anything else (unreadable, unset, or
 * set to something different) is reported as unconfirmed, and the caller
 * then reports the map as NOT derived, so the field falls back to manual
 * entry exactly as it does when no sweep has run. */
static bool zone_sweep_confirm_ct_map_landed(uint8_t mask, char *reason, size_t reason_cap)
{
    uint16_t best_known_crc = 0;
    bool peer_known = false;
    (void)safety_link_get_peer_build_status(s_hw_safety, &peer_known, NULL, NULL, NULL, NULL, NULL,
                                             NULL, &best_known_crc);
    if (!safety_cfg_store_refetch(s_hw_safety, peer_known ? best_known_crc : 0)) {
        snprintf(reason, reason_cap,
                 "the CT map commit could not be read back to confirm it -- treated as "
                 "NOT written");
        return false;
    }
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        if ((mask & (1u << c)) == 0) {
            continue;
        }
        uint8_t committed = 0;
        if (!zone_ct_map_committed_value(c, &committed) || committed != s_ct_derive.zone_for_ch[c]) {
            snprintf(reason, reason_cap,
                     "ct_channel_map[%u] does not read back as zone %u -- treated as "
                     "NOT written",
                     (unsigned)c, (unsigned)s_ct_derive.zone_for_ch[c]);
            return false;
        }
    }
    return true;
}

/* Stages every channel this run resolved as SET_PARAM 0x0106+ch (u8, the
 * zone index) and commits, exactly the path safety_cfg_http.c's apply_pairs()
 * uses for a hand-typed value -- the Pico cannot and must not tell the
 * difference between a derived write and a typed one. The Pico's own
 * config_params_finalize_ct_channel_map() runs inside its COMMIT_CONFIG
 * handler (link_task.c) and is what marks the group commissioned once all
 * three per-channel bits are present, so there is deliberately nothing here
 * that waits for a complete triple before sending: a partial derivation
 * stages the channels it actually confirmed and leaves the group bit unset,
 * which is exactly what that function already does with two of three.
 *
 * Only ever called after the sweep's relays are off and the run has ended --
 * safety_link_send_set_param()/_commit_config() both block for a link round
 * trip, and this must not sit inside a window where a relay is energized. */
static void zone_sweep_push_ct_channel_map(void)
{
    char note[sizeof(s_sweep.ct_map_reason)];
    note[0] = '\0';
    /* Scoped to the whole function, not just the staging loop below: every
     * failure arm past the loop (commit unacked, commit rejected, commit
     * acked but the read-back doesn't confirm it landed) still needs to know
     * which channels made it into the Pico's staged buffer, so it can back
     * them out the same way a staging failure already does -- see H3's
     * comment on zone_sweep_unstage_ct_channels() above. Left at 0 (a no-op
     * for that function) on every path that never reaches the loop. */
    uint8_t staged_mask = 0;

    if (s_ct_derive.derived_mask == 0) {
        /* LOW (opus review): in summed topology this branch fires on EVERY
         * run, by design -- zone_sweep_task_record_ct_channels() never even
         * attempts the one-relay-one-channel derivation there (a single
         * shared CT has no per-relay mapping to find). The per_zone wording
         * ("no CT channel could be identified") reads as a failed attempt,
         * which misleads an operator on a summed board where nothing was
         * ever attempted. */
        if (s_ct_topology_summed) {
            snprintf(note, sizeof(note), "summed CT topology has no per-zone channel map to derive -- not applicable");
        } else {
            snprintf(note, sizeof(note), "no CT channel could be identified -- map not changed");
        }
    } else if (!s_hw_safety) {
        snprintf(note, sizeof(note), "safety link not available -- CT map not written");
        s_ct_derive.derived_mask = 0;
        /* Nothing staged, so nothing to back out -- but there is also no
         * link for the k_ct push to use, and its own !s_hw_safety arm says
         * so. Not flagged as a push FAILURE: the staged buffer is untouched. */
    } else {
        for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
            if ((s_ct_derive.derived_mask & (1u << c)) == 0) {
                continue;
            }
            kilnlink_param_value_t v;
            memset(&v, 0, sizeof(v));
            v.u8_val = s_ct_derive.zone_for_ch[c];
            esp_err_t err = safety_link_send_set_param(s_hw_safety, ZONE_CT_MAP_PARAM_ID(c),
                                                       KILNLINK_PARAM_TYPE_U8, v);
            if (err != ESP_OK) {
                /* %.24s, not a bare %s, for the same reason the ambiguity
                 * note below uses one -- see its comment. */
                snprintf(note, sizeof(note), "staging ct_channel_map[%u] failed: %.24s", c,
                         esp_err_to_name(err));
                /* H3: whatever already staged must not be left sitting in the
                 * Pico's staged record for an unrelated commit to pick up. */
                zone_sweep_unstage_ct_channels(staged_mask, note, sizeof(note));
                s_ct_derive.derived_mask = 0;
                s_ct_derive.map_push_failed = true;
                break;
            }
            staged_mask |= (uint8_t)(1u << c);
        }
    }

    if (s_ct_derive.derived_mask != 0) {
        uint16_t reject_param_id = 0;
        uint8_t reject_reason = 0;
        bool rejected = false;
        esp_err_t err = safety_link_send_commit_config(s_hw_safety, &reject_param_id, &reject_reason,
                                                        &rejected);
        if (err != ESP_OK) {
            snprintf(note, sizeof(note), "CT map staged but the commit was not "
                                          "acknowledged (%.24s)", esp_err_to_name(err));
            /* An unacked commit's fate on the Pico is unknown -- it may not
             * have applied, in which case the staged buffer this loop wrote
             * is still sitting there for an unrelated later commit to pick
             * up. Same H3 exposure as a staging-loop failure, so the same
             * repair. */
            zone_sweep_unstage_ct_channels(staged_mask, note, sizeof(note));
            s_ct_derive.derived_mask = 0;
            s_ct_derive.map_push_failed = true;
        } else if (rejected) {
            /* Expected and legitimate when the relay is ARMED -- config
             * writes are refused outright then (CONFIG_REFERENCE.md). Say so
             * rather than leaving the operator to infer it from a map that
             * silently did not change. A rejected commit leaves the staged
             * buffer exactly as staged (rejection means nothing was
             * applied), so it still needs backing out. */
            snprintf(note, sizeof(note), "the safety processor rejected the CT map commit "
                                          "(id 0x%04X, reason %u)", (unsigned)reject_param_id,
                     (unsigned)reject_reason);
            zone_sweep_unstage_ct_channels(staged_mask, note, sizeof(note));
            s_ct_derive.derived_mask = 0;
            s_ct_derive.map_push_failed = true;
        } else if (!zone_sweep_confirm_ct_map_landed(s_ct_derive.derived_mask, note, sizeof(note))) {
            /* H1: ACKed and not rejected is not proof. Nothing is persisted
             * and no channel is reported as derived -- the operator sees the
             * reason and enters the map by hand, exactly as they would if the
             * sweep had never run. The read-back disagreeing is exactly the
             * case H3 exists for too: back the staged buffer out rather than
             * leave a value known not to match what was intended sitting
             * there for the next commit. */
            zone_sweep_unstage_ct_channels(staged_mask, note, sizeof(note));
            s_ct_derive.derived_mask = 0;
            s_ct_derive.map_push_failed = true;
        } else {
            for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
                if ((s_ct_derive.derived_mask & (1u << c)) != 0) {
                    zone_ct_map_set(c, s_ct_derive.zone_for_ch[c]);
                }
            }
        }
    }

    /* An ambiguity note never overwrites a hard failure above -- the failure
     * is the more actionable of the two -- but it is reported whenever the
     * write itself was fine and some zone still could not be resolved. */
    if (note[0] == '\0' && (s_ct_derive.unresolved_zone_mask != 0 || s_ct_derive.conflict_mask != 0)) {
        /* %.24s, not a bare %s, so -Werror=format-truncation can prove this
         * fits note[] whatever the two literals grow into later. */
        snprintf(note, sizeof(note), "CT map incomplete (%.24s) -- enter the rest by hand",
                 s_ct_derive.conflict_mask != 0 ? "two zones share one CT" : "a zone was ambiguous");
    }

    s_sweep.ct_map_derived_mask = s_ct_derive.derived_mask;
    strncpy((char *)s_sweep.ct_map_reason, note, sizeof(s_sweep.ct_map_reason) - 1);
    s_sweep.ct_map_reason[sizeof(s_sweep.ct_map_reason) - 1] = '\0';
}

/* ---- M12b: pushing the derived k_ct_v_per_a ------------------------------
 * Deliberately a MIRROR of the ct_channel_map push above rather than a
 * generalisation of it: the two share a shape (stage / commit / read-back /
 * back out) but not a single decision -- different param ids, a different
 * wire type, a different restore placeholder, a different definition of
 * "landed", and a completely different rule for what may be derived at all.
 * Folding them into one parameterised routine would mean every future change
 * to one has to be argued for the other, which is exactly the coupling the
 * CT-map push's own H1/H3 comments were written to avoid. */
#define ZONE_KCT_PARAM_ID(ch) ((uint16_t)(0x0308u + (ch)))

/* The two commissioning answers this calibration reads (COMMISSIONING_UX.md
 * sec 2, Q3 and Q4). Both are F32 on the wire. */
#define ZONE_MAINS_VOLTAGE_PARAM_ID  ((uint16_t)0x030Eu)
#define ZONE_MAX_POWER_PARAM_ID      ((uint16_t)0x0319u)

/* config_store.c leaves k_ct_v_per_a at its memset(0) -- the uncommissioned
 * value (that file's own comment at the seed). Unlike ct_channel_map's 0xFF,
 * 0.0f is not merely implausible but actively meaningful downstream:
 * current_presence_policy.c branches on k_ct_v_per_a <= 0.0f and falls back
 * to its deliberately sensitive counts-domain margin. Restoring 0.0f for a
 * channel that has never been committed therefore puts the Pico back on the
 * SAFE side of that branch, which is the right direction for a repair. */
#define ZONE_KCT_UNCOMMISSIONED 0.0f

/* The F32 value the Pico has actually COMMITTED for `param_id`, according to
 * the ESP's cache of its record -- the f32 twin of
 * zone_ct_map_committed_value() above, and it answers false in the same two
 * cases (no such row, or a row the Pico has never had set). */
static bool zone_cfg_committed_f32(uint16_t param_id, float *out)
{
    size_t count = safety_cfg_store_param_count();
    for (size_t i = 0; i < count; i++) {
        safety_cfg_param_t row;
        memset(&row, 0, sizeof(row));
        if (!safety_cfg_store_get_by_index(i, &row) || row.param_id != param_id) {
            continue;
        }
        if (!row.set) {
            return false;
        }
        if (out) {
            *out = row.value.f32_val;
        }
        return true;
    }
    return false;
}

/* H1's verification, for this push. An ACKed, un-rejected COMMIT_CONFIG is
 * not proof anything was stored (zone_sweep_confirm_ct_map_landed()'s
 * comment) -- force a LIVE re-fetch and require every channel written to
 * read back as exactly the float that was sent. Exact equality is right
 * here, not a tolerance: the wire, config_store's record and this cache all
 * carry the identical IEEE-754 f32, so anything other than bit-equality
 * means the value did not land, not that it landed imprecisely.
 *
 * Forward-declared: zone_sweep_unstage_k_ct() below (the restore path) needs
 * to call this SAME check against its own restore values -- see that
 * function's comment for why a restore that only stages and never commits
 * is exactly the defect this reuse closes. */
static bool zone_sweep_confirm_k_ct_landed(uint8_t mask, const float *k_new, char *reason,
                                            size_t reason_cap);

/* H3's repair, for this push -- see zone_sweep_unstage_ct_channels()'s own
 * comment for the full reasoning about why leftovers in the Pico's staged
 * buffer are a real hazard and why overwriting is the only available
 * remedy.
 *
 * Opus review finding 2: this used to SET_PARAM the restore values and stop
 * -- no COMMIT_CONFIG. That is harmless when the push's own COMMIT_CONFIG
 * never landed (rejected, or the send itself failed): nothing was ever
 * committed, so overwriting the Pico's STAGED buffer is enough to stop a
 * later unrelated commit from picking up the stale bytes. But the caller
 * also reaches this function after zone_sweep_confirm_k_ct_landed() fails on
 * an ACKed, un-rejected commit -- meaning the push's COMMIT_CONFIG may well
 * have actually landed. Staging the old value without committing it back
 * leaves the Pico's COMMITTED record holding the new (bad-readback) value
 * while the caller went on to report "not written". This function now
 * commits the restore and verifies it landed the same way the original push
 * did -- but ONLY when `commit_may_have_landed` says the push's own commit
 * might actually have reached the Pico's committed record (an ACKed,
 * un-rejected COMMIT_CONFIG whose read-back then failed to confirm). When
 * the push's commit demonstrably never landed at all (a SET_PARAM failure,
 * a rejected commit, or the commit send itself failing) the Pico's
 * COMMITTED record cannot hold the new value, so overwriting the STAGED
 * buffer is the whole remedy -- committing that restore too would just be
 * an extra round trip with nothing at risk if skipped, and previous
 * behaviour (and its host tests) already relied on no commit happening on
 * that path.
 *
 * `prior` must be the value captured BEFORE this push started staging (the
 * caller's own pre-push snapshot, not a possibly-just-refreshed cache read
 * here) so a restore always targets the true prior state. Returns true once
 * the restore is known-safe (either confirmed committed, or never needed to
 * be); the caller must treat false as "unknown", never as "successfully
 * backed out". */
static bool zone_sweep_unstage_k_ct(uint8_t staged_mask, const float *prior, bool commit_may_have_landed,
                                     char *note, size_t note_cap)
{
    if (staged_mask == 0 || !s_hw_safety) {
        return true;
    }
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        if ((staged_mask & (1u << c)) == 0) {
            continue;
        }
        kilnlink_param_value_t v;
        memset(&v, 0, sizeof(v));
        v.f32_val = prior[c];
        if (safety_link_send_set_param(s_hw_safety, ZONE_KCT_PARAM_ID(c),
                                       KILNLINK_PARAM_TYPE_F32, v) != ESP_OK) {
            snprintf(note, note_cap,
                     "CT scale backout SET_PARAM failed -- UNKNOWN state, re-run the sweep");
            return false;
        }
    }

    if (!commit_may_have_landed) {
        return true;
    }

    uint16_t reject_param_id = 0;
    uint8_t reject_reason = 0;
    bool rejected = false;
    esp_err_t err = safety_link_send_commit_config(s_hw_safety, &reject_param_id, &reject_reason,
                                                    &rejected);
    if (err != ESP_OK || rejected) {
        snprintf(note, note_cap,
                 "CT scale backout commit failed -- UNKNOWN state, re-run the sweep");
        return false;
    }

    char verify_reason[96];
    if (!zone_sweep_confirm_k_ct_landed(staged_mask, prior, verify_reason, sizeof(verify_reason))) {
        snprintf(note, note_cap,
                 "CT scale backout not confirmed -- UNKNOWN state, re-run the sweep");
        return false;
    }
    return true;
}

/* Definition for the forward declaration above zone_sweep_unstage_k_ct(). */
static bool zone_sweep_confirm_k_ct_landed(uint8_t mask, const float *k_new, char *reason,
                                            size_t reason_cap)
{
    uint16_t best_known_crc = 0;
    bool peer_known = false;
    (void)safety_link_get_peer_build_status(s_hw_safety, &peer_known, NULL, NULL, NULL, NULL, NULL,
                                             NULL, &best_known_crc);
    if (!safety_cfg_store_refetch(s_hw_safety, peer_known ? best_known_crc : 0)) {
        snprintf(reason, reason_cap,
                 "the CT scale commit could not be read back to confirm it -- treated as "
                 "NOT written");
        return false;
    }
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        if ((mask & (1u << c)) == 0) {
            continue;
        }
        float committed = 0.0f;
        if (!zone_cfg_committed_f32(ZONE_KCT_PARAM_ID(c), &committed) || committed != k_new[c]) {
            snprintf(reason, reason_cap,
                     "k_ct_v_per_a[%u] does not read back as written -- treated as NOT written",
                     (unsigned)c);
            return false;
        }
    }
    return true;
}

/* Decides what this run may calibrate, ahead of touching the link at all --
 * separated from the staging below so the whole decision (including every
 * refusal) is reachable from a host test without a fake link. Returns the
 * mask of channels to write, filling out_k[] for each, and `note` with the
 * reason whenever that mask comes back 0. */
static uint8_t zone_sweep_plan_k_ct(float *out_k, char *note, size_t note_cap)
{
    if (s_ct_derive.derived_mask == 0) {
        snprintf(note, note_cap, "no CT channel was identified -- CT scale not calibrated");
        return 0;
    }
    /* An incomplete pass cannot be summed into a whole-kiln total: a zone
     * that did not resolve still drew its current, so its absence would drag
     * the measured total down and scale k_ct with it -- silently, and in the
     * direction that makes every later reading read LOW. Same reason the map
     * push refuses to derive anything from a partial run. */
    if (s_ct_derive.unresolved_zone_mask != 0 || s_ct_derive.conflict_mask != 0) {
        /* Kept to 94 chars + NUL so it fits k_ct_reason[96] -- the longer
         * wording this replaced was 111 bytes and broke the build under
         * -Werror=format-truncation, which is the compiler correctly
         * refusing to let a reason string be silently cut in half on the
         * status page. Same meaning, no truncation. */
        snprintf(note, note_cap,
                 "not every zone resolved to a CT -- an incomplete total would scale k_ct low, "
                 "so it was not set");
        return 0;
    }
    if (s_ct_derive.map_push_failed) {
        snprintf(note, note_cap, "the CT map write failed -- CT scale not calibrated");
        return 0;
    }
    float mains_v = 0.0f, power_w = 0.0f;
    if (!zone_cfg_committed_f32(ZONE_MAINS_VOLTAGE_PARAM_ID, &mains_v) ||
        !zone_cfg_committed_f32(ZONE_MAX_POWER_PARAM_ID, &power_w)) {
        /* %.70s, not %.72s: "CT scale not calibrated: " is 25 bytes with its
         * NUL, and both callers pass note_cap==sizeof(s_sweep.{ct_map,k_ct}_
         * reason)==96, so 25 + 70 + 1(NUL) == 96 exactly -- provably fits
         * however long zone_kct_derive_str()'s literals grow, the same
         * reasoning as the %.24s sites above. The old %.72s put that bound at
         * 98, two bytes past the buffer. */
        snprintf(note, note_cap, "CT scale not calibrated: %.70s",
                 zone_kct_derive_str(ZONE_KCT_DERIVE_NO_NAMEPLATE));
        return 0;
    }
    uint8_t plan_mask = 0;
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        if ((s_ct_derive.derived_mask & (1u << c)) == 0) {
            continue;
        }
        /* CT_COMMISSIONING_PLAN.md step 1: "manual wins over the sweep (the
         * sweep must not overwrite a manual value)". A channel the operator
         * hand-entered an A_fs/zero_mv for (safety_cfg_store's ct_cal
         * record, source MANUAL) is skipped here entirely -- the sweep's own
         * derivation for it is simply never computed or staged, same as an
         * unresolved channel. safety_cfg_store_set_ct_cal_input() itself
         * refuses the mirror-image write (a SWEEP-sourced write attempted
         * against a MANUAL channel), but that refusal never runs at all
         * because this loop is what would have produced it. */
        float existing_a_fs = 0.0f, existing_zero_mv = 0.0f;
        safety_ct_cal_source_t existing_source = SAFETY_CT_CAL_SOURCE_SWEEP;
        if (safety_cfg_store_get_ct_cal_input(c, &existing_a_fs, &existing_zero_mv, &existing_source) &&
            existing_source == SAFETY_CT_CAL_SOURCE_MANUAL) {
            continue;
        }
        float k_old = 0.0f;
        if (!zone_cfg_committed_f32(ZONE_KCT_PARAM_ID(c), &k_old)) {
            k_old = 0.0f; /* never committed -- zone_sweep_derive_k_ct() refuses on it */
        }
        float k_new = 0.0f;
        zone_kct_derive_t r =
            zone_sweep_derive_k_ct(s_ct_derive.measured_total_a, power_w, mains_v, k_old, &k_new);
        if (r != ZONE_KCT_DERIVE_OK) {
            /* One channel's refusal ends the whole calibration rather than
             * calibrating the others: the scale factor is a property of the
             * measurement, not of a channel, so a channel that cannot take
             * it means this run's own inputs are unusable. Writing the rest
             * would leave the three channels on different scales with
             * nothing recording that they disagree. */
            /* Same %.72s bound as the nameplate-missing site above. */
            snprintf(note, note_cap, "CT scale not calibrated: %.70s", zone_kct_derive_str(r));
            return 0;
        }
        out_k[c] = k_new;
        plan_mask |= (uint8_t)(1u << c);
    }
    return plan_mask;
}

/* Stages every planned channel as SET_PARAM 0x0308+c (f32) and commits --
 * the same path a hand-typed value takes through safety_cfg_http.c, since
 * the Pico must not be able to tell a derived write from a typed one.
 *
 * Called only from zone_sweep_task(), immediately after
 * zone_sweep_push_ct_channel_map(), with every relay already off: both
 * senders block for a link round trip and neither may sit inside a window
 * where an element is energized. */
static void zone_sweep_push_k_ct_v_per_a(void)
{
    char note[sizeof(s_sweep.k_ct_reason)];
    note[0] = '\0';
    float k_new[ZONE_CT_CHANNEL_COUNT] = {0};
    float prior_k[ZONE_CT_CHANNEL_COUNT] = {0};
    uint8_t staged_mask = 0;

    uint8_t plan_mask = zone_sweep_plan_k_ct(k_new, note, sizeof(note));
    if (plan_mask != 0 && !s_hw_safety) {
        snprintf(note, sizeof(note), "safety link not available -- CT scale not written");
        plan_mask = 0;
    }

    if (plan_mask != 0) {
        for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
            if ((plan_mask & (1u << c)) == 0) {
                continue;
            }
            /* Prior value, captured BEFORE staging -- see
             * zone_sweep_unstage_k_ct()'s comment for why a restore must
             * target this snapshot rather than a cache that may already
             * have been refreshed by a failed confirm's own refetch.
             * Defaults to ZONE_KCT_UNCOMMISSIONED (never committed). */
            prior_k[c] = ZONE_KCT_UNCOMMISSIONED;
            (void)zone_cfg_committed_f32(ZONE_KCT_PARAM_ID(c), &prior_k[c]);
            kilnlink_param_value_t v;
            memset(&v, 0, sizeof(v));
            v.f32_val = k_new[c];
            esp_err_t err = safety_link_send_set_param(s_hw_safety, ZONE_KCT_PARAM_ID(c),
                                                       KILNLINK_PARAM_TYPE_F32, v);
            if (err != ESP_OK) {
                snprintf(note, sizeof(note), "staging k_ct_v_per_a[%u] failed: %.24s", c,
                         esp_err_to_name(err));
                (void)zone_sweep_unstage_k_ct(staged_mask, prior_k, false, note, sizeof(note));
                plan_mask = 0;
                break;
            }
            staged_mask |= (uint8_t)(1u << c);
        }
    }

    if (plan_mask != 0) {
        uint16_t reject_param_id = 0;
        uint8_t reject_reason = 0;
        bool rejected = false;
        esp_err_t err = safety_link_send_commit_config(s_hw_safety, &reject_param_id, &reject_reason,
                                                        &rejected);
        if (err != ESP_OK) {
            snprintf(note, sizeof(note), "CT scale staged but the commit was not "
                                          "acknowledged (%.24s)", esp_err_to_name(err));
            (void)zone_sweep_unstage_k_ct(staged_mask, prior_k, false, note, sizeof(note));
            plan_mask = 0;
        } else if (rejected) {
            snprintf(note, sizeof(note), "the safety processor rejected the CT scale commit "
                                          "(id 0x%04X, reason %u)", (unsigned)reject_param_id,
                     (unsigned)reject_reason);
            (void)zone_sweep_unstage_k_ct(staged_mask, prior_k, false, note, sizeof(note));
            plan_mask = 0;
        } else if (!zone_sweep_confirm_k_ct_landed(plan_mask, k_new, note, sizeof(note))) {
            /* The commit above was ACKed and un-rejected -- it may actually
             * have landed on the Pico despite the read-back failure. The
             * restore below must itself be committed and verified, or this
             * note must say UNKNOWN rather than "not written" -- see
             * zone_sweep_unstage_k_ct()'s comment. */
            (void)zone_sweep_unstage_k_ct(staged_mask, prior_k, true, note, sizeof(note));
            plan_mask = 0;
        } else {
            for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
                if ((plan_mask & (1u << c)) != 0) {
                    (void)zone_k_ct_set(c, k_new[c]);
                }
            }
        }
    }

    s_sweep.k_ct_derived_mask = plan_mask;
    strncpy((char *)s_sweep.k_ct_reason, note, sizeof(s_sweep.k_ct_reason) - 1);
    s_sweep.k_ct_reason[sizeof(s_sweep.k_ct_reason) - 1] = '\0';
}

/* ---- Feature: nameplate current -> S14/S15 arming --------------------------
 * safety_guards.c's S14 (per-channel over-current) and S15 (per-zone
 * under-current, summed topology) both key entirely off
 * cfg->i_normal_a[0..2]/i_normal_valid[0..2] on the SAFETY PROCESSOR -- see
 * that file's own S14/S15 block. Before this, nothing ever wrote those
 * params: zone_normals_set() (called from
 * zone_sweep_task_record_normal()/zone_sweep_task_record_ct_channels() above)
 * persists a zone's measured normal current only in the ESP's own NVS
 * (zones_config_get_normal_current()) -- the Pico never sees it, so
 * i_normal_valid[] stays false forever and both guards report "inactive"
 * even on a board that has completed a full, successful current sweep. That
 * is exactly this bench's situation. This function is the missing push,
 * built on the identical stage/COMMIT_CONFIG/verify pattern
 * zone_sweep_push_k_ct_v_per_a() above already uses for the same safety
 * processor -- deliberately AFTER that call (never before/interleaved, same
 * reasoning: both stage into the one Pico-side config buffer and each ends
 * its own COMMIT_CONFIG transaction). */

#define ZONE_INORMAL_PARAM_ID(zi) ((uint16_t)(0x031Au + (zi)))

/* Pure planning decision: which zones have an already-measured, still-
 * plausible normal current worth pushing, and what value. Host-testable
 * without a fake link -- every input is the ESP's own persisted store.
 *
 * Deliberately NOT limited to zones measured in the run that just finished:
 * zones_config_get_normal_current() is the durable record (survives a
 * reboot), and a zone skipped this run (relay_mask == 0, or this run only
 * re-measured a subset) still has a previously-measured normal that keeps
 * its own S14/S15 armed -- there is no reason to let a partial re-sweep
 * un-arm a zone it did not touch. A zone with no measurement on record
 * (never swept) is left unplanned -- exactly the "leave the guard dormant
 * rather than fabricate a threshold" rule the owner asked for: this
 * function never invents a value, it only relays one this board already
 * measured. `out_a` must have MAX31856_CHANNEL_COUNT entries. */
uint8_t zone_sweep_plan_i_normal(float *out_a, char *note, size_t note_cap)
{
    uint8_t plan_mask = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        float amps = 0.0f;
        bool measured = false;
        if (!zones_config_get_normal_current(zi, &amps, &measured) || !measured) {
            continue;
        }
        if (!isfinite(amps) || amps <= 0.0f) {
            /* zones_config_get_normal_current()'s own contract already
             * refuses to persist a non-finite/non-positive value (see
             * zone_normals_set()), so this is defence in depth, not the
             * expected path -- never push a guard threshold this function
             * cannot vouch for. */
            continue;
        }
        out_a[zi] = amps;
        plan_mask |= (uint8_t)(1u << zi);
    }
    if (plan_mask == 0 && note) {
        snprintf(note, note_cap, "no zone has a measured normal current yet -- S14/S15 stay dormant");
    }
    return plan_mask;
}

/* Forward-declared: zone_sweep_unstage_i_normal() below needs to call this
 * SAME check against its own restore values -- see that function's comment. */
static bool zone_sweep_confirm_i_normal_landed(uint8_t mask, const float *planned_a, char *reason,
                                                size_t reason_cap);

/* H3-style repair for this push -- mirrors zone_sweep_unstage_k_ct() above,
 * including its opus-review fix: staging the restore values alone is not
 * enough once the push's own COMMIT_CONFIG may have actually landed (ACKed,
 * un-rejected, only the read-back failed) -- the restore must be committed
 * and verified too, or the caller must be told the outcome is UNKNOWN, never
 * silently reported as "not written" while the Pico may hold the new value.
 *
 * `prior` holds whatever the Pico had committed for each planned zone BEFORE
 * this attempt started staging (captured by the caller, since once staging
 * begins the ESP's own cache of "committed" is stale until the next
 * refetch); a zone with no prior commit restores to 0.0f/unset, matching
 * safety_guards.c reading i_normal_valid[] false the same way it does for a
 * board that was never swept at all -- the safe direction for a repair.
 *
 * `commit_may_have_landed`, same split as zone_sweep_unstage_k_ct(): only
 * true when the push's own COMMIT_CONFIG was ACKed and un-rejected and the
 * read-back is what failed -- the only case where the Pico's COMMITTED
 * record (not just its staging buffer) might hold the new value. The other
 * failure paths (a SET_PARAM failure, a rejected commit, an unacknowledged
 * commit) never got that far, so restaging without committing is already
 * the whole remedy there, matching prior behaviour and its host tests.
 *
 * Returns true once the restore is known-safe (confirmed committed, or
 * never needed to be). */
static bool zone_sweep_unstage_i_normal(uint8_t staged_mask, const float *prior, bool commit_may_have_landed,
                                         char *note, size_t note_cap)
{
    if (staged_mask == 0 || !s_hw_safety) {
        return true;
    }
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if ((staged_mask & (1u << zi)) == 0) {
            continue;
        }
        kilnlink_param_value_t v;
        memset(&v, 0, sizeof(v));
        v.f32_val = prior ? prior[zi] : 0.0f;
        if (safety_link_send_set_param(s_hw_safety, ZONE_INORMAL_PARAM_ID(zi),
                                       KILNLINK_PARAM_TYPE_F32, v) != ESP_OK) {
            snprintf(note, note_cap,
                     "i_normal_a backout SET_PARAM failed -- UNKNOWN state, re-run the sweep");
            return false;
        }
    }

    if (!commit_may_have_landed) {
        return true;
    }

    uint16_t reject_param_id = 0;
    uint8_t reject_reason = 0;
    bool rejected = false;
    esp_err_t err = safety_link_send_commit_config(s_hw_safety, &reject_param_id, &reject_reason,
                                                    &rejected);
    if (err != ESP_OK || rejected) {
        snprintf(note, note_cap,
                 "i_normal_a backout commit failed -- UNKNOWN state, re-run the sweep");
        return false;
    }

    char verify_reason[96];
    if (!zone_sweep_confirm_i_normal_landed(staged_mask, prior, verify_reason, sizeof(verify_reason))) {
        snprintf(note, note_cap,
                 "i_normal_a backout not confirmed -- UNKNOWN state, re-run the sweep");
        return false;
    }
    return true;
}

/* H1-style verification for this push -- mirrors
 * zone_sweep_confirm_k_ct_landed() above, same "an ACKed commit is not proof
 * anything was stored" reasoning and the same exact-bit-equality check (both
 * sides carry the identical IEEE-754 f32). */
static bool zone_sweep_confirm_i_normal_landed(uint8_t mask, const float *planned_a, char *reason,
                                                size_t reason_cap)
{
    uint16_t best_known_crc = 0;
    bool peer_known = false;
    (void)safety_link_get_peer_build_status(s_hw_safety, &peer_known, NULL, NULL, NULL, NULL, NULL,
                                             NULL, &best_known_crc);
    if (!safety_cfg_store_refetch(s_hw_safety, peer_known ? best_known_crc : 0)) {
        snprintf(reason, reason_cap,
                 "the i_normal_a commit could not be read back to confirm it -- treated as "
                 "NOT written");
        return false;
    }
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if ((mask & (1u << zi)) == 0) {
            continue;
        }
        float committed = 0.0f;
        if (!zone_cfg_committed_f32(ZONE_INORMAL_PARAM_ID(zi), &committed) || committed != planned_a[zi]) {
            /* Same "%u is at most 3 digits, prove the fixed text fits reason_cap
             * (96)" discipline as zone_sweep_run_all_zones()'s ENERGIZE_REFUSED
             * arm -- the longer wording this replaced (naming S14/S15
             * explicitly) measured 106 bytes against reason_cap==96 and broke
             * -Werror=format-truncation. Same meaning, shorter words. */
            snprintf(reason, reason_cap,
                     "i_normal_a[%u] does not read back as written -- treated as NOT written",
                     (unsigned)zi);
            return false;
        }
    }
    return true;
}

/* Called only from zone_sweep_task(), immediately after
 * zone_sweep_push_k_ct_v_per_a(), with every relay already off -- same
 * calling convention as that function.
 *
 * NOTE (constraint check, owner-directed): this stages SET_PARAM + one
 * COMMIT_CONFIG, the exact path safety_cfg_http.c's hand-typed-value POST
 * already uses -- no new wire message and no KILNLINK_PROTOCOL_VERSION or
 * ZONES_CFG_VERSION bump. It is subject to the SAME "Pico refuses config
 * writes while relay_owner is ARMED, only in the post-reset grace window"
 * rule as every other push in this file (zone_sweep_push_ct_channel_map(),
 * zone_sweep_push_k_ct_v_per_a()) -- a rejected/unacknowledged commit here
 * reads exactly like those do (rejected/timeout note, unstaged, mask stays
 * 0), it is not a new failure mode this function introduces. */
static void zone_sweep_push_i_normal_a(void)
{
    char note[sizeof(s_sweep.i_normal_reason)];
    note[0] = '\0';
    float planned_a[MAX31856_CHANNEL_COUNT] = {0};
    float prior_a[MAX31856_CHANNEL_COUNT] = {0};
    uint8_t staged_mask = 0;

    uint8_t plan_mask = zone_sweep_plan_i_normal(planned_a, note, sizeof(note));
    if (plan_mask != 0 && !s_hw_safety) {
        snprintf(note, sizeof(note), "safety link not available -- i_normal_a not written");
        plan_mask = 0;
    }

    if (plan_mask != 0) {
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if ((plan_mask & (1u << zi)) == 0) {
                continue;
            }
            (void)zone_cfg_committed_f32(ZONE_INORMAL_PARAM_ID(zi), &prior_a[zi]); /* 0.0f if never committed */
            kilnlink_param_value_t v;
            memset(&v, 0, sizeof(v));
            v.f32_val = planned_a[zi];
            esp_err_t err = safety_link_send_set_param(s_hw_safety, ZONE_INORMAL_PARAM_ID(zi),
                                                       KILNLINK_PARAM_TYPE_F32, v);
            if (err != ESP_OK) {
                snprintf(note, sizeof(note), "staging i_normal_a[%u] failed: %.24s", zi,
                         esp_err_to_name(err));
                (void)zone_sweep_unstage_i_normal(staged_mask, prior_a, false, note, sizeof(note));
                plan_mask = 0;
                break;
            }
            staged_mask |= (uint8_t)(1u << zi);
        }
    }

    if (plan_mask != 0) {
        uint16_t reject_param_id = 0;
        uint8_t reject_reason = 0;
        bool rejected = false;
        esp_err_t err = safety_link_send_commit_config(s_hw_safety, &reject_param_id, &reject_reason,
                                                        &rejected);
        if (err != ESP_OK) {
            snprintf(note, sizeof(note), "i_normal_a staged but the commit was not "
                                          "acknowledged (%.24s)", esp_err_to_name(err));
            (void)zone_sweep_unstage_i_normal(staged_mask, prior_a, false, note, sizeof(note));
            plan_mask = 0;
        } else if (rejected) {
            snprintf(note, sizeof(note), "the safety processor rejected the i_normal_a commit "
                                          "(id 0x%04X, reason %u)", (unsigned)reject_param_id,
                     (unsigned)reject_reason);
            (void)zone_sweep_unstage_i_normal(staged_mask, prior_a, false, note, sizeof(note));
            plan_mask = 0;
        } else if (!zone_sweep_confirm_i_normal_landed(plan_mask, planned_a, note, sizeof(note))) {
            /* The commit above was ACKed and un-rejected -- it may actually
             * have landed on the Pico despite the read-back failure. */
            (void)zone_sweep_unstage_i_normal(staged_mask, prior_a, true, note, sizeof(note));
            plan_mask = 0;
        } else {
            note[0] = '\0';
        }
    }

    s_sweep.i_normal_pushed_mask = plan_mask;
    strncpy((char *)s_sweep.i_normal_reason, note, sizeof(s_sweep.i_normal_reason) - 1);
    s_sweep.i_normal_reason[sizeof(s_sweep.i_normal_reason) - 1] = '\0';
}

static void zone_sweep_task_zone_done(void *ctx)
{
    (void)ctx;
    s_sweep.zones_done++; /* live -- visible to a status poll while the sweep is still running */
}

/* Background task body -- the only place this module ever commands a relay
 * ON (via zone_sweep_run_one_zone()'s deps->energize, called from
 * zone_sweep_run_all_zones() above). One zone at a time is structural, not a
 * convention: zone_sweep_run_all_zones() runs exactly one zone per iteration
 * and that call's own choke point (zone_sweep_force_relays_off()) drops
 * EVERY relay before ever starting the next iteration or exiting -- there is
 * no code path in this function that can have two zones' relays on at once,
 * and test_zone_sweep_run_all_zones_never_energizes_two_zones_at_once()
 * (M3, opus review 2026-08-28) proves it against this exact function rather
 * than only arguing it from reading the code. */
static void zone_sweep_task(void *arg)
{
    (void)arg;
    static const zone_sweep_zone_deps_t hw_deps = {
        .energize = zone_sweep_hw_energize,
        .force_off = zone_sweep_hw_force_off,
        .read_temp = zone_sweep_hw_read_temp,
        .sample_current = zone_sweep_hw_sample_current,
        .sample_channels = zone_sweep_hw_sample_channels,
        .link_up = zone_sweep_hw_link_up,
        .trip_latched = zone_sweep_hw_trip_latched,
        .abort_requested = zone_sweep_hw_abort_requested,
        .delay_poll = zone_sweep_hw_delay_poll,
        .ctx = NULL,
    };
    static const zone_sweep_all_hooks_t hw_hooks = {
        .relay_mask_for_zone = zone_sweep_task_relay_mask_for_zone,
        .set_zone_index = zone_sweep_task_set_zone_index,
        .record_normal = zone_sweep_task_record_normal,
        .record_ct_channels = zone_sweep_task_record_ct_channels,
        .zone_done = zone_sweep_task_zone_done,
        .ctx = NULL,
    };

    memset(&s_ct_derive, 0, sizeof(s_ct_derive));
    zone_ct_map_clear(); /* a re-sweep must not leave a stale channel claim visible as current */
    zone_k_ct_clear();   /* M12b: same reasoning, for the derived CT scale */

    /* CT_COMMISSIONING_PLAN.md step 3 -- read fresh every run, never cached
     * across sweeps. zones_current_sweep_start() already refused to start
     * with any relay on (zone_sweep_check_refusal()'s RELAYS_ON case), so
     * this sample is genuinely an idle baseline. */
    s_ct_topology_summed = (zone_cfg_committed_ct_topology() != 0u);
    s_ct_summed_idle_a = 0.0f;
    if (s_ct_topology_summed) {
        float idle_ch[ZONE_CT_CHANNEL_COUNT];
        zone_sweep_hw_sample_channels(NULL, idle_ch);
        float idle = idle_ch[ZONE_CT_CHANNEL_COUNT - 1];
        s_ct_summed_idle_a = isnan(idle) ? 0.0f : idle;
    }

    zone_sweep_all_result_t result;
    zone_sweep_run_all_zones(s_sweep.zones_total, &hw_deps, &hw_hooks, &result);

    /* DELIBERATELY not `s_sweep.state = result.state` for a successful run:
     * DONE is published below, only once zone_sweep_push_ct_channel_map()
     * has finished (see that call's comment). A failed/aborted run publishes
     * immediately -- it derives nothing, so there is nothing to wait for. */
    if (result.state != ZONE_SWEEP_DONE) {
        s_sweep.state = result.state;
    }
    strncpy((char *)s_sweep.reason, result.reason, sizeof(s_sweep.reason) - 1);
    s_sweep.reason[sizeof(s_sweep.reason) - 1] = '\0';

    zone_sweep_force_relays_off(); /* final choke point -- covers normal completion too */
    if (s_sweep.state != ZONE_SWEEP_ABORTED && s_sweep.state != ZONE_SWEEP_FAILED) {
        s_sweep.reason[0] = '\0';
        /* M12: only a run that finished every zone gets to write the CT map.
         * An aborted or failed sweep has measured some zones and not others,
         * and a partial pass cannot see the two-zones-one-channel conflict
         * that is the whole reason the one-to-one check exists -- deriving
         * from it would write a map that looks confirmed and is not. */
        zone_sweep_push_ct_channel_map();
        /* M12b: strictly AFTER the map push, never before or interleaved.
         * Both stage into the SAME staged-config buffer on the Pico and each
         * ends with its own COMMIT_CONFIG, so they have to be two complete
         * transactions in sequence; zone_sweep_plan_k_ct() additionally
         * refuses outright if the map push left that buffer in a state it
         * could not repair. */
        zone_sweep_push_k_ct_v_per_a();
        /* Feature: nameplate current -> S14/S15 arming. Strictly AFTER the
         * k_ct push, same "one Pico-side staged buffer, one transaction at a
         * time" reasoning -- see zone_sweep_push_i_normal_a()'s own comment. */
        zone_sweep_push_i_normal_a();
        /* DONE goes up only AFTER the push has finished (opus review,
         * 2026-08-28). Setting it first left a window two link round trips
         * wide in which a status poll saw state=done with
         * ct_map_derived_mask still 0 and rendered a permanent "not
         * derived" -- the page never re-reads a sweep it has already seen
         * finish. The push runs entirely with the relays off either way; it
         * is only the moment the page is TOLD the run is over that moves. */
        s_sweep.state = ZONE_SWEEP_DONE;
    }
    /* Release the heat claim taken in zones_current_sweep_start() -- must
     * happen before s_sweep.active goes false, not after: the moment
     * s_sweep.active reads false, a waiting profile/autotune start can
     * observe it and attempt its own heat-zone claim; releasing first means
     * that claim is genuinely free the instant this sweep stops being
     * reachable, instead of leaving a window where the sweep looks finished
     * but still (briefly) holds exclusivity. */
    relay_authority_heat_sweep_claim_end();
    s_sweep.active = false;
    s_sweep.task = NULL;
    vTaskDelete(NULL);
}

zone_sweep_refusal_t zones_current_sweep_start(void)
{
    bool profile_running_or_paused = false;
    profile_exec_status_t pstat;
    memset(&pstat, 0, sizeof(pstat));
    profile_executor_get_status(&pstat);
    profile_running_or_paused = (pstat.state == PROFILE_EXEC_RUNNING || pstat.state == PROFILE_EXEC_PAUSED);

    bool link_up = false;
    bool trip_latched = false;
    if (s_hw_safety) {
        safety_link_status_t st;
        memset(&st, 0, sizeof(st));
        if (safety_link_get_status(s_hw_safety, &st) == ESP_OK) {
            link_up = st.link_up;
            trip_latched = st.fault_asserted || (st.diag_ever_received && st.diag_state == SAFETY_LINK_DIAG_STATE_TRIPPED);
        }
    }

    /* H1 (opus review, 2026-08-27): have_hw used to be `s_hw_io != NULL`
     * alone. zone_sweep_read_zone_temp() early-returns invalid whenever
     * !s_hw_thermo_bus || !initialized, and zone_sweep_ceiling_hit() treats
     * an invalid reading as "not a ceiling hit" (by design -- see that
     * function's own comment). Together that means a board with relay I/O
     * but no thermo bus wired in could start a sweep that energizes real
     * elements with the ceiling abort structurally incapable of ever firing
     * -- no thermocouple reading ever arrives to trip it. Require the thermo
     * bus (and its own initialized flag) in have_hw too, so that gap refuses
     * up front (ZONE_SWEEP_REFUSE_NO_HW) instead of running unsupervised. */
    bool have_hw = (s_hw_io != NULL) && (s_hw_thermo_bus != NULL) && s_hw_thermo_bus->initialized;
    /* N9 (opus review, 2026-08-28): belt-and-suspenders with
     * zone_sweep_hw_energize()'s 0xFF all-others-off write. That write
     * already forces every OTHER relay off once a zone starts measuring, so
     * on its own it would be enough -- this check adds refusing to start at
     * all while anything is on, per zones_current_sweep_start()'s own
     * contract ("no relay is ever touched on a refused start"), and surfaces
     * the foreign-load condition to the operator explicitly instead of
     * silently overriding whatever they had on. */
    bool relays_on = s_hw_io && (kiln_io_get_relay_shadow(s_hw_io) != 0);
    /* opus review finding (MEDIUM): zone_cfg_committed_ct_topology() below
     * silently defaults an UNFETCHED safety param cache to per_zone, the
     * same value a genuinely-committed per_zone board reads -- the two are
     * indistinguishable to that accessor. Every first boot after the v2->v3
     * store bump starts with safety_cfg_store_fetched_ms_ago() ==
     * UINT32_MAX (never fetched), so refuse here rather than let a
     * summed-topology board sweep and persist per-zone-shaped normals. */
    bool ct_topology_unknown = (safety_cfg_store_fetched_ms_ago() == UINT32_MAX);
    zone_sweep_refusal_t refusal = zone_sweep_check_refusal(
        s_sweep.active, have_hw, s_zones_config_valid, s_zones.cfg.thermo_count,
        profile_running_or_paused, autotune_engine_is_active(), link_up, trip_latched, relays_on,
        ct_topology_unknown);
    if (refusal != ZONE_SWEEP_REFUSE_OK) {
        return refusal;
    }

    /* The atomic gate (relay_authority.h's heat-claim doc comment): every
     * check above, including profile_running_or_paused/autotune_active just
     * fed into zone_sweep_check_refusal(), is a plain read of another
     * module's state with no lock spanning the read and this function's own
     * commit just below -- exactly the TOCTOU a reviewer found bounded but
     * not correct-by-construction. This call is the last possible moment
     * before that commit, and it is a single mutex-protected test-and-set
     * against profile_executor.c's/autotune_engine.c's matching gate, so
     * whichever of the two commits first is the one that actually wins --
     * the loser is refused here with the SAME reason the informational
     * check above already reports for the common (non-race) case. */
    relay_heat_sweep_claim_result_t heat_claim = relay_authority_heat_sweep_claim_begin();
    if (heat_claim != RELAY_HEAT_SWEEP_CLAIM_OK) {
        return (heat_claim == RELAY_HEAT_SWEEP_CLAIM_REFUSE_PROFILE) ? ZONE_SWEEP_REFUSE_PROFILE_RUNNING
                                                                      : ZONE_SWEEP_REFUSE_AUTOTUNE_RUNNING;
    }

    s_sweep.active = true;
    s_sweep.abort_requested = false;
    s_sweep.state = ZONE_SWEEP_RUNNING;
    s_sweep.zone_index = 0;
    s_sweep.zones_done = 0;
    s_sweep.zones_total = s_zones.cfg.thermo_count;
    if (s_sweep.zones_total > MAX31856_CHANNEL_COUNT) {
        s_sweep.zones_total = MAX31856_CHANNEL_COUNT;
    }
    s_sweep.reason[0] = '\0';
    s_sweep.ct_map_derived_mask = 0;
    s_sweep.ct_map_reason[0] = '\0';
    s_sweep.k_ct_derived_mask = 0;
    s_sweep.k_ct_reason[0] = '\0';
    s_sweep.i_normal_pushed_mask = 0;
    s_sweep.i_normal_reason[0] = '\0';
    s_sweep.summed_unmeasured_mask = 0;

    BaseType_t created = xTaskCreate(zone_sweep_task, "zone_sweep", 4096, NULL, tskIDLE_PRIORITY + 2, &s_sweep.task);
    if (created != pdPASS) {
        relay_authority_heat_sweep_claim_end(); /* task never started -- give the claim back */
        s_sweep.active = false;
        s_sweep.state = ZONE_SWEEP_FAILED;
        snprintf((char *)s_sweep.reason, sizeof(s_sweep.reason), "failed to start sweep task");
        return ZONE_SWEEP_REFUSE_NO_HW;
    }
    return ZONE_SWEEP_REFUSE_OK;
}

/* B2: see zones_http.h's doc comment above the declaration. */
bool zones_current_sweep_is_active(void)
{
    return s_sweep.active;
}

void zones_current_sweep_abort(void)
{
    if (s_sweep.active) {
        s_sweep.abort_requested = true;
    }
}

void zones_current_sweep_get_status(zone_sweep_status_t *out)
{
    if (!out) {
        return;
    }
    out->state = s_sweep.state;
    out->zone_index = s_sweep.zone_index;
    out->zones_done = s_sweep.zones_done;
    out->zones_total = s_sweep.zones_total;
    strncpy(out->reason, (const char *)s_sweep.reason, sizeof(out->reason) - 1);
    out->reason[sizeof(out->reason) - 1] = '\0';
    out->ct_map_derived_mask = s_sweep.ct_map_derived_mask;
    strncpy(out->ct_map_reason, (const char *)s_sweep.ct_map_reason, sizeof(out->ct_map_reason) - 1);
    out->ct_map_reason[sizeof(out->ct_map_reason) - 1] = '\0';
    out->k_ct_derived_mask = s_sweep.k_ct_derived_mask;
    strncpy(out->k_ct_reason, (const char *)s_sweep.k_ct_reason, sizeof(out->k_ct_reason) - 1);
    out->k_ct_reason[sizeof(out->k_ct_reason) - 1] = '\0';
    out->i_normal_pushed_mask = s_sweep.i_normal_pushed_mask;
    strncpy(out->i_normal_reason, (const char *)s_sweep.i_normal_reason, sizeof(out->i_normal_reason) - 1);
    out->i_normal_reason[sizeof(out->i_normal_reason) - 1] = '\0';
    out->summed_unmeasured_mask = s_sweep.summed_unmeasured_mask;
}

/* ---- Task 2: runtime CT-to-zone mapping check ----------------------------- */

/* Ratio band a live reading must fall within to be considered a plausible
 * match for the measured normal, plus an absolute floor so a tiny normal
 * (a lightly-loaded zone) doesn't turn ordinary measurement noise into a
 * false warning purely from ratio math. Deliberately wide -- this is a
 * WRONG-JACK detector (a swapped CT reads close to 0A, or reads some OTHER
 * zone's current instead), not a precision check; SaftyFW/docs/
 * CURRENT_SENSE.md §0 already scopes current accuracy as "within a factor
 * of ~2" for load-active detection, and this reuses that same order-of-
 * magnitude tolerance rather than inventing a tighter one nothing in the
 * hardware chain can actually promise. */
#define ZONE_CT_MISMATCH_RATIO_LOW 0.4f
#define ZONE_CT_MISMATCH_RATIO_HIGH 2.5f
#define ZONE_CT_MISMATCH_MIN_DELTA_A 0.3f

bool zones_ct_mapping_mismatch(float normal_current_a, bool normal_measured, float live_current_a)
{
    if (!normal_measured) {
        return false; /* silent -- see this function's header comment */
    }
    if (isnan(normal_current_a) || normal_current_a <= 0.0f || isnan(live_current_a) || live_current_a < 0.0f) {
        return false;
    }
    float lo = normal_current_a * ZONE_CT_MISMATCH_RATIO_LOW;
    float hi = normal_current_a * ZONE_CT_MISMATCH_RATIO_HIGH;
    if (live_current_a >= lo && live_current_a <= hi) {
        return false;
    }
    return fabsf(live_current_a - normal_current_a) >= ZONE_CT_MISMATCH_MIN_DELTA_A;
}

uint8_t zones_ct_mapping_warn_mask(void)
{
    if (!s_hw_io || !s_hw_safety) {
        return 0;
    }
    safety_link_status_t st;
    memset(&st, 0, sizeof(st));
    if (safety_link_get_status(s_hw_safety, &st) != ESP_OK || !st.link_up) {
        return 0;
    }
    uint8_t relay_now = kiln_io_get_relay_shadow(s_hw_io);
    uint8_t warn = 0;
    for (uint8_t zi = 0; zi < s_zones.cfg.thermo_count && zi < MAX31856_CHANNEL_COUNT; zi++) {
        uint8_t relay_mask = s_zones.cfg.zones[zi].relay_mask;
        if (relay_mask == 0 || (relay_now & relay_mask) == 0) {
            continue; /* zone not commanded on right now -- nothing to compare */
        }
        float normal_a = 0.0f;
        bool measured = false;
        zones_config_get_normal_current(zi, &normal_a, &measured);
        uint8_t ctmask = 0;
        zones_config_get_ct_mask(zi, &ctmask);
        float live_a = 0.0f;
        for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
            if (ctmask & (1u << c)) {
                live_a += st.current_a[c];
            }
        }
        if (zones_ct_mapping_mismatch(normal_a, measured, live_a)) {
            warn |= (uint8_t)(1u << zi);
        }
    }
    return warn;
}

/* ---- Task 3: read-only safety-processor wiring display --------------------- */

void zones_get_safety_wiring(zone_safety_wiring_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_hw_safety) {
        return;
    }
    safety_link_status_t st;
    memset(&st, 0, sizeof(st));
    if (safety_link_get_status(s_hw_safety, &st) != ESP_OK || !st.link_up) {
        return; /* leave the zeroed/false "UNSET" defaults */
    }
    out->link_up = true;
    out->tc_temp_valid = !isnan(st.tc_temp_c);
    out->tc_temp_c = st.tc_temp_c;
    out->tc_fault = st.tc_fault;
    out->relay_energized = (st.flags & SAFETY_FLAG_RELAY) != 0;
    out->tc_is_separate_sensor = safety_tc_is_separate_physical_sensor(&st);
}


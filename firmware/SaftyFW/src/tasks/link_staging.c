// link_staging.c -- see link_staging.h.
#include "link_staging.h"

#include <string.h>

#include "config_params.h"
#include "kilnlink/kilnlink_param_value.h"

void link_staging_reset(link_staging_t *st)
{
    if (!st) {
        return;
    }
    memset(st, 0, sizeof(*st));
}

size_t link_staging_count(const link_staging_t *st)
{
    return st ? st->count : 0u;
}

static void link_staging_remove_at(link_staging_t *st, size_t idx)
{
    for (size_t i = idx + 1u; i < st->count; ++i) {
        st->edits[i - 1u] = st->edits[i];
    }
    st->count--;
    memset(&st->edits[st->count], 0, sizeof(st->edits[st->count]));
}

bool link_staging_stage(link_staging_t *st, const kilnlink_set_param_t *edit,
                        config_store_record_t *scratch)
{
    if (!st || !edit || !scratch) {
        return false;
    }
    if (!config_params_set(scratch, edit->param_id, edit->type, edit->value)) {
        return false;
    }
    for (size_t i = 0; i < st->count; ++i) {
        if (st->edits[i].param_id == edit->param_id) {
            st->edits[i] = *edit;
            return true;
        }
    }
    if (st->count >= LINK_STAGING_CAPACITY) {
        // Unreachable while every staged id is a distinct config_params id
        // (config_params.c asserts its table is <= 72) -- refuse rather than
        // silently overwrite an earlier edit.
        return false;
    }
    st->edits[st->count++] = *edit;
    return true;
}

bool link_staging_apply(const link_staging_t *st, config_store_record_t *inout)
{
    if (!st || !inout) {
        return false;
    }
    for (size_t i = 0; i < st->count; ++i) {
        const kilnlink_set_param_t *e = &st->edits[i];
        if (!config_params_set(inout, e->param_id, e->type, e->value)) {
            return false;
        }
    }
    return true;
}

static bool link_staging_field_differs(const config_store_record_t *a,
                                       const config_store_record_t *b, uint16_t id)
{
    uint8_t ta = 0u;
    uint8_t tb = 0u;
    kilnlink_param_value_t va;
    kilnlink_param_value_t vb;
    memset(&va, 0, sizeof(va));
    memset(&vb, 0, sizeof(vb));
    bool ga = config_params_get(a, id, &ta, &va);
    bool gb = config_params_get(b, id, &tb, &vb);
    if (ga != gb || ta != tb) {
        return true;
    }
    if (!ga) {
        return false;
    }
    size_t len = kilnlink_param_value_len(ta);
    if (len == 0u || len > sizeof(va)) {
        return true;
    }
    if (memcmp(&va, &vb, len) != 0) {
        return true;
    }
    return config_params_is_set(a, id) != config_params_is_set(b, id);
}

size_t link_staging_drop_superseded(link_staging_t *st, const config_store_record_t *before,
                                    const config_store_record_t *after)
{
    if (!st || !before || !after) {
        return 0u;
    }
    size_t dropped = 0u;
    size_t i = 0u;
    while (i < st->count) {
        if (link_staging_field_differs(before, after, st->edits[i].param_id)) {
            link_staging_remove_at(st, i);
            dropped++;
        } else {
            ++i;
        }
    }
    return dropped;
}

bool link_staging_drop_id(link_staging_t *st, uint16_t param_id)
{
    if (!st) {
        return false;
    }
    for (size_t i = 0; i < st->count; ++i) {
        if (st->edits[i].param_id == param_id) {
            link_staging_remove_at(st, i);
            return true;
        }
    }
    return false;
}

bool link_staging_new_esp_session(bool prev_known, uint8_t prev_boot_id, uint8_t boot_id,
                                  bool context_gap)
{
    if (!prev_known) {
        return false;
    }
    return (boot_id != prev_boot_id) || context_gap;
}

// panel_detect.c -- see panel_detect.h for what this is, why it has no
// ESP-IDF/I2C/SPI code in it, and the TODO naming exactly which two bench
// measurements populate the (currently empty) id_matches table this
// function decides over.
#include "panel_detect.h"

/* Index of the candidate whose `touch` field equals `kind`, or -1 if none
 * (or more than one -- an n_candidates table naming the same touch kind
 * twice is a caller bug this treats the same as "no usable signal" rather
 * than guessing). PANEL_DETECT_TOUCH_UNKNOWN never matches: it means "this
 * candidate has nothing to say about touch", not "the touch probe found
 * UNKNOWN". */
static int touch_says_index(const panel_detect_candidate_t *candidates, size_t n_candidates,
                             panel_detect_touch_kind_t kind)
{
    if (kind == PANEL_DETECT_TOUCH_UNKNOWN) return -1;

    int found = -1;
    for (size_t i = 0; i < n_candidates; ++i) {
        if (candidates[i].touch == kind) {
            if (found >= 0) return -1; /* two candidates claim the same touch kind */
            found = (int)i;
        }
    }
    return found;
}

panel_detect_result_t panel_detect_choose(const uint8_t id[3], bool touch_ns2009_present,
                                           bool touch_ft6336_present,
                                           const panel_detect_candidate_t *candidates,
                                           size_t n_candidates,
                                           const panel_desc_t *kconfig_default)
{
    panel_detect_result_t result = {
        .panel = kconfig_default,
        .source = PANEL_DETECT_SOURCE_FALLBACK,
        .disagreement = false,
        .matched_count = 0,
    };

    /* Exactly one of the two touch signals present resolves to a single
     * kind; both or neither present is "no usable tiebreak signal" -- the
     * exact same "ambiguous corroboration" treatment as an ambiguous SPI ID
     * match below, not an error. */
    panel_detect_touch_kind_t touch_kind = PANEL_DETECT_TOUCH_UNKNOWN;
    if (touch_ns2009_present && !touch_ft6336_present) {
        touch_kind = PANEL_DETECT_TOUCH_NS2009;
    } else if (touch_ft6336_present && !touch_ns2009_present) {
        touch_kind = PANEL_DETECT_TOUCH_FT6336;
    }
    int touch_idx = touch_says_index(candidates, n_candidates, touch_kind);

    /* Count SPI-ID matches. A NULL id_matches (today's real descriptors,
     * per the empty-table TODO in panel_detect.h) never matches -- it is
     * not a crash and not treated any differently from a matcher that
     * always returns false. */
    int match_idx = -1;
    uint8_t matched_count = 0;
    for (size_t i = 0; i < n_candidates; ++i) {
        if (candidates[i].panel && candidates[i].panel->id_matches &&
            candidates[i].panel->id_matches(id)) {
            matched_count++;
            match_idx = (int)i;
        }
    }
    result.matched_count = matched_count;

    if (matched_count == 1) {
        /* Unambiguous SPI match: it wins. The touch probe still
         * corroborates -- if it names a *different* candidate, that is
         * diagnostic information the caller must log loudly (Sec.6 Step 3
         * point 3), not silently swallowed by preferring the SPI result. A
         * touch signal that named nothing (touch_idx < 0, e.g. no touch
         * chip answered, or both/neither address answered) is not a
         * disagreement -- there is nothing to disagree with. */
        result.panel = candidates[match_idx].panel;
        result.source = PANEL_DETECT_SOURCE_SPI_MATCH;
        result.disagreement = (touch_idx >= 0) && (touch_idx != match_idx);
        return result;
    }

    if (matched_count > 1) {
        /* Ambiguous SPI match (Sec.13 Risks: "RDDID may not distinguish the
         * panels"). If the touch probe names exactly one candidate AND that
         * candidate is among the ones that matched on SPI, it breaks the
         * tie. Otherwise there is no safe way to pick between them --
         * fall back, and flag the disagreement so it gets logged (an
         * ambiguous ID table plus a touch signal that could not resolve it
         * is exactly the situation Sec.6 Step 3 point 3 wants surfaced). */
        if (touch_idx >= 0) {
            bool touch_choice_matched = false;
            for (size_t i = 0; i < n_candidates; ++i) {
                if ((int)i == touch_idx && candidates[i].panel && candidates[i].panel->id_matches &&
                    candidates[i].panel->id_matches(id)) {
                    touch_choice_matched = true;
                    break;
                }
            }
            if (touch_choice_matched) {
                result.panel = candidates[touch_idx].panel;
                result.source = PANEL_DETECT_SOURCE_TOUCH_TIEBREAK;
                result.disagreement = false;
                return result;
            }
        }
        result.disagreement = true;
        return result; /* matched_count already set; source stays FALLBACK */
    }

    /* matched_count == 0: no SPI match at all. Sec.6 Step 3 point 4 --
     * fall back to the Kconfig default. A touch signal here (touch_idx >= 0
     * but naming a candidate the SPI probe did not confirm) is still worth
     * flagging: the touch chip disagrees with "nothing matched", which is
     * exactly the two signals pointing in different directions. */
    result.disagreement = (touch_idx >= 0);
    return result;
}

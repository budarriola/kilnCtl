// Host tests for panel_detect_choose() (../drivers/panel_detect.c).
// DISPLAY_ST7796_PLAN.md Sec.6 Step 3 / Sec.12 Phase 4.
//
// The real descriptors (ili9488_panel_desc in panel_spi.c, st7796_panel_desc
// in st7796_panel.c) both have id_matches == NULL today -- Sec.4's bench
// bytes do not exist yet. panel_spi.c is not host-tested at all (it pulls in
// driver/spi_master.h/esp_spi_owner.h/FreeRTOS -- see test_st7796_panel.c's
// own comment for the same reason), so the ILI9488 candidate here is a
// small local stand-in descriptor, not ILI9488_get_panel_desc(). The real,
// host-testable ST7796 descriptor (st7796_panel.c) is used directly for the
// "today's actual table" fallback test so that test exercises real
// production data, not just a synthetic stand-in.
#include "test_common.h"
#include "../drivers/panel_detect.h"
#include "../drivers/st7796_panel.h"

#include <string.h>

static bool matches_AA(const uint8_t id[3]) { return id[0] == 0xAA && id[1] == 0xBB && id[2] == 0xCC; }
static bool matches_11(const uint8_t id[3]) { return id[0] == 0x11 && id[1] == 0x22 && id[2] == 0x33; }
/* A second matcher that answers true on the SAME bytes as matches_AA, used
 * only to build a deliberately ambiguous table. */
static bool matches_AA_too(const uint8_t id[3]) { return id[0] == 0xAA && id[1] == 0xBB && id[2] == 0xCC; }

static const panel_desc_t stub_panel_a = {
    .name = "StubA", .id_matches = matches_AA,
};
static const panel_desc_t stub_panel_b = {
    .name = "StubB", .id_matches = matches_11,
};
static const panel_desc_t stub_panel_c_ambiguous = {
    .name = "StubC", .id_matches = matches_AA_too,
};
static const panel_desc_t stub_kconfig_default = {
    .name = "KconfigDefault", .id_matches = NULL,
};

static void test_empty_table_falls_back(void)
{
    TEST_SECTION("panel_detect_choose: empty/unpopulated table falls back");

    /* Today's real production shape: both real descriptors have
     * id_matches == NULL (st7796_panel_desc, host-testable, used directly;
     * the ILI9488 side is represented by another NULL-matcher stub since
     * panel_spi.c itself cannot be linked into a host test). No byte pattern
     * -- including all-zero and all-0xFF, the "no panel wired" case
     * ILI9488_read_id() already treats specially -- can ever match, so the
     * result must always be the Kconfig default, with matched_count 0 and
     * source FALLBACK. THIS is the test that would go red if the fallback
     * path were ever removed or short-circuited to "just return the first
     * candidate" -- see the negative-test log in the session report. */
    const panel_detect_candidate_t table[] = {
        { .panel = ST7796_get_panel_desc(), .touch = PANEL_DETECT_TOUCH_FT6336 },
        { .panel = &stub_kconfig_default, .touch = PANEL_DETECT_TOUCH_NS2009 },
    };
    const uint8_t all_zero[3] = { 0x00, 0x00, 0x00 };
    const uint8_t all_ff[3] = { 0xFF, 0xFF, 0xFF };
    const uint8_t plausible[3] = { 0x12, 0x34, 0x56 };

    for (int i = 0; i < 3; ++i) {
        const uint8_t *id = i == 0 ? all_zero : i == 1 ? all_ff : plausible;
        panel_detect_result_t r =
            panel_detect_choose(id, false, false, table, 2, &stub_kconfig_default);
        TEST_CHECK(r.panel == &stub_kconfig_default,
                   "empty table: resolved panel is the Kconfig default");
        TEST_CHECK(r.source == PANEL_DETECT_SOURCE_FALLBACK, "empty table: source is FALLBACK");
        TEST_CHECK(r.matched_count == 0, "empty table: matched_count is 0");
        TEST_CHECK(!r.disagreement, "empty table: no touch signal, so no disagreement");
    }
}

static void test_empty_n_candidates_falls_back(void)
{
    TEST_SECTION("panel_detect_choose: zero-length candidate array");

    const uint8_t id[3] = { 0xAA, 0xBB, 0xCC };
    panel_detect_result_t r = panel_detect_choose(id, false, false, NULL, 0, &stub_kconfig_default);
    TEST_CHECK(r.panel == &stub_kconfig_default, "n=0: falls back to the Kconfig default");
    TEST_CHECK(r.source == PANEL_DETECT_SOURCE_FALLBACK, "n=0: source is FALLBACK");
    TEST_CHECK(r.matched_count == 0, "n=0: matched_count is 0");
}

static void test_unambiguous_spi_match(void)
{
    TEST_SECTION("panel_detect_choose: single SPI ID match, no touch signal");

    const panel_detect_candidate_t table[] = {
        { .panel = &stub_panel_a, .touch = PANEL_DETECT_TOUCH_NS2009 },
        { .panel = &stub_panel_b, .touch = PANEL_DETECT_TOUCH_FT6336 },
    };
    const uint8_t id[3] = { 0xAA, 0xBB, 0xCC };
    panel_detect_result_t r = panel_detect_choose(id, false, false, table, 2, &stub_kconfig_default);

    TEST_CHECK(r.panel == &stub_panel_a, "single match: resolves to the matching candidate");
    TEST_CHECK(r.source == PANEL_DETECT_SOURCE_SPI_MATCH, "single match: source is SPI_MATCH");
    TEST_CHECK(r.matched_count == 1, "single match: matched_count is 1");
    TEST_CHECK(!r.disagreement, "single match, no touch probe result: no disagreement");
}

static void test_touch_agrees_no_disagreement(void)
{
    TEST_SECTION("panel_detect_choose: SPI match corroborated by touch");

    const panel_detect_candidate_t table[] = {
        { .panel = &stub_panel_a, .touch = PANEL_DETECT_TOUCH_NS2009 },
        { .panel = &stub_panel_b, .touch = PANEL_DETECT_TOUCH_FT6336 },
    };
    const uint8_t id[3] = { 0xAA, 0xBB, 0xCC };
    /* NS2009 answers, matching stub_panel_a's expected touch kind -- same
     * chip the SPI ID already pointed at. */
    panel_detect_result_t r = panel_detect_choose(id, true, false, table, 2, &stub_kconfig_default);

    TEST_CHECK(r.panel == &stub_panel_a, "touch agrees: resolved panel unchanged");
    TEST_CHECK(r.source == PANEL_DETECT_SOURCE_SPI_MATCH, "touch agrees: source stays SPI_MATCH");
    TEST_CHECK(!r.disagreement, "touch agrees: disagreement is false");
}

static void test_touch_disagrees_is_logged(void)
{
    TEST_SECTION("panel_detect_choose: SPI match contradicted by touch probe");

    const panel_detect_candidate_t table[] = {
        { .panel = &stub_panel_a, .touch = PANEL_DETECT_TOUCH_NS2009 },
        { .panel = &stub_panel_b, .touch = PANEL_DETECT_TOUCH_FT6336 },
    };
    const uint8_t id[3] = { 0xAA, 0xBB, 0xCC }; /* matches stub_panel_a (NS2009 panel) */
    /* But the FT6336U answered instead -- stub_panel_b's chip. The two
     * signals disagree about which panel is attached. SPI still wins (it is
     * the primary signal per Sec.6 Step 3), but disagreement MUST be true so
     * the caller logs it loudly -- this is the exact check that would go red
     * if the disagreement flag were ever dropped. */
    panel_detect_result_t r = panel_detect_choose(id, false, true, table, 2, &stub_kconfig_default);

    TEST_CHECK(r.panel == &stub_panel_a, "touch disagrees: SPI match still wins");
    TEST_CHECK(r.source == PANEL_DETECT_SOURCE_SPI_MATCH, "touch disagrees: source stays SPI_MATCH");
    TEST_CHECK(r.disagreement, "touch disagrees: disagreement flag is set");
}

static void test_both_touch_addresses_present_is_not_a_signal(void)
{
    TEST_SECTION("panel_detect_choose: both touch addresses answering is ambiguous, not a signal");

    const panel_detect_candidate_t table[] = {
        { .panel = &stub_panel_a, .touch = PANEL_DETECT_TOUCH_NS2009 },
        { .panel = &stub_panel_b, .touch = PANEL_DETECT_TOUCH_FT6336 },
    };
    const uint8_t id[3] = { 0xAA, 0xBB, 0xCC };
    /* Two chips answering at once is not evidence for either side -- treated
     * the same as neither answering: no disagreement, SPI match still wins. */
    panel_detect_result_t r = panel_detect_choose(id, true, true, table, 2, &stub_kconfig_default);

    TEST_CHECK(r.panel == &stub_panel_a, "both touch addresses: SPI match still wins");
    TEST_CHECK(!r.disagreement, "both touch addresses answering: not treated as disagreement");
}

static void test_ambiguous_spi_broken_by_touch(void)
{
    TEST_SECTION("panel_detect_choose: ambiguous SPI ID, touch breaks the tie");

    const panel_detect_candidate_t table[] = {
        { .panel = &stub_panel_a, .touch = PANEL_DETECT_TOUCH_NS2009 },
        { .panel = &stub_panel_c_ambiguous, .touch = PANEL_DETECT_TOUCH_FT6336 },
    };
    const uint8_t id[3] = { 0xAA, 0xBB, 0xCC }; /* matches BOTH stub_panel_a and stub_panel_c_ambiguous */
    panel_detect_result_t r = panel_detect_choose(id, false, true, table, 2, &stub_kconfig_default);

    TEST_CHECK(r.matched_count == 2, "ambiguous SPI: both candidates counted as matched");
    TEST_CHECK(r.panel == &stub_panel_c_ambiguous, "ambiguous SPI: touch breaks the tie correctly");
    TEST_CHECK(r.source == PANEL_DETECT_SOURCE_TOUCH_TIEBREAK, "ambiguous SPI: source is TOUCH_TIEBREAK");
    TEST_CHECK(!r.disagreement, "ambiguous SPI resolved by touch: not a disagreement");
}

static void test_ambiguous_spi_no_touch_falls_back(void)
{
    TEST_SECTION("panel_detect_choose: ambiguous SPI ID, no usable touch signal");

    const panel_detect_candidate_t table[] = {
        { .panel = &stub_panel_a, .touch = PANEL_DETECT_TOUCH_NS2009 },
        { .panel = &stub_panel_c_ambiguous, .touch = PANEL_DETECT_TOUCH_FT6336 },
    };
    const uint8_t id[3] = { 0xAA, 0xBB, 0xCC };
    panel_detect_result_t r = panel_detect_choose(id, false, false, table, 2, &stub_kconfig_default);

    TEST_CHECK(r.matched_count == 2, "ambiguous SPI, no touch: still counted as 2 matches");
    TEST_CHECK(r.panel == &stub_kconfig_default,
               "ambiguous SPI, no touch: falls back to the Kconfig default, does not guess");
    TEST_CHECK(r.source == PANEL_DETECT_SOURCE_FALLBACK, "ambiguous SPI, no touch: source is FALLBACK");
    TEST_CHECK(r.disagreement, "ambiguous SPI, no touch: flagged so an operator can see it");
}

void run_test_panel_detect(void)
{
    test_empty_table_falls_back();
    test_empty_n_candidates_falls_back();
    test_unambiguous_spi_match();
    test_touch_agrees_no_disagreement();
    test_touch_disagrees_is_logged();
    test_both_touch_addresses_present_is_not_a_signal();
    test_ambiguous_spi_broken_by_touch();
    test_ambiguous_spi_no_touch_falls_back();
}

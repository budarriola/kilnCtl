/* Host tests for readiness_http.c's "Safety processor commissioned" item.
 *
 * Why this item exists at all: before 2026-08-22 the readiness page could show
 * every light green on a board whose safety processor had never been
 * commissioned -- no trip thresholds, no TC type, nothing -- because no item
 * ever asked. These tests pin the three answers that matter, in particular the
 * one that is easy to get wrong: "link down and nothing cached" must NOT be
 * reported as unfinished operator work, because the ESP cannot distinguish it
 * from a Pico it simply has not talked to yet.
 */

#include "test_common.h"
#include "../drivers/readiness_http.h"

void run_test_readiness_commissioning(void)
{
    TEST_SECTION("readiness commissioning item -- safety processor commissioned");

    /* Never fetched, link up: the Pico is answering and still has no config,
     * so this is real, actionable, unfinished commissioning. */
    TEST_CHECK(readiness_commissioning_status(true, 0, 0, 57) == READY_NOT_DONE,
                "crc=0 with the link up must read not_done");

    /* Never fetched, link down: indistinguishable from "not talked to yet".
     * Reporting not_done here would nag the operator about a task they cannot
     * perform and might already have done. */
    TEST_CHECK(readiness_commissioning_status(false, 0, 0, 57) == READY_CANNOT_YET,
                "crc=0 with the link down must read cannot_yet, not not_done");

    /* Fetched, every parameter has a value. */
    TEST_CHECK(readiness_commissioning_status(true, 0xBEEF, 0, 57) == READY_OK,
                "a real crc with no unset params must read ok");

    /* Fetched, but some parameters still have no value -- the sec-1 fields
     * with no compiled-in default read this way until actually set. */
    TEST_CHECK(readiness_commissioning_status(true, 0xBEEF, 4, 57) == READY_NOT_DONE,
                "a real crc with unset params must read not_done");

    /* One unset out of many is still not_done: this item is all-or-nothing on
     * purpose, since a single unset trip threshold is a real safety hole. */
    TEST_CHECK(readiness_commissioning_status(true, 0xBEEF, 1, 57) == READY_NOT_DONE,
                "a single unset param must still read not_done");

    /* A cached crc with an empty param table must not report ok by vacuous
     * truth ("all zero of them are set"). */
    TEST_CHECK(readiness_commissioning_status(true, 0xBEEF, 0, 0) == READY_NOT_DONE,
                "an empty param table must not read ok vacuously");

    /* The link state must not override a successful fetch: once we hold real
     * values, a momentarily-down link does not make them unknown. */
    TEST_CHECK(readiness_commissioning_status(false, 0xBEEF, 0, 57) == READY_OK,
                "cached values stay ok even when the link is momentarily down");
}

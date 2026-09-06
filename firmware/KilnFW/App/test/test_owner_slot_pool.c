// Host tests for owner_slot_pool.c (../drivers/owners/owner_slot_pool.c) -- the
// pure two-sided release protocol kiln_io_owner.c's and thermo_owner.c's
// post_and_wait()/owner_task() use to fix the 2026-08-24 stack-lifetime bug
// (see owner_slot_pool.h's top comment for the full story). FreeRTOS's real
// blocking semaphore/queue behaviour cannot be exercised through this
// project's host-test stubs (App/test/stubs/freertos/*.h -- xSemaphoreTake()
// always returns pdFALSE, xQueueSend()/xQueueReceive() never actually
// deliver, by design; see those headers' own comments), so this suite tests
// the extracted pure state machine directly instead, per this pass's
// instructions to prefer that over leaving the race untested.
//
// The race under test: a client "gives up" (post_and_wait()'s timeout path)
// and the owner task "finishes late" (owner_task()'s tail) are exactly
// owner_slot_pool_release() calls in either order. This suite drives both
// orders, plus the two-clients-concurrently and back-to-back-timeout-then-
// success shapes the task named, and proves a slot can never look free (and
// so be handed to a NEW command) while either side might still touch it.
#include "test_common.h"
#include "../drivers/owners/owner_slot_pool.h"

#include <stdint.h>

void run_test_owner_slot_pool(void)
{
    TEST_SECTION("owner_slot_pool");

    /* --- alloc(): hands out free slots, refuses when the pool is exhausted --- */
    {
        uint8_t refs[3] = { 0, 0, 0 };

        int a = owner_slot_pool_alloc(refs, 3);
        int b = owner_slot_pool_alloc(refs, 3);
        int c = owner_slot_pool_alloc(refs, 3);
        TEST_CHECK(a == 0 && b == 1 && c == 2, "alloc() hands out slots 0,1,2 in order");
        TEST_CHECK(refs[0] == OWNER_SLOT_POOL_HELD_BY_BOTH && refs[1] == OWNER_SLOT_POOL_HELD_BY_BOTH &&
                       refs[2] == OWNER_SLOT_POOL_HELD_BY_BOTH,
                   "every allocated slot starts held by both sides (refcount 2)");

        int d = owner_slot_pool_alloc(refs, 3);
        TEST_CHECK(d == -1, "alloc() refuses once every slot is held -- fails closed, same as a full queue");
    }

    /* --- Normal success: client takes the result, then both sides release
     * in the expected order (client first, since it read the semaphore
     * before the owner's post-Give() release runs) -- first release must
     * NOT free the slot (the other side hasn't let go yet), second must. */
    {
        uint8_t refs[1] = { 0 };
        int idx = owner_slot_pool_alloc(refs, 1);
        TEST_CHECK(idx == 0, "normal success: slot allocated");

        bool freed_by_client = owner_slot_pool_release(refs, 1, idx);
        TEST_CHECK(!freed_by_client, "normal success: client's release alone does not free the slot");
        TEST_CHECK(refs[0] == 1, "normal success: refcount is 1 after one release");

        bool freed_by_owner = owner_slot_pool_release(refs, 1, idx);
        TEST_CHECK(freed_by_owner, "normal success: owner's release (the second one) frees the slot");
        TEST_CHECK(refs[0] == 0, "normal success: refcount is 0 once both sides released");

        int reused = owner_slot_pool_alloc(refs, 1);
        TEST_CHECK(reused == 0, "normal success: a fully-released slot can be reused");
    }

    /* --- THE RACE: client times out (releases first) while the owner is
     * still mid-transfer, owner completes and releases LATE. This is the
     * exact scenario the task description names: "a command that times out
     * on the client side, then the owner completing afterward" -- prove the
     * late completion cannot free the slot early (which would let a NEW
     * command's alloc() reuse it and then have the late Give()/write land
     * on the WRONG command's result) and cannot itself under/over-flow. --- */
    {
        uint8_t refs[1] = { 0 };
        int idx = owner_slot_pool_alloc(refs, 1);

        /* Client gives up first -- post_and_wait()'s timeout path. */
        bool freed_by_client_timeout = owner_slot_pool_release(refs, 1, idx);
        TEST_CHECK(!freed_by_client_timeout,
                   "race: client timing out first does NOT free the slot (owner still holds it)");

        /* Between the two releases, the slot must still look busy -- this is
         * the "cannot corrupt a subsequent command's result" half: a new
         * command posted right now must NOT be handed this same index while
         * the owner task might still write into it. */
        int other = owner_slot_pool_alloc(refs, 1);
        TEST_CHECK(other == -1,
                   "race: the timed-out slot stays reserved -- a NEW command cannot be handed it "
                   "while the owner might still write a late result into it");

        /* Owner finishes late and releases its half. */
        bool freed_by_owner_late = owner_slot_pool_release(refs, 1, idx);
        TEST_CHECK(freed_by_owner_late,
                   "race: the owner's late release (arriving second) is the one that frees the slot");

        /* Only now may the slot be reused. */
        int reused = owner_slot_pool_alloc(refs, 1);
        TEST_CHECK(reused == idx, "race: the slot becomes available again only after BOTH sides released");
    }

    /* --- Same race, opposite order: owner finishes (and releases) before
     * the client's timeout fires -- proves order truly does not matter. --- */
    {
        uint8_t refs[1] = { 0 };
        int idx = owner_slot_pool_alloc(refs, 1);

        bool freed_by_owner_first = owner_slot_pool_release(refs, 1, idx);
        TEST_CHECK(!freed_by_owner_first, "race (reversed): owner releasing first does not free the slot");

        bool freed_by_client_second = owner_slot_pool_release(refs, 1, idx);
        TEST_CHECK(freed_by_client_second,
                   "race (reversed): client's release (arriving second) is the one that frees the slot");
    }

    /* --- Back-to-back: a client times out on one command, then immediately
     * posts another, before the first's owner-side release ever arrives.
     * The second command must land on a DIFFERENT slot, and the first
     * command's eventual late completion must not disturb it. --- */
    {
        uint8_t refs[2] = { 0, 0 };

        int first = owner_slot_pool_alloc(refs, 2);
        owner_slot_pool_release(refs, 2, first); /* client times out on command 1 */

        int second = owner_slot_pool_alloc(refs, 2); /* client immediately posts command 2 */
        TEST_CHECK(second != -1 && second != first,
                   "back-to-back: a second command right after a timeout gets a DIFFERENT slot");

        /* Command 1's owner finally answers, late. */
        bool first_freed = owner_slot_pool_release(refs, 2, first);
        TEST_CHECK(first_freed, "back-to-back: command 1's slot frees on its owner-side release");

        /* Command 2 is still in flight and must be completely unaffected. */
        TEST_CHECK(refs[second] == OWNER_SLOT_POOL_HELD_BY_BOTH,
                   "back-to-back: command 2's slot is untouched by command 1's late completion");

        /* Command 2 finishes normally. */
        owner_slot_pool_release(refs, 2, second);
        bool second_freed = owner_slot_pool_release(refs, 2, second);
        TEST_CHECK(second_freed, "back-to-back: command 2 frees normally afterward");
    }

    /* --- Two concurrent clients, interleaved release order -- proves the
     * pool tracks each slot independently rather than any global state. --- */
    {
        uint8_t refs[2] = { 0, 0 };
        int slot_a = owner_slot_pool_alloc(refs, 2);
        int slot_b = owner_slot_pool_alloc(refs, 2);
        TEST_CHECK(slot_a != slot_b, "two concurrent clients get two distinct slots");

        /* A's owner finishes first; B's client times out first -- fully
         * interleaved, opposite roles on each slot. */
        bool a_owner_first = owner_slot_pool_release(refs, 2, slot_a);
        bool b_client_first = owner_slot_pool_release(refs, 2, slot_b);
        TEST_CHECK(!a_owner_first && !b_client_first, "concurrent: first release on each slot does not free it");

        bool a_client_second = owner_slot_pool_release(refs, 2, slot_a);
        bool b_owner_second = owner_slot_pool_release(refs, 2, slot_b);
        TEST_CHECK(a_client_second && b_owner_second, "concurrent: second release on each slot frees it");
    }

    /* --- Double release: a caller bug, not a real path in kiln_io_owner.c/
     * thermo_owner.c, but the pure function must refuse to underflow a
     * uint8_t past zero (which would wrap to 255 and make a free slot look
     * held again, effectively leaking it forever). --- */
    {
        uint8_t refs[1] = { 0 };
        int idx = owner_slot_pool_alloc(refs, 1);
        owner_slot_pool_release(refs, 1, idx);
        owner_slot_pool_release(refs, 1, idx); /* frees it -- refcount now 0 */

        bool third_release = owner_slot_pool_release(refs, 1, idx);
        TEST_CHECK(!third_release, "double release: releasing an already-free slot reports false, not freed-again");
        TEST_CHECK(refs[0] == 0, "double release: refcount stays 0 -- no underflow/wrap to 255");
    }

    /* --- Out-of-range index: refuses rather than reading/writing OOB. --- */
    {
        uint8_t refs[2] = { 1, 1 };
        TEST_CHECK(!owner_slot_pool_release(refs, 2, -1), "release() refuses a negative index");
        TEST_CHECK(!owner_slot_pool_release(refs, 2, 2), "release() refuses an index == count");
        TEST_CHECK(!owner_slot_pool_release(NULL, 2, 0), "release() refuses a NULL refcounts array");
        TEST_CHECK(owner_slot_pool_alloc(NULL, 2) == -1, "alloc() refuses a NULL refcounts array");
    }
}

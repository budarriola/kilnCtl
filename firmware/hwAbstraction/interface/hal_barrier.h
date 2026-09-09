/* hal_barrier.h -- one macro, HAL_DMB(), for the single cross-core ordering
 * primitive config_store_flash.c's seqlock needs (2026-09-09 pass: "Seqlock
 * for the RP2040 config store's RAM cache", docs/... audit on the
 * `safety_config_version` 131->132 torn-read report).
 *
 * WHY THIS EXISTS AT ALL, RATHER THAN JUST `volatile`: `s_seq_counter` being
 * `volatile` stops the COMPILER from caching it in a register or reordering
 * its own accesses relative to each other, but says nothing about the order
 * in which the CPU's write buffer retires stores to SRAM, or about the order
 * in which a DIFFERENT core's loads observe them. Both cores here are plain
 * Cortex-M0+ AHB-Lite bus masters (no data cache, no cache-coherency
 * hardware to worry about) but the AHB-Lite fabric and each core's own
 * write buffer can still retire a core's stores out of program order as
 * seen from the OTHER core. A Data Memory Barrier forces every memory
 * access before it, in program order on that core, to be visible to other
 * bus masters before any access after it is issued -- exactly the ordering
 * a seqlock's writer/reader protocol depends on. See config_store_flash.c's
 * own comment at the seqlock for which specific reordering each call site's
 * barrier rules out.
 *
 * WHY NOT A GENERAL LOCKING FRAMEWORK: there is currently no lock/critical-
 * section abstraction anywhere in firmware/hwAbstraction/, and this pass
 * does not add one. A seqlock is not a lock (the writer never waits on a
 * reader, and a reader never blocks the writer or another reader) -- it is
 * a lock-free lag-detection protocol built from one counter and ordinary
 * loads/stores, and the ONE piece of platform-specific behaviour it needs
 * is this barrier. Adding a mutex/spinlock type here to serve one call site
 * would be exactly the "general locking framework" the task's own review
 * warns against; a single macro is not that.
 *
 * Backend selection: SaftyFW's pico target builds with arm-none-eabi-gcc
 * (see firmware/SaftyFW/CMakeLists.txt's toolchain comment), which predefines
 * `__GNUC__` and `__arm__`. Rather than reach for the CMSIS `__DMB()`
 * builtin directly (which needs core_cm0plus.h pulled in through some
 * include chain this header cannot guarantee), this backend includes
 * pico-sdk's own "hardware/sync.h" and uses its documented `__dmb()`
 * wrapper -- the same header firmware/hwAbstraction/pico/uart/uart_owner.c
 * already includes for save_and_disable_interrupts(), so this is the
 * established way this codebase reaches pico-sdk's sync primitives, not a
 * new dependency. It lowers to the identical real `dmb` instruction on
 * Cortex-M0+. The host build (test/build_host_tests.ps1) uses MSVC
 * (`_MSC_VER`), which has neither of those;
 * `_ReadWriteBarrier()` (compiler-only fence, from <intrin.h>) is combined
 * with `MemoryBarrier()` (a real hardware fence on x86/x64, also
 * <intrin.h>/<windows.h>) so the host build's concurrency test (real
 * Windows threads, see test/test_config_store_flash.c) exercises an
 * equally-real cross-thread barrier rather than a no-op -- x86/x64 already
 * has strong store ordering, so MemoryBarrier() here is mainly defending
 * against the COMPILER treating the two threads' accesses as reorderable,
 * which is exactly the class of bug a torn read on real hardware also
 * depends on ruling out.
 */
#ifndef HAL_BARRIER_H
#define HAL_BARRIER_H

#if defined(__GNUC__) && defined(__arm__)
#include "hardware/sync.h" /* pico-sdk's __dmb() wrapper */
#define HAL_DMB() __dmb()
#elif defined(_MSC_VER)
#include <intrin.h>
#include <windows.h> /* MemoryBarrier() */
#define HAL_DMB() do { _ReadWriteBarrier(); MemoryBarrier(); } while (0)
#else
/* No other backend exists today (host tests are MSVC-only per build_host_
 * tests.ps1; the only other compiler in this tree is the pico arm-none-eabi-
 * gcc target above). Fail loudly at compile time rather than silently
 * defining HAL_DMB() as a no-op, which would make a seqlock that compiles
 * clean and is simply wrong. */
#error "hal_barrier.h: no HAL_DMB() backend for this compiler -- add one, do not stub it out"
#endif

#endif /* HAL_BARRIER_H */

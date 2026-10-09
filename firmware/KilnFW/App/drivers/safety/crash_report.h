// crash_report -- persists a small summary of the LAST crash (panic/fault)
// to flash, readable later from the diagnostics web page, after the ESP's
// own core-dump-to-flash mechanism (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH,
// proven working on this board 2026-08-20 -- see main.c's coredump-check
// block) has already captured the full ELF dump into the `coredump`
// partition. That full dump is for espcoredump.py on a PC; this module is
// the "what happened" answer an operator can get from the web page alone,
// without ever plugging in a laptop.
//
// SAME PATTERN AS run_state.c/.h, DELIBERATELY: a small versioned struct in
// NVS, an `acknowledged` flag, load-tolerant on corruption/size-mismatch. See
// run_state.c's header comment for the reasoning behind every one of those
// choices -- it is not repeated here. The one thing this module adds beyond
// run_state.c's shape is an explicit CRC over the record (owner requirement):
// run_state.c's record can only ever describe a *cleanly recorded* run's own
// fields, none of which come from a fault path where memory could be in a
// half-written state; a crash record, by definition, is generated on the
// return trip FROM a crash, so trusting a blob that merely deserializes
// without an integrity check is exactly the wrong default here.
//
// WHY A SEPARATE NVS KEY (not folded into run_state_record_t): same
// reasoning as run_state.c's own header comment for why IT has its own key
// separate from zones_cfg -- a corrupt/rejected crash record must never be
// able to take another module's breadcrumb down with it, and vice versa.
//
// ONE RECORD ONLY, CAPTURED ONCE PER COREDUMP: this module does not keep a
// history. It captures the CURRENT coredump image (if any) into NVS exactly
// once -- subsequent boots recognize "this is the same coredump I already
// captured" via `dump_id` (a CRC over the raw esp_core_dump_summary_t) and
// do not re-write. A NEW crash, producing a NEW coredump image, gets a new
// dump_id and is captured as a fresh record, overwriting the old one -- same
// "last one wins, no rotation" simplicity as ota_record.c's first pass.
//
// NO WALL CLOCK: same as run_state.h/ota_record.h -- this board has no RTC
// and no guaranteed SNTP, so nothing here claims an absolute crash time.
#ifndef KILNCTL_CRASH_REPORT_H
#define KILNCTL_CRASH_REPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever crash_report_record_t's layout changes. An older record is
// discarded rather than migrated -- same "one lost breadcrumb costs nothing,
// mis-parsing an old layout would print a confidently wrong answer" argument
// as run_state.h's own RUN_STATE_RECORD_VERSION.
//
// v2 -> v3 (ROADMAP.md follow-up, 2026-09-22): a pending crash record could
// not be dated from outside -- no build identity, no approximate time, no
// stable per-crash id an operator could quote. v3 adds fw_build (the
// firmware build string of the image that was RUNNING when this record was
// captured -- see fw_build's own field comment for why that is a proxy for
// "the image that crashed", not always identical to it) and
// crash_uptime_s/crash_uptime_known (a best-effort seconds-since-boot
// reading, see crash_uptime_beacon_t below). dump_id already existed in v2
// (used internally to dedupe recapture of the same coredump) but was never
// surfaced in the JSON response -- diagnostics_http.c now reports it too, no
// record-layout change needed for that part.
#define CRASH_REPORT_RECORD_VERSION 3u

// esp_core_dump_bt_info_t.bt[] (port/xtensa/esp_core_dump_summary_port.h) is
// itself capped at 16 entries -- this just mirrors that cap, not an
// independent choice.
#define CRASH_REPORT_BT_MAX 16

#define CRASH_REPORT_TASK_NAME_MAX   16  // matches esp_core_dump_summary_t.exc_task's own size
#define CRASH_REPORT_CAUSE_STR_MAX   32  // holds every xtensa exception-cause mnemonic with room to spare
#define CRASH_REPORT_RESET_STR_MAX   16  // matches main.c's own reset-reason name strings ("BROWNOUT", etc.)
#define CRASH_REPORT_FW_BUILD_MAX    40  // matches dashboard_http.h's own fw_build[40] ("Aug 20 2026 14:03:11")

// The record persisted to NVS. Explicit reserved padding, same discipline
// run_state.c's header comment insists on for its own record -- a silent
// layout change here would look like flash corruption to the loader rather
// than fail the build, so the size is pinned in crash_report.c.
typedef struct {
    uint8_t  version;          // CRASH_REPORT_RECORD_VERSION at write time
    uint8_t  acknowledged;     // operator has seen this record (see crash_report_acknowledge)
    uint8_t  bt_count;         // number of valid entries in backtrace_pc[], 0..CRASH_REPORT_BT_MAX
    uint8_t  bt_corrupted;     // esp_core_dump_bt_info_t.corrupted, verbatim
    uint32_t crc32;            // esp_crc32_le() over this struct with crc32 itself zeroed --
                               // see crash_report.c's crash_report_compute_crc() / _validate()
    uint32_t dump_id;          // CRC over the raw esp_core_dump_summary_t this record was captured
                               // from -- identifies "which coredump", so a second boot against the
                               // SAME uncleared coredump does not recapture/overwrite this record
    uint32_t exc_cause;        // esp_core_dump_summary_extra_info_t.exc_cause (xtensa EXCCAUSE)
    uint32_t exc_pc;           // esp_core_dump_summary_t.exc_pc
    uint32_t exc_addr;         // esp_core_dump_summary_extra_info_t.exc_vaddr (faulting address)
    uint32_t exc_a0;           // esp_core_dump_summary_extra_info_t.exc_a[0] -- the crashing frame's
                               // return address. See crash_report_frame_trustworthy() below.
    uint32_t exc_a1;           // esp_core_dump_summary_extra_info_t.exc_a[1] -- the crashing frame's
                               // STACK POINTER. Added 2026-09-08 (docs/audits/
                               // crash_loadprohibited_0x18_2026-09-08.md): without it, a record
                               // showing exc_cause=LoadProhibited/exc_addr=0x18 is ambiguous between
                               // "a NULL struct pointer was dereferenced at field offset 0x18" and
                               // "the STACK POINTER itself was NULL/garbage and a perfectly ordinary
                               // local at frame offset 0x18 was loaded". Those two have completely
                               // different fixes, and the summary as captured could not tell them
                               // apart -- a1 decides it in one glance.
    uint32_t backtrace_pc[CRASH_REPORT_BT_MAX]; // esp_core_dump_bt_info_t.bt[], first bt_count valid
    char     exc_task[CRASH_REPORT_TASK_NAME_MAX];      // faulting task's name
    char     exc_cause_str[CRASH_REPORT_CAUSE_STR_MAX]; // decoded EXCCAUSE mnemonic, "" if unknown
    char     reset_reason[CRASH_REPORT_RESET_STR_MAX];  // esp_reset_reason() name, e.g. "PANIC"

    // v3 additions (see CRASH_REPORT_RECORD_VERSION's comment above) --------
    uint32_t crash_uptime_s;     // best-effort seconds-since-boot near the crash, read from the
                                  // crash_uptime_beacon_t in RTC memory (see crash_report.c) at
                                  // capture time -- NOT the exact crash instant (the beacon is only
                                  // refreshed on monitor_task's heartbeat cadence, ~300 ms), and NOT
                                  // a wall-clock time (this board has no RTC/SNTP, same as every
                                  // other "NO WALL CLOCK" module -- see this header's top comment).
                                  // 0 if crash_uptime_known is 0.
    uint8_t  crash_uptime_known; // 1 if crash_uptime_s came from a beacon written by monitor_task
                                  // during the boot that crashed; 0 on a power-on reset (RTC memory
                                  // lost) or if that boot crashed before monitor_task's first
                                  // heartbeat ran -- crash_report_init() invalidates the beacon at
                                  // the start of EVERY boot, so a value left over from an earlier
                                  // boot can never be reported as this crash's uptime.
                                  // crash_uptime_s must not be read as 0 seconds when this is 0,
                                  // only as "unknown".
    char     fw_build[CRASH_REPORT_FW_BUILD_MAX]; // hal_sysinfo_get_build_info()'s date+time,
                                  // formatted the same way dashboard_http.c's own fw_build field is
                                  // ("Aug 20 2026 14:03:11") -- read from the RUNNING image at
                                  // capture time (crash_report_init() runs on the very next boot
                                  // after the crash). This is the build that CRASHED unless an OTA
                                  // or reflash happened between the crash and this boot -- the one
                                  // scenario that can make it stale is exactly the same one
                                  // ota_rollback_esp()'s hazard note (CLAUDE.md) already warns
                                  // about elsewhere, and is called out again here rather than
                                  // silently assumed.
} crash_report_record_t;

// Records "the scheduler was alive at approximately this many seconds since
// boot" into RTC memory (survives a software reset/panic/watchdog reset,
// lost only on a power cycle -- same storage class boot_guard.c's own RTC
// record uses, see that file's header comment for the full rationale on why
// RTC memory and not NVS). crash_report_init() reads this back on the NEXT
// boot as a best-effort stand-in for "uptime at the crash", since neither
// esp_core_dump_summary_t nor this board's NVS store anything of the kind
// today. Intended caller: monitor_task.c's existing heartbeat loop (already
// runs on a fixed, frequent cadence with no NVS/flash touch of its own) --
// no new task, no new periodic writer. Cheap (two RTC-memory word writes,
// no I/O); safe to call from any task, any frequency.
void crash_report_note_alive(void);

// Called ONCE at boot, early, AFTER kiln_nvs (KILN_NVS_PARTITION) is
// reachable -- see main.c's call site next to its esp_core_dump_image_check()
// block. If esp_core_dump_image_check() reports a valid coredump AND its
// dump_id differs from any already-stored record's, captures a new record
// via esp_core_dump_get_summary() and persists it (acknowledged=0). If a
// record for the SAME coredump already exists, does nothing (no re-write).
// If no coredump is present, does nothing. Safe to call more than once;
// never fails app_main -- every error is logged and swallowed, same
// convention as run_state_init()/board_temps_start().
void crash_report_init(void);

// Copies the currently stored record (whether or not it has already been
// acknowledged -- unlike run_state_get_boot_record(), the diagnostics page
// needs to keep showing an acknowledged crash record, just annotated as
// read). Returns false (and zeroes *out) if there is no valid record --
// never written, corrupted (CRC mismatch), or a size/version this build
// doesn't recognize.
bool crash_report_get(crash_report_record_t *out);

// Cached, no-I/O version of "have_record && !acknowledged" (the same
// condition readiness_crash_report_status() gates a firing on -- see
// readiness_http.h). Added 2026-09-15 (docs/audits/
// manual_relay_readiness_gating_options_2026-09-15.md, option B) so
// kiln_io_owner.c's relay_on_blocked() -- the single choke point for every
// MANUAL relay-ON write (LCD override, benchproto SET_RELAY, the danger
// relay route, the CT sweep) -- can refuse while an unacknowledged crash
// report exists WITHOUT doing NVS I/O from inside the relay path: unlike
// crash_report_get() above, this reads an in-RAM flag maintained by
// crash_report_init() (set once at boot, before kiln_io_owner_start() is
// ever called -- see main_boot_early.c's call order) and refreshed by
// crash_report_acknowledge()/crash_report_clear() whenever either succeeds.
// Reads false before crash_report_init() has run (including throughout
// RECOVERY MODE boot, which still calls it -- boot_guard_init() runs AFTER
// crash_report_init() in main_boot_early.c) -- the same "no record yet"
// default crash_report_get() itself returns, never a stale true left over
// from a previous boot's cache. Never does I/O; safe to call from any task,
// with no lock held.
bool crash_report_has_unacknowledged(void);

// True only if the exception frame this record was built from looks
// self-consistent enough that exc_pc/exc_addr may be reasoned about.
//
// WHY THIS EXISTS (2026-09-08): esp_core_dump_summary_t.exc_pc is not the raw
// saved PC -- espcoredump runs it through esp_cpu_process_stack_pc(), which
// returns `pc - 3`. A stored exc_pc of 0xfffffffd therefore means the saved
// PC was EXACTLY 0x00000000, i.e. the frame's PC field is not a code address
// at all. A record in that state was read, on this board, as though its
// exc_addr were a meaningful struct field offset; it is not safe to do that,
// because a frame whose PC is zero has no established relationship to its own
// exccause/vaddr fields. Pure predicate over the record, no I/O -- host-tested.
bool crash_report_frame_trustworthy(const crash_report_record_t *rec);

// Sets the acknowledged flag on the stored record and persists it. Returns
// false if there is no valid record to acknowledge.
bool crash_report_acknowledge(void);

// Bounded-wait sibling of crash_report_acknowledge(), for a caller (the LCD
// diagnostics page's Acknowledge control, on lvgl_task) that must not block
// indefinitely behind some OTHER caller's long flash-worker job -- see
// crash_report.c's own doc comment (MEDIUM 1,
// docs/audits/review_crash_gate_low_fixes_c534a0df_2026-09-15.md). Waits at
// most timeout_ms to become the next job on the flash worker; if that wait
// itself times out, returns false and sets *out_timed_out (if non-NULL) to
// true, distinct from every other false-returning case (no record; a
// dispatched write that failed) -- nothing was read or written in the
// timeout case. Every other caller (diagnostics_http.c) should keep using
// the unbounded crash_report_acknowledge() above.
bool crash_report_acknowledge_timeout(uint32_t timeout_ms, bool *out_timed_out);

// Acknowledges (see above) AND erases the underlying coredump image via
// esp_core_dump_image_erase(), freeing the `coredump` partition slot for the
// next crash, then erases this module's own NVS record too so a stale
// record can never outlive the coredump it described. Returns ESP_OK only
// if both steps succeed; the record and/or coredump may be partially cleared
// on a partial failure, which is logged.
esp_err_t crash_report_clear(void);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_CRASH_REPORT_H

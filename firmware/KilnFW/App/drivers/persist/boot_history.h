// boot_history -- FD-03: persisted ring of the last BOOT_HISTORY_MAX boots.
//
// One NVS blob (kiln_cfg/boot_hist in KILN_NVS_PARTITION), rewritten ONCE per
// boot (FW-01: identical consecutive boots are coalesced into the last entry's
// repeat count and persisted only every BOOT_HISTORY_FLUSH_EVERY repeats) by boot_history_record_boot() after the reset reason is known: that is
// the whole flash-wear budget (1 write per boot, no heartbeat writes). The
// previous boot's uptime is NOT tracked here; the caller passes the RTC-memory
// beacon value crash_report.c already snapshots (known flag false => unknown).
// Wall-clock time is 0 when unknown (no RTC; SNTP is usually not up this early).
#ifndef KILNCTL_BOOT_HISTORY_H
#define KILNCTL_BOOT_HISTORY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOOT_HISTORY_MAX 16
#define BOOT_HISTORY_VERSION 2u /* v2 (FW-01): entry gained a repeat count; a v1 blob is discarded cleanly */
#define BOOT_HISTORY_FLUSH_EVERY 16u /* FW-01: coalesced identical boots persist every Nth repeat */
#define BOOT_HISTORY_FW_MAX 13 /* up to 12 chars of commit short hash + NUL */

typedef struct {
    uint32_t seq;            /* boot sequence number, 1-based, strictly increasing */
    uint32_t dump_id;        /* crash_report dump_id if a crash record exists, else 0 */
    uint32_t prev_uptime_s;  /* uptime of the previous boot near its end; 0 if !prev_uptime_known */
    uint32_t wall_time;      /* unix seconds, 0 = unknown */
    char     fw[BOOT_HISTORY_FW_MAX]; /* firmware commit short hash ("" if unknown) */
    uint8_t  reset_reason;   /* hal_reset_reason_t numeric value */
    uint8_t  prev_uptime_known;
    uint8_t  has_crash;      /* 1 if a crash record existed at this boot */
    uint16_t repeat;         /* FW-01: further identical boots (same reason + fw) folded into this entry */
} boot_history_entry_t;      /* 4*4 + 13 + 3 + 2 (+2 pad) = 36 bytes */

typedef struct {
    uint8_t  version;
    uint8_t  count;   /* valid entries, 0..BOOT_HISTORY_MAX */
    uint8_t  head;    /* index the NEXT entry is written to */
    uint8_t  reserved;
    uint32_t next_seq;
    uint32_t crc32;   /* esp_crc32_le over the struct with crc32 zeroed */
    boot_history_entry_t entries[BOOT_HISTORY_MAX];
} boot_history_blob_t;

/* Pure ring logic (host-tested). */
bool boot_history_blob_valid(const boot_history_blob_t *b);
void boot_history_blob_init(boot_history_blob_t *b);               /* empty, next_seq = 1 */
void boot_history_blob_push(boot_history_blob_t *b, const boot_history_entry_t *e); /* assigns seq, wraps */
/* i = 0 is the newest. Returns false when i >= count. */
bool boot_history_blob_get(const boot_history_blob_t *b, unsigned i, boot_history_entry_t *out);
/* "-g<hash>" token of a git-describe string (up to 12 chars), else the leading
 * 12 chars of the string; always NUL-terminated, alnum and . _ - only. */
void boot_history_short_hash(const char *version, char out[BOOT_HISTORY_FW_MAX]);
const char *boot_history_reason_name(uint8_t hal_reset_reason);

/* BH-02: sets wall_time on THIS boot's entry (the one boot_history_record_boot() wrote) if it is
 * still 0. One NVS write. Call from task context at most once per boot (first SNTP sync).
 * ESP_ERR_INVALID_STATE when this boot recorded no entry or the entry already has a wall time. */
esp_err_t boot_history_backfill_wall_time(uint32_t wall_time);

/* Loads the ring; a missing/corrupt/wrong-version blob yields an empty ring
 * (returns false then). */
bool boot_history_load(boot_history_blob_t *out);

/* In-RAM copy of the newest BOOT_HISTORY_RECENT entries, filled by
 * boot_history_record_boot() (no NVS read, safe from the LVGL task). Returns
 * the number of entries copied, newest first; 0 before the first record. */
#define BOOT_HISTORY_RECENT 5
unsigned boot_history_recent(boot_history_entry_t out[BOOT_HISTORY_RECENT]);
/* seq of the newest in-RAM entry (this boot), 0 when none. No copy: safe on small stacks. */
uint32_t boot_history_latest_seq(void);
/* FV-01: low 8 bits of the newest entry's coalesced-repeat count. With boot_history_latest_seq() it
 * identifies THIS boot uniquely even when the boot was folded into the previous entry. */
uint8_t boot_history_latest_rep(void);

/* Appends this boot and persists with exactly one write. Call once per boot
 * from the early-boot sequence. Returns ESP_OK or the write error (the boot
 * continues either way). */
esp_err_t boot_history_record_boot(uint8_t hal_reset_reason, bool prev_uptime_known,
                                   uint32_t prev_uptime_s, uint32_t dump_id, bool has_crash,
                                   const char *fw_version, uint32_t wall_time);

#ifdef __cplusplus
}
#endif

#endif

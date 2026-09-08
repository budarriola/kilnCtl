// flash_layout.h -- the frozen RP2040 flash map, docs/BOOTLOADER.md section 2.
// Pure constants only, no code, no SDK/RTOS dependency -- host-includable,
// same discipline as src/board/board_pins.h ("a plain header, not a driver").
//
// These offsets are a commitment (docs/BOOTLOADER.md's own header comment:
// "once a bootloader is written to a board over SWD it is not going to be
// changed in the field"). Nothing has been flashed to real hardware yet
// (docs/ARCHITECTURE.md's own completion checklist: "never flashed or run on
// real hardware" as of this file's creation), which is exactly why this is
// the moment to get it right, not a reason to treat it as casually editable
// later. If you are changing a value in this file, you are almost certainly
// changing docs/BOOTLOADER.md section 2 in the same commit, not the other
// way around -- that document is the source of truth this file implements.
//
// All offsets are relative to the start of flash (0x00000000), matching
// pico-sdk's hardware/flash.h API (flash_range_erase()/flash_range_program()
// take flash-relative offsets, not XIP-mapped addresses). Add XIP_BASE
// (0x10000000, from pico-sdk's hardware/regs/addressmap.h) to get an
// execute-in-place pointer.
#ifndef SAFTYFW_BOOTLOADER_FLASH_LAYOUT_H
#define SAFTYFW_BOOTLOADER_FLASH_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Total flash on this board: 2MB, fixed by PICO_BOARD=pico's pico-sdk board
// definition (PICO_FLASH_SIZE_BYTES in boards/pico.h) -- TODO.md Phase 10
// item 10.1, confirmed 2026-08-17, not a bench measurement (the RP2040
// Pico's onboard flash is a fixed hardware fact of the stock board this
// project targets, unlike the ESP32-S3 third-party board's genuinely
// ambiguous flash size).
#define BOOTLOADER_FLASH_TOTAL_SIZE 0x00200000u // 2 MiB

// boot2 (0x00000000..0x00000100, 256B): vendor second-stage loader, not part
// of this build -- picked up automatically by pico-sdk's own boot_stage2/
// linker machinery. Not represented here; nothing in this codebase writes it.

// Bootloader region: written once over SWD, never updated in the field
// (docs/BOOTLOADER.md section 1's central constraint).
#define BOOTLOADER_FLASH_OFFSET      0x00000100u
#define BOOTLOADER_FLASH_SIZE        0x0000FF00u // ~64K (63.75K exactly)

// Public-key reservation (docs/BOOTLOADER.md section 6): unused today
// (signing is off), reserved at a fixed offset near the end of the
// bootloader region so the bootloader's own code/.data size can grow
// without moving it. 768 B is sized against Ed25519 (32 B) with headroom
// for a larger scheme (e.g. RSA-2048 at 256 B, or more than one key) --
// which algorithm, and whether more than one key is reservable, is
// deliberately unpicked (docs/BOOTLOADER.md section 6). This is a
// reservation only: nothing in this codebase reads or writes it, and no
// verification logic exists.
#define BOOTLOADER_PUBKEY_FLASH_SIZE   0x00000300u // 768 B
#define BOOTLOADER_PUBKEY_FLASH_OFFSET \
    (BOOTLOADER_FLASH_OFFSET + BOOTLOADER_FLASH_SIZE - BOOTLOADER_PUBKEY_FLASH_SIZE) // 0x0000FD00

// Metadata region: ONE 4K sector (not two, despite docs/BOOTLOADER.md's
// original "two copies... written alternately" wording -- see this file's
// metadata.h header comment for why that phrasing describes a scheme that
// is not actually safe within a single erase-granularity sector, and what
// replaces it without moving this offset or size).
#define BOOTLOADER_METADATA_FLASH_OFFSET 0x00010000u
#define BOOTLOADER_METADATA_FLASH_SIZE   0x00001000u // 4K, one erase sector

// Application slots: A and B, identical size, both erase-block aligned
// (4K sectors, and comfortably 64K-block aligned too: 0x11000 and 0xE1000
// are both multiples of 0x1000).
#define BOOTLOADER_SLOT_A_FLASH_OFFSET 0x00011000u
#define BOOTLOADER_SLOT_B_FLASH_OFFSET 0x000E1000u
#define BOOTLOADER_SLOT_FLASH_SIZE     0x000D0000u // 832K each

// Config: outside both slots, so an update never touches it. Nothing in
// this bootloader ever writes here -- docs/BOOTLOADER.md section 3, "what
// it must never do".
#define BOOTLOADER_CONFIG_FLASH_OFFSET 0x001B1000u
#define BOOTLOADER_CONFIG_FLASH_SIZE   0x00010000u // 64K

// src/config_store.{c,h} (TODO.md Phase 9): one 4K erase sector carved from
// the front of the 64K config region above -- the config store is an
// application-layer concern (like BOOTLOADER_CONFIG_FLASH_SIZE's own comment
// says, this bootloader never writes here), so it gets a named sub-region
// rather than a new top-level offset. Sized like BOOTLOADER_METADATA_FLASH_
// SIZE (one sector, 8 slots of CONFIG_STORE_RECORD_LEN=512B each as of
// format version 2 -- was 16 slots of 256B under format version 1; see
// config_store.h's header comment) -- still ample, since a record is
// written only at commissioning, not at runtime, with 60K of the config
// region still unclaimed after this for whatever comes next.
#define SAFTYFW_CONFIG_STORE_FLASH_OFFSET BOOTLOADER_CONFIG_FLASH_OFFSET
#define SAFTYFW_CONFIG_STORE_FLASH_SIZE   0x00001000u // 4K, one erase sector

// Flash endurance review 2026-09-07 (docs/audits/flash_endurance_review_
// 2026-09-07.md, R2): sector A above is an 8-slot append-only log, but it is
// the ONLY copy -- when its 8th write wraps, config_store_flash.c must erase
// the whole sector before it can program slot 0 again, and between that
// erase and the next successful program there are ZERO valid copies of the
// safety configuration on this board. A power cut in that window loses TC
// type, abs_max_temp_c, CT cal outright.
//
// Sector B is the fix: a second, identical 4K sector immediately following
// sector A, carved from the SAME 64K BOOTLOADER_CONFIG_FLASH_SIZE region that
// was already reserved for this and had 60K unused (15 spare sectors) --
// no flash-layout move, no bootloader change. config_store_flash.c now
// round-robins its 8-slot log inside whichever of A/B is "active", and only
// SWITCHES to the other sector (erase-then-program, verified, before the
// old sector is ever touched) when the active one fills up -- see that
// file's header comment for the full write/switch/arbitration design. This
// halves the per-sector erase rate as a free side effect (the endurance
// review's R2 wear-levelling bonus), but the reason this exists is
// atomicity, not wear: at least one of A/B always holds a complete,
// CRC-valid record, at every instant, including mid-erase and mid-program of
// the other one.
#define SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B \
    (SAFTYFW_CONFIG_STORE_FLASH_OFFSET + SAFTYFW_CONFIG_STORE_FLASH_SIZE)
#define SAFTYFW_CONFIG_STORE_NUM_SECTORS 2u

// Reserved headroom: 0x1B2000+0x1000 (sector B) ..0x1C2000, 56K, plus
// 0x1C2000..0x200000, 252K. Not represented by a macro here -- nothing
// addresses it yet, and giving unclaimed space a name invites something to
// start using it without a doc update.

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_BOOTLOADER_FLASH_LAYOUT_H

// own_ok_persist.h -- AT-15: keeps the S5 own-TC ride-through allowance across a BENIGN Pico
// reboot of an active firing (owner 2026-11-19), in watchdog scratch slot 5 under tag 0xC7.
// Slot 5 is the fatal-fault latch register: a fatal hook or overdue latch overwrites this word,
// so a fatal reboot loses the allowance by construction; a power-on zeroes it.
#ifndef SAFTYFW_OWN_OK_PERSIST_H
#define SAFTYFW_OWN_OK_PERSIST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OWN_OK_PERSIST_MAGIC_BYTE 0xC7u

uint32_t own_ok_persist_encode(bool on);
bool     own_ok_persist_decode(uint32_t word); // true only for the exact tagged "on" word

// Boot, before watchdog_overdue_diag_clear(): reads slot 5; the allowance is restored only when
// the previous boot was benign (not a watchdog timeout, no latched trip).
void own_ok_persist_boot_read(bool benign_boot);
bool own_ok_persist_restored(void);

// Runtime, on a change of the guard's allowance. Writing false only clears our own word, never
// a fatal latch.
void own_ok_persist_write(bool on);

#ifdef __cplusplus
}
#endif
#endif

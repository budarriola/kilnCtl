Fake backends for MSVC host tests, shared by both firmwares (fake_spi,
fake_i2c, fake_uart, fake_gpio, fake_adc, fake_kv, fake_time, fake_flash,
fake_scratch, fake_wdt, fake_pwm, fake_sysinfo).

Filled in Phase 2 of docs/HW_ABSTRACTION_PLAN.md, replacing the stub-header
include-path trick interface by interface. Empty as of Phase 0.

fake_gpio/fake_adc/fake_uart landed 2026-09-05 (per-pin level+direction
history, scripted ADC samples, and a real TX-capture/RX-ring UART model with
drop counters); fake_spi (pump-driven async completions, wedge latch on
async-pool exhaustion, distinct enqueue/completion-timeout injection) and
fake_i2c (per-address ack/nack scripting so hal_i2c_probe is testable,
ordered transfer record, scripted rx) landed the same day. fake_kv (RAM
namespace/key store per partition, pending-vs-committed durability model
with fake_kv_simulate_power_loss() and wrong-type/corruption/no-space error
injection) and fake_time (manually advanced clock, forward-only, delay_ms
advances it instead of sleeping) landed once hal_kv.h/hal_time.h existed.
fake_flash (in-memory sector image, erased-state 0xFF, program-only-clears-
bits AND semantics since hal_flash.h does not pin erase-before-program
enforcement at the interface level, per-sector erase counts for wear
assertions, injectable per-operation failure, and
fake_flash_simulate_power_loss_during() for a half-written page) landed once
hal_flash.h existed. fake_scratch (claim table keyed on (slot, tag), slot-4
hard reservation, reset-vs-power-loss distinction -- watchdog resets keep
slot values, power loss clears them), fake_wdt (records init cfg and feed
count, fake_wdt_advance_ms() latches a sticky fired flag once feeds lapse
past timeout, reboot request latch instead of an accurate never-returns),
and fake_pwm (captures init cfg and a bounded duty-set history, rejects
out-of-range duty) landed once hal_scratch.h/hal_wdt.h/hal_pwm.h existed;
fake_sysinfo (scriptable reset reason/partition/build info/temperature/
random sequence/coredump presence, with coredump erase clearing presence)
landed the same way once hal_sysinfo.h existed. Standalone MSVC tests +
negative tests for all twelve live in ../test/test_host_fakes.ps1 (not yet
wired into either firmware's build_host_tests.ps1 -- that's the Option A
response-file switch still to do); the script now builds with /WX alongside
/W3 -- host/fake_kv.c's strncpy calls (formerly C4996 under /WX) were
replaced with a bounded memcpy+NUL helper (copy_bounded()), no
_CRT_SECURE_NO_WARNINGS needed.

Fake backends for MSVC host tests, shared by both firmwares (fake_spi,
fake_i2c, fake_uart, fake_gpio, fake_adc, fake_kv, fake_time, fake_flash).

Filled in Phase 2 of docs/HW_ABSTRACTION_PLAN.md, replacing the stub-header
include-path trick interface by interface. Empty as of Phase 0.

fake_gpio/fake_adc/fake_uart landed 2026-09-05 (per-pin level+direction
history, scripted ADC samples, and a real TX-capture/RX-ring UART model with
drop counters); fake_spi (pump-driven async completions, wedge latch on
async-pool exhaustion, distinct enqueue/completion-timeout injection) and
fake_i2c (per-address ack/nack scripting so hal_i2c_probe is testable,
ordered transfer record, scripted rx) landed the same day. fake_kv/fake_time/
fake_flash remain unstarted -- interface/ has no hal_kv.h/hal_time.h/
hal_flash.h yet, and a host fake must follow an existing interface header
rather than invent one. Standalone MSVC tests + negative tests for all five
live in ../test/test_host_fakes.ps1 (not yet wired into either firmware's
build_host_tests.ps1 -- that's the Option A response-file switch still to do).

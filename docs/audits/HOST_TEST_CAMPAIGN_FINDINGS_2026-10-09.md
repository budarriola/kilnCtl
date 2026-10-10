# Host test campaign findings, 2026-10-09

Entries are appended by campaign agents. Defects found while writing host tests;
the failing cases are NOT committed (the test file carries a comment naming the finding).

## Campaign 7: kiln_io.c / SX1509.c relay command path

Test: `firmware/KilnFW/App/test/test_kiln_io_sx_fake.c` (real `kiln_io.c` + real
`SX1509.c` over a register-level fake SX1509 behind `i2c_master_transmit*`).
Not testable on host: owner-task queue/lock interleaving (FreeRTOS stubs never deliver).
All three confirmed by probe runs of the fake.

### K7-01 (MEDIUM): write lands, transfer reports error, coil left energised but module says OFF
- Where: `drivers/hw/SX1509.c` raw write / `write16_verified` (shadow updates only on ESP_OK); `drivers/owners/kiln_io.c` `kiln_io_set_relay` error path.
- Input: `kiln_io_set_relay(io, 1, true)` while the bus lands the write then reports `ESP_ERR_TIMEOUT` (lost ACK).
- Observed: returns an error, `relay_shadow == 0`, chip relay 1 energised.
- Expected: shadow reflects the chip (or a safe-off write is attempted) so module state never says OFF while a coil is driven.

### K7-02 (LOW/MEDIUM): stuck-high latch bit: OFF fails read-back, shadow says OFF
- Where: `kiln_io.c:142` `kiln_io_resync_relay_shadow` (adopts driver data shadow, not a chip read-back), called from the failure paths at `kiln_io.c:302`/`:328`.
- Input: relay 1 on, latch bit 0 stuck high, `kiln_io_set_relay(io, 1, false)`.
- Observed: `ESP_ERR_INVALID_RESPONSE` returned, `relay_shadow == 0`, chip still energised.
- Expected: resync reads the chip, so `relay_shadow` reports ON.

### K7-03 (HIGH): ON after expander soft reset reports success but drives nothing
- Where: `SX1509.c` `SX1509_reset` (all pins back to input, POR shadows); owner `CMD_SX_RESET` at `kiln_io_owner.c:554` sets `relay_shadow = 0` and does not re-drive dir/latches.
- Input: `SX1509_reset(&exp, false)`, then `kiln_io_set_relay(io, 1, true)`.
- Observed: returns `ESP_OK`, `relay_shadow == 1`, chip dir `0xFFFF` (relay pins inputs), relay not energised.
- Expected: relay pins re-configured as outputs after reset (re-init), or the command fails; never OK with nothing driven.

### K7-04 (LOW): after chip POR, `kiln_io_all_relays_off` returns an error and never repairs direction
- Where: `kiln_io.c` all-relays-off path versus a stale dir shadow.
- Input: relay 1 on, chip POR behind the driver's back, `kiln_io_all_relays_off`.
- Observed: `ESP_ERR_INVALID_RESPONSE` (264), chip dir `0xFFFF` left as POR default.
- Expected: either ESP_OK with outputs re-driven off, or a re-init. (Pins as inputs are de-energised, so this is a safe state, only reported as failure and not repaired.)

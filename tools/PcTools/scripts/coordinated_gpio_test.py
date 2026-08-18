#!/usr/bin/env python3
"""Coordinated two-board GPIO test -- `tools/PcTools/TODO.md` capability 1c,
implementing `firmware/SaftyFW/docs/HARDWARE.md` section 1, Steps A and B.

Verifies the isolated safety-link UART wiring (ESP GPIO4/GPIO5 <-> Pico
GPIO4/GPIO5) by direct pin control, independent of both firmwares' UART
peripherals -- this is the only test in the project that proves the crossing
rather than assuming it from the schematic.

Each processor is reached by a path that is *not* the link under test:
  - ESP:  GPIO_PROBE task (12) over its USB-serial PC link (`kilnctrl.probe`)
  - Pico: raw RP2040 SIO/IO_BANK0 register pokes over SWD (`kilnctrl.debug_probe`)

Step C (the fault line, ESP GPIO6) is deliberately NOT implemented here: GPIO6
is permanently on the ESP probe's deny-list (`gpio_probe.c`) because a debug
tool that can drive it can silently misrepresent the main controller's health
to the safety processor -- unlike GPIO4/5, that one hazard is not worth an
exception. HARDWARE.md already calls that crossing "undisputed" from the
schematic evidence alone.

Both boards must be powered (main board 12V *and* 12V_Safty) and connected:
ESP over its normal USB-serial link, Pico over its SWD debug probe.

Usage:
    uv run --project tools/PcTools python tools/PcTools/scripts/coordinated_gpio_test.py
"""

from __future__ import annotations

import re
import sys
import time

sys.path.insert(0, __file__.rsplit("scripts", 1)[0] + "src")

from kilnctrl import debug_probe, probe
from kilnctrl.devices import GpioProbeRefused
from kilnctrl.link_hub import get_shared_link

# --- RP2040 register map (RP2040 datasheet ch. 2.3 SIO, ch. 2.19 IO_BANK0) --
IO_BANK0_BASE = 0x40014000
SIO_BASE = 0xD0000000

SIO_GPIO_IN = SIO_BASE + 0x004
SIO_GPIO_OUT_SET = SIO_BASE + 0x014
SIO_GPIO_OUT_CLR = SIO_BASE + 0x018
SIO_GPIO_OE_SET = SIO_BASE + 0x024
SIO_GPIO_OE_CLR = SIO_BASE + 0x028

GPIO_FUNC_SIO = 5


def gpio_ctrl_addr(gpio_num: int) -> int:
    return IO_BANK0_BASE + 8 * gpio_num + 4


def pico_gpio_to_sio(peer: str, gpio_num: int) -> None:
    """Detach gpio_num from whatever peripheral (UART1) owns it and hand it
    to the plain software-controlled SIO block, as an input (OE clear)."""
    ok, out = debug_probe.write_memory(peer, gpio_ctrl_addr(gpio_num), GPIO_FUNC_SIO, width=32)
    if not ok:
        raise RuntimeError(f"pico gpio{gpio_num} funcsel->SIO failed: {out}")
    ok, out = debug_probe.write_memory(peer, SIO_GPIO_OE_CLR, 1 << gpio_num, width=32)
    if not ok:
        raise RuntimeError(f"pico gpio{gpio_num} OE clear failed: {out}")


def pico_gpio_set_output(peer: str, gpio_num: int, level: bool) -> None:
    ok, out = debug_probe.write_memory(peer, SIO_GPIO_OE_SET, 1 << gpio_num, width=32)
    if not ok:
        raise RuntimeError(f"pico gpio{gpio_num} OE set failed: {out}")
    addr = SIO_GPIO_OUT_SET if level else SIO_GPIO_OUT_CLR
    ok, out = debug_probe.write_memory(peer, addr, 1 << gpio_num, width=32)
    if not ok:
        raise RuntimeError(f"pico gpio{gpio_num} drive {'high' if level else 'low'} failed: {out}")


def pico_gpio_read(peer: str, gpio_num: int) -> bool:
    ok, out = debug_probe.read_memory(peer, SIO_GPIO_IN, count=1, width=32)
    if not ok:
        raise RuntimeError(f"pico gpio_in read failed: {out}")
    # debug_probe.read_memory prints "MEMRD 0xADDR 0xVALUE" (see its own
    # docstring for why not mdw's raw display output).
    match = re.search(r"MEMRD\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)", out)
    if not match:
        raise RuntimeError(f"could not parse read_memory output for gpio_in: {out!r}")
    value = int(match.group(1), 16)
    return bool(value & (1 << gpio_num))


def esp_setup(link) -> probe.ProbeClient:
    return probe.ProbeClient(link)


_ESP_TIMEOUT_S = 5.0


def esp_drive(p: probe.ProbeClient, gpio_num: int, level) -> None:
    if level is None:
        p.set_mode(gpio_num, probe.MODE_INPUT, timeout=_ESP_TIMEOUT_S)
        return
    p.set_mode(gpio_num, probe.MODE_OUTPUT, timeout=_ESP_TIMEOUT_S)
    p.write(gpio_num, level, timeout=_ESP_TIMEOUT_S)


def main() -> int:
    print("=== Coordinated two-board GPIO test (HARDWARE.md section 1, Steps A/B) ===")
    print("Reminder: SWD to the Pico bonds GND_Safty to PC ground for the duration.")
    print("Bench only, no load wiring. Both boards must be powered.\n")

    link = get_shared_link()
    esp = esp_setup(link)

    results = {}

    try:
        print("--- Pre-flight: halting Pico, detaching GPIO4/5/10 from firmware ---")
        debug_probe.halt(debug_probe.PEER_PICO)
        for pin in (4, 5, 10):
            pico_gpio_to_sio(debug_probe.PEER_PICO, pin)

        print("\n--- Step A: ESP drives GPIO4, Pico reads GPIO5 (+GPIO10 baseline) ---")
        esp_drive(esp, 5, None)  # ESP GPIO5 input, own RX -- expect no change
        for level in (True, False):
            esp_drive(esp, 4, level)
            time.sleep(0.05)
            pico5 = pico_gpio_read(debug_probe.PEER_PICO, 5)
            pico10 = pico_gpio_read(debug_probe.PEER_PICO, 10)
            esp5 = esp.read(5, timeout=_ESP_TIMEOUT_S)
            print(f"  ESP GPIO4={'HIGH' if level else 'LOW '}  ->  Pico GPIO5={'HIGH' if pico5 else 'LOW '}  "
                  f"Pico GPIO10={'HIGH' if pico10 else 'LOW '}  ESP GPIO5(own RX)={'HIGH' if esp5 else 'LOW '}")
            results[f"stepA_esp4_{level}_pico5"] = pico5
        esp_drive(esp, 4, False)  # leave idle low afterward

        print("\n--- Step B: Pico drives GPIO4, ESP reads GPIO4 and GPIO5 ---")
        esp_drive(esp, 4, None)  # ESP GPIO4 input now
        esp_drive(esp, 5, None)  # ESP GPIO5 input (own RX)
        for level in (True, False):
            pico_gpio_set_output(debug_probe.PEER_PICO, 4, level)
            time.sleep(0.05)
            esp4 = esp.read(4, timeout=_ESP_TIMEOUT_S)
            esp5 = esp.read(5, timeout=_ESP_TIMEOUT_S)
            print(f"  Pico GPIO4={'HIGH' if level else 'LOW '}  ->  ESP GPIO4={'HIGH' if esp4 else 'LOW '}  "
                  f"ESP GPIO5={'HIGH' if esp5 else 'LOW '}")
            results[f"stepB_pico4_{level}_esp5"] = esp5
        pico_gpio_set_output(debug_probe.PEER_PICO, 4, False)  # leave idle low

    finally:
        print("\n--- Restoring normal operation ---")
        esp.close()
        print("Resetting Pico to restore SaftyFW's own UART1 init...")
        debug_probe.reset(debug_probe.PEER_PICO, mode="run")
        print("Resetting ESP to restore safety_link's own UART1 init...")
        debug_probe.reset(debug_probe.PEER_ESP, mode="run")

    print("\n=== Reading the result (HARDWARE.md section 1) ===")
    a_high = results.get("stepA_esp4_True_pico5")
    a_low = results.get("stepA_esp4_False_pico5")
    b_high = results.get("stepB_pico4_True_esp5")
    b_low = results.get("stepB_pico4_False_esp5")

    doc_says_ok = (a_high is False) and (a_low is True) and (b_high is False) and (b_low is True)
    if doc_says_ok:
        print("MATCHES HARDWARE.md: ESP GPIO4->Pico GPIO5 and Pico GPIO4->ESP GPIO5 both invert.")
        print("Config (TX_IO=GPIO4, RX_IO=GPIO5, inversion on the ESP side) is correct as documented.")
    else:
        print("DOES NOT MATCH HARDWARE.md's prediction -- do not paper over this.")
        print(f"  Step A: ESP4=HIGH -> Pico5={a_high!r} (doc expects False/LOW)")
        print(f"  Step A: ESP4=LOW  -> Pico5={a_low!r} (doc expects True/HIGH)")
        print(f"  Step B: Pico4=HIGH -> ESP4={b_high!r} (doc expects False/LOW)")
        print(f"  Step B: Pico4=LOW  -> ESP4={b_low!r} (doc expects True/HIGH)")
        print("Re-check HARDWARE.md section 1's optocoupler trace against these measurements.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

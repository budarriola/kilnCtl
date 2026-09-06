"""Routing at the link_hub layer.

See TODO.md "Two peers": the wire protocol (protocol.Device) only ever
distinguishes ESP and HOST -- the RP2040 safety processor is not a third
addressable Device, it is reached as a task (UART_TASK_ID_SAFETY) relayed
through the ESP. `_as_device` is the one function in link_hub.py that turns a
client-supplied integer into a routing decision, so it is the right place to
pin that down: a request naming an out-of-range device id must be passed
through honestly (and fail downstream) rather than silently coerced onto a
device that happens to exist, which would hide exactly the kind of "peer"
confusion a future SAFETY-as-a-Device change could introduce.
"""
from kilnctrl.link_hub import _as_device
from kilnctrl.protocol import Device


def test_as_device_known_values_map_to_enum():
    assert _as_device(int(Device.ESP)) is Device.ESP
    assert _as_device(int(Device.HOST)) is Device.HOST


def test_as_device_unknown_value_passes_through_unchanged():
    # Negative case: there is no Device.SAFETY. An unknown id (e.g. a stray
    # "peer" constant from a future change) must come back as the same raw
    # int, not silently remapped to Device.ESP -- that would make a routing
    # bug on the client side invisible on the hub side.
    assert _as_device(7) == 7
    assert not isinstance(_as_device(7), Device)

// usb_descriptors.c -- TinyUSB device/config/string descriptor callbacks for
// SimFW's dual-CDC composite device (2026-08-23: a second CDC added
// alongside the original benchproto-protocol CDC, dedicated to console/log
// output -- see this file's "Configuration Descriptor" section below for the
// two-IAD layout and usb_owner.c's header comment for why the two streams
// must never share one wire). Part of usb_owner's exclusive USB domain (see
// usb_owner.h's header comment): this file and usb_owner.c are the only two
// translation units that include tusb.h. Split out from usb_owner.c itself
// only because TinyUSB's descriptor callbacks are conventionally their own
// file (every example under lib/tinyusb/examples/device/ does the same) --
// there is no functional reason cmd_task.c or any other task file would ever
// need to touch this.
//
// Modeled on lib/tinyusb/examples/device/cdc_msc_freertos/src/usb_descriptors.c
// (see tusb_config.h's header comment for why that example is the closest
// precedent in this toolchain), trimmed to CDC-only -- no MSC, no
// bsp/board_api.h board-support layer (SimFW does not link tinyusb_board;
// there is no LED-blink/board_init() need here, only the CDC descriptors
// themselves) and no high-speed device_qualifier path (CFG_TUD_MAX_SPEED is
// pinned to OPT_MODE_FULL_SPEED in tusb_config.h -- the RP2040 has no
// high-speed PHY).
#include "tusb.h"

#include <string.h>

#include "pico/unique_id.h"

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+

// A bench-fixture-specific VID/PID pair has not been formally allocated
// (this is a one-off test fixture, never mass produced), but multiple
// RP2040-class devices sit on the same bench at once -- this fixture Pico,
// the spi_test_master reference Pico (tools/spi_test_master/), and the
// safety processor's Debug Probe -- so a shared/generic ID is a real hazard,
// not a cosmetic one: `kilnsim`'s PC-side auto-detect
// (tools/PcTools/src/kilnsim/link.py) could silently talk to, or try to
// reset, the wrong device. Per docs/HARDWARE.md's "USB identity" section
// (the claimed-ID authority -- update that doc first if either value below
// ever changes), this firmware informally borrows Raspberry Pi's own VID
// (0x2E8A) rather than TinyUSB's 0xCafe placeholder used previously, but
// picks a PID well outside every RPi-documented PID under that VID seen in
// this toolchain's pico-sdk checkout and general RPi USB ID references
// (0x0003 RP2040 BOOTSEL/bootrom, 0x0004 Picoprobe/Debug Probe CDC, 0x0009
// non-RP2040 pico-sdk stdio CDC, 0x000A RP2040 pico-sdk stdio CDC --
// confirmed directly in this checkout's
// lib/pico-sdk/src/rp2_common/pico_stdio_usb/stdio_usb_descriptors.c --
// 0x000C Debug Probe CMSIS-DAP). Those are all low, sequentially-allocated
// values; 0xF00A sits far outside that range (a nod to the old placeholder
// PID, 0x000A, this fixture used to share with generic pico-sdk CDC
// examples) so a newly-registered official RPi PID -- which has so far only
// ever grown that low range -- cannot collide with it. This is informal use
// of RPi's VID for an in-house bench tool that will never ship; accepted
// as such, not a claim of RPi's endorsement.
#define SIMFW_USB_VID 0x2E8Au
#define SIMFW_USB_PID 0xF00Au
#define SIMFW_USB_BCD 0x0200u

tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = SIMFW_USB_BCD,

    // Use Interface Association Descriptor (IAD) for CDC, same convention
    // as every other TinyUSB CDC device -- required so composite-capable
    // hosts group the CDC Control+Data interface pair as one function
    // (irrelevant with only one interface today, but costs nothing to set
    // correctly now and matters the moment a second class is ever added).
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,

    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor = SIMFW_USB_VID,
    .idProduct = SIMFW_USB_PID,
    .bcdDevice = 0x0100u,

    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,

    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// String Descriptor indices
//--------------------------------------------------------------------+
// Declared here, ahead of the Configuration Descriptor below, because
// TUD_CDC_DESCRIPTOR()'s `_stridx` argument needs STRID_CDC0_INTERFACE /
// STRID_CDC1_INTERFACE at that point in the file. The actual string TABLE
// (string_desc_arr[]) stays down in the "String Descriptors" section with
// tud_descriptor_string_cb() -- only the index enum needs to move.
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC0_INTERFACE, // "SimFW Control" -- the benchproto protocol CDC
    STRID_CDC1_INTERFACE, // "SimFW Console" -- the log/console CDC
};

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
// Two CDC functions, each its own IAD (Interface Association Descriptor),
// four USB interfaces total -- this is what a composite device needs for
// Windows to bind both CDC-ACM instances at once. TinyUSB's own
// TUD_CDC_DESCRIPTOR() macro already emits one IAD per invocation (the
// 8-byte "Interface Associate" block ahead of that function's own
// control+data interface pair), so two invocations back to back, one per
// CDC function, is the entire IAD story here -- there is no separate IAD
// table to hand-assemble.
//
// CDC0 = the benchproto PROTOCOL link, interfaces 0 (control) and 1 (data)
//        -- byte-for-byte the same wire behavior the single-CDC build had
//        (usb_owner.c's SLIP/benchproto framing over `tud_cdc_n_*(0, ...)`,
//        which is what plain `tud_cdc_*()` resolves to for CDC instance 0).
// CDC1 = the CONSOLE sink, interfaces 2 (control) and 3 (data) -- mirrors
//        printf() output that already goes to UART0 (usb_owner.c's
//        usb_owner_console_write(), driven by drivers/console_sink.c's
//        stdio_driver_t). Declared SECOND, deliberately: TinyUSB's CDC
//        instance index (the `itf` argument tud_cdc_n_*()/
//        tud_cdc_line_coding_cb()/tud_cdc_line_state_cb() all take) is
//        assigned in config-descriptor declaration order, so "CDC0 is the
//        protocol link" is enforced by this ordering, not by convention --
//        see usb_owner.c's header comment on why the 1200-baud bootloader
//        touch is gated on instance 0 only.
enum {
    ITF_NUM_CDC0_CTRL = 0,
    ITF_NUM_CDC0_DATA,
    ITF_NUM_CDC1_CTRL,
    ITF_NUM_CDC1_DATA,
    ITF_NUM_TOTAL,
};

// CDC0 (protocol) endpoints -- unchanged from the single-CDC build, so a
// host-side capture/driver binding made against the old layout still lines
// up for this function.
#define EPNUM_CDC0_NOTIF 0x81u
#define EPNUM_CDC0_OUT   0x02u
#define EPNUM_CDC0_IN    0x82u

// CDC1 (console) endpoints -- next free addresses. Full-speed bulk/interrupt
// endpoints only (no isochronous), well inside the RP2040 USB controller's
// endpoint budget alongside CDC0's three above.
#define EPNUM_CDC1_NOTIF 0x83u
#define EPNUM_CDC1_OUT   0x04u
#define EPNUM_CDC1_IN    0x84u

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_CDC_DESC_LEN)

static uint8_t const desc_fs_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    // Interface number, string index, EP notification address and size, EP data address (out, in) and size.
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC0_CTRL, STRID_CDC0_INTERFACE, EPNUM_CDC0_NOTIF, 8, EPNUM_CDC0_OUT, EPNUM_CDC0_IN, 64),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC1_CTRL, STRID_CDC1_INTERFACE, EPNUM_CDC1_NOTIF, 8, EPNUM_CDC1_OUT, EPNUM_CDC1_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index; // single configuration only
    return desc_fs_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+
// STRID_* indices themselves are declared earlier (see that section's own
// comment for why). Distinct, descriptive strings for the two CDC
// interfaces matter for more than cosmetics: kilnsim's PC-side port
// discovery (tools/PcTools/src/kilnsim/link.py's `list_protocol_ports()`)
// reads these back (via pyserial's `interface`/`description`/`product`
// fields, which mirror a USB interface's iInterface string on most OSes) to
// tell the two same-VID/PID COM ports apart -- and a human staring at
// Device Manager/`ls /dev/serial/by-id` gets the same disambiguation for
// free.

static char const *const string_desc_arr[] = {
    NULL,                      // 0: LANGID, handled specially below
    "kilnCtl",                 // 1: Manufacturer
    "SimFW Bench Fixture",     // 2: Product
    NULL,                      // 3: Serial -- filled from the RP2040's unique board id, see below
    "SimFW Control",           // 4 (STRID_CDC0_INTERFACE): benchproto protocol CDC
    "SimFW Console",           // 5 (STRID_CDC1_INTERFACE): log/console CDC
};

// RP2040's flash unique id is 8 bytes -> 16 hex chars, well under
// PICO_UNIQUE_BOARD_ID_SIZE_BYTES*2+1; this doubles as a stable per-board
// serial so `kilnsim`'s PC-side auto-detect (DESIGN_NOTES.md sec 6.1's
// `sim_connect`, "auto-detect by USB VID/PID + protocol PING") can tell two
// SimFW fixtures apart on the same host without relying on enumeration order.
static char s_serial_str[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

static uint16_t s_desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chr_count;

    if (index == STRID_LANGID) {
        s_desc_str[1] = 0x0409; // English (0x0409)
        chr_count = 1;
    } else if (index == STRID_SERIAL) {
        if (s_serial_str[0] == '\0') {
            pico_get_unique_board_id_string(s_serial_str, sizeof(s_serial_str));
        }
        chr_count = strlen(s_serial_str);
        for (size_t i = 0; i < chr_count; i++) {
            s_desc_str[1 + i] = (uint16_t)(unsigned char)s_serial_str[i];
        }
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0]) || !string_desc_arr[index]) {
            return NULL;
        }
        char const *str = string_desc_arr[index];
        chr_count = strlen(str);
        size_t const max_count = sizeof(s_desc_str) / sizeof(s_desc_str[0]) - 1;
        if (chr_count > max_count) {
            chr_count = max_count;
        }
        for (size_t i = 0; i < chr_count; i++) {
            s_desc_str[1 + i] = (uint16_t)(unsigned char)str[i];
        }
    }

    s_desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return s_desc_str;
}

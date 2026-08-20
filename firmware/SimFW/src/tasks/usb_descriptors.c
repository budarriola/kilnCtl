// usb_descriptors.c -- TinyUSB device/config/string descriptor callbacks for
// SimFW's single CDC interface. Part of usb_owner's exclusive USB domain
// (see usb_owner.h's header comment): this file and usb_owner.c are the only
// two translation units that include tusb.h. Split out from usb_owner.c
// itself only because TinyUSB's descriptor callbacks are conventionally
// their own file (every example under lib/tinyusb/examples/device/ does the
// same) -- there is no functional reason cmd_task.c or any other task file
// would ever need to touch this.
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

// A bench-fixture-specific VID/PID pair has not been allocated (this is a
// one-off test fixture, never mass produced) -- 0xCafe is TinyUSB's own
// well-known placeholder VID (see the vendored example this file mirrors),
// reused here for the same reason that example uses it: it is a real,
// harmless value that every OS's generic CDC-ACM driver binds to happily,
// and this device is never meant to ship or be sold. PID's low bit set
// (0x4001) marks "CDC present, nothing else" using the same auto-PID bitmap
// convention the mirrored example documents, so a future SimFW revision that
// adds another USB class does not collide with this one's driver binding on
// Windows.
#define SIMFW_USB_VID 0xCafeu
#define SIMFW_USB_PID 0x4001u
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
// Configuration Descriptor
//--------------------------------------------------------------------+

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL,
};

#define EPNUM_CDC_NOTIF 0x81u
#define EPNUM_CDC_OUT   0x02u
#define EPNUM_CDC_IN    0x82u

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static uint8_t const desc_fs_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    // Interface number, string index, EP notification address and size, EP data address (out, in) and size.
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index; // single configuration only
    return desc_fs_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC_INTERFACE,
};

static char const *const string_desc_arr[] = {
    NULL,                      // 0: LANGID, handled specially below
    "kilnCtl",                 // 1: Manufacturer
    "SimFW Bench Fixture",     // 2: Product
    NULL,                      // 3: Serial -- filled from the RP2040's unique board id, see below
    "SimFW Control",           // 4: CDC Interface
};

// RP2040's flash unique id is 8 bytes -> 16 hex chars, well under
// PICO_UNIQUE_BOARD_ID_SIZE_BYTES*2+1; this doubles as a stable per-board
// serial so `kilnsim`'s PC-side auto-detect (PLAN.md sec 6.1's
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

#include "startup_faults.h"

#include <stdint.h>
#include <string.h>

/* One byte per id: distinct addresses, so concurrent notes never clobber. */
static volatile uint8_t s_set[STARTUP_FAULT_COUNT];

static const char *const k_name[STARTUP_FAULT_COUNT] = {
    [STARTUP_FAULT_EXEC_WATCHDOG] = "profile_exec_wdt (guard 9)",
    [STARTUP_FAULT_PC_LINK_WATCHDOG] = "PC link watchdog",
    [STARTUP_FAULT_PROFILE_EXECUTOR] = "profile executor",
    [STARTUP_FAULT_AUTOTUNE_ENGINE] = "autotune engine",
    [STARTUP_FAULT_KILN_IO_OWNER] = "relay/IO owner",
    [STARTUP_FAULT_THERMO_OWNER] = "thermocouple owner",
    [STARTUP_FAULT_LOG_STORE] = "log store",
    [STARTUP_FAULT_TELEMETRY_LOG] = "telemetry log",
    [STARTUP_FAULT_RELAY_CYCLES] = "relay cycle counter",
    [STARTUP_FAULT_OTA_CONFIRM] = "OTA rollback confirmation",
    [STARTUP_FAULT_OTA_ROUTES] = "OTA routes",
    [STARTUP_FAULT_KILN_CFG_SWAP] = "kiln config apply worker",
    [STARTUP_FAULT_LCD_UI] = "LCD UI",
    [STARTUP_FAULT_BOOT_GUARD_NVS] = "boot_guard counter",
    [STARTUP_FAULT_DANGER_MODE] = "danger mode",
    [STARTUP_FAULT_PC_BRIDGES] = "PC bridge tasks",
    [STARTUP_FAULT_DNS_HIJACK] = "captive-portal DNS",
    [STARTUP_FAULT_HTTP_ROUTES] = "some web pages/APIs",
    [STARTUP_FAULT_WEB_AUTH_ROUTES] = "login/security routes",
    [STARTUP_FAULT_SETTINGS_STORE] = "settings store",
    [STARTUP_FAULT_PICO_AUTO_UPDATE] = "Pico auto-update",
    [STARTUP_FAULT_HEARTBEAT_MONITOR] = "heartbeat monitor",
    [STARTUP_FAULT_TIME_SYNC] = "network time",
    [STARTUP_FAULT_LCD_BACKLIGHT] = "LCD auto-blank/backlight",
    [STARTUP_FAULT_TOUCH] = "LCD touch input",
};

static const char *const k_impact[STARTUP_FAULT_COUNT] = {
    [STARTUP_FAULT_EXEC_WATCHDOG] =
        "guard 9 (executor stall watchdog) is NOT running this boot; do not fire, reboot the board",
    [STARTUP_FAULT_PC_LINK_WATCHDOG] = "relays will not drop on PC link loss; do not fire, reboot the board",
    [STARTUP_FAULT_PROFILE_EXECUTOR] = "no profile can run this boot; reboot, and reflash if it repeats",
    [STARTUP_FAULT_AUTOTUNE_ENGINE] = "autotune is unavailable this boot; reboot",
    [STARTUP_FAULT_KILN_IO_OWNER] = "relay commands via the IO owner are unreachable; reboot",
    [STARTUP_FAULT_THERMO_OWNER] = "thermocouple commands are not routed through the owner; reboot",
    [STARTUP_FAULT_LOG_STORE] = "firing and autotune logs are not saved to flash this boot",
    [STARTUP_FAULT_TELEMETRY_LOG] = "no firing telemetry log this boot",
    [STARTUP_FAULT_RELAY_CYCLES] = "relay contact-cycle history is not kept this boot",
    [STARTUP_FAULT_OTA_CONFIRM] = "this image stays unconfirmed (pending verify) and may roll back on a reboot; reflash it",
    [STARTUP_FAULT_OTA_ROUTES] = "OTA, reset and recovery routes are absent this boot; reboot",
    [STARTUP_FAULT_KILN_CFG_SWAP] = "applying a saved kiln config is unavailable this boot; reboot",
    [STARTUP_FAULT_LCD_UI] = "the LCD stays blank this boot; the web UI still works",
    [STARTUP_FAULT_BOOT_GUARD_NVS] =
        "the boot-loop counter is not saved, so a reset loop would not reach recovery mode; reboot",
    [STARTUP_FAULT_DANGER_MODE] =
        "danger mode is unavailable this boot; reboot",
    [STARTUP_FAULT_PC_BRIDGES] =
        "some PC-link GUI/tool functions are dead this boot (see the log); reboot, and reflash if it repeats",
    [STARTUP_FAULT_DNS_HIJACK] =
        "phones joining the setup AP get no sign-in popup; browse to 192.168.4.1; reboot to retry",
    [STARTUP_FAULT_HTTP_ROUTES] =
        "some web pages or API routes are missing this boot (see the log); reboot, and reflash if it repeats",
    [STARTUP_FAULT_WEB_AUTH_ROUTES] =
        "login, security or TOTP routes are missing this boot, so sign-in may not work; reboot",
    [STARTUP_FAULT_SETTINGS_STORE] =
        "a saved-settings store did not load, so defaults apply this boot; check settings, reboot",
    [STARTUP_FAULT_PICO_AUTO_UPDATE] =
        "the safety processor firmware was not checked against the staged image this boot; reboot",
    [STARTUP_FAULT_HEARTBEAT_MONITOR] =
        "the IO-owner heartbeat monitor is not running this boot; reboot",
    [STARTUP_FAULT_TIME_SYNC] =
        "no network time this boot, so log timestamps are wrong; check Wi-Fi, reboot",
    [STARTUP_FAULT_LCD_BACKLIGHT] =
        "LCD auto-blank or backlight control is off this boot; reboot",
    [STARTUP_FAULT_TOUCH] =
        "the LCD touch panel does not respond this boot; use the web UI, check the touch cable, reboot",
};

void startup_fault_note(startup_fault_t id)
{
    if ((unsigned)id < (unsigned)STARTUP_FAULT_COUNT) {
        s_set[id] = 1;
    }
}

bool startup_fault_is_set(startup_fault_t id)
{
    return (unsigned)id < (unsigned)STARTUP_FAULT_COUNT && s_set[id] != 0;
}

unsigned startup_fault_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < (unsigned)STARTUP_FAULT_COUNT; i++) {
        n += s_set[i] != 0;
    }
    return n;
}

const char *startup_fault_name(startup_fault_t id)
{
    return (unsigned)id < (unsigned)STARTUP_FAULT_COUNT ? k_name[id] : NULL;
}

const char *startup_fault_impact(startup_fault_t id)
{
    return (unsigned)id < (unsigned)STARTUP_FAULT_COUNT ? k_impact[id] : NULL;
}

unsigned startup_fault_summarize(char *out, size_t cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    size_t o = 0;
    unsigned named = 0;
    for (unsigned i = 0; i < (unsigned)STARTUP_FAULT_COUNT; i++) {
        if (!s_set[i]) {
            continue;
        }
        const char *nm = k_name[i];
        size_t len = strlen(nm);
        size_t need = len + (o ? 2 : 0);
        if (o + need + 4 > cap) { /* keep room for "..." and NUL */
            if (o + 4 <= cap) {
                memcpy(out + o, "...", 4);
            }
            return named;
        }
        if (o) {
            out[o++] = ';';
            out[o++] = ' ';
        }
        memcpy(out + o, nm, len + 1);
        o += len;
        named++;
    }
    return named;
}

void startup_fault_reset_for_test(void)
{
    for (unsigned i = 0; i < (unsigned)STARTUP_FAULT_COUNT; i++) {
        s_set[i] = 0;
    }
}

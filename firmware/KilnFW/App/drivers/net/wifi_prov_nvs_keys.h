// Wi-Fi persisted-config NVS namespace and key names, in ONE place.
// Shared by wifi_prov_nvs.c (reader/writer) and persist/legacy_default_nvs.c
// (factory-reset erase of the legacy pre-split copy in the default partition),
// so a key added here is automatically erased by the legacy wifi erase and the
// two cannot drift. Add every new wifi_cfg key to WIFI_PROV_NVS_ALL_KEYS_INIT.
#ifndef WIFI_PROV_NVS_KEYS_H
#define WIFI_PROV_NVS_KEYS_H

#include "nvs_key_check.h"

#define NVS_NAMESPACE "wifi_cfg"
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);

#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"
#define NVS_KEY_HAS_CREDS "has_creds"
NVS_KEY_LEN_CHECK(NVS_KEY_SSID);
NVS_KEY_LEN_CHECK(NVS_KEY_PASS);
NVS_KEY_LEN_CHECK(NVS_KEY_HAS_CREDS);
/* Legacy single-network keys (NVS_KEY_SSID/NVS_KEY_PASS/NVS_KEY_HAS_CREDS)
 * are never written by this build any more -- see NVS_KEY_SAVED_NETS below --
 * but are still READ once, by the one-time list-format migration in
 * nvs_load_saved_nets(), for boards provisioned by firmware that predates
 * TODO.md 8.4's bounded-list rework. Left in place afterward, same rationale
 * as every other "old copy stays, never re-read" migration in this file. */
#define NVS_KEY_SAVED_NETS "saved_nets"
#define NVS_KEY_MODE "mode"           /* u8: 0 = WIFI_PROV_MODE_HOME, 1 = WIFI_PROV_MODE_AP */
#define NVS_KEY_LOCAL_ONLY "local_only" /* legacy, read-only: pre-2026-08-11 firmware's
                                          * only mode flag. Migrated into NVS_KEY_MODE the
                                          * first time this runs against an old NVS blob;
                                          * never written by this build. See nvs_load(). */
#define NVS_KEY_AP_SSID "ap_ssid"
#define NVS_KEY_HAS_AP_SSID "has_ap_ssid"
#define NVS_KEY_AP_PASS "ap_pass"
#define NVS_KEY_HAS_AP_PASS "has_ap_pass"
NVS_KEY_LEN_CHECK(NVS_KEY_SAVED_NETS);
NVS_KEY_LEN_CHECK(NVS_KEY_MODE);
NVS_KEY_LEN_CHECK(NVS_KEY_LOCAL_ONLY);
NVS_KEY_LEN_CHECK(NVS_KEY_AP_SSID);
NVS_KEY_LEN_CHECK(NVS_KEY_HAS_AP_SSID);
NVS_KEY_LEN_CHECK(NVS_KEY_AP_PASS);
NVS_KEY_LEN_CHECK(NVS_KEY_HAS_AP_PASS);

/* 2026-08-20, web-GUI-only static-IP addition (see wifi_prov.h's "Static IP"
 * section). New keys, same partition/namespace as everything else in this
 * file -- never repurposing an existing key. Absent (first boot, or a board
 * that predates this feature) reads back as DHCP with empty strings, which
 * is exactly today's always-on default behavior. */
#define NVS_KEY_IP_MODE "ip_mode" /* u8: 0 = DHCP, 1 = STATIC */
#define NVS_KEY_STATIC_IP "static_ip"
#define NVS_KEY_STATIC_NETMASK "static_netmask"
#define NVS_KEY_STATIC_GW "static_gw"
/* 2026-10-03: optional static-mode DNS servers (ROADMAP M18). Individual
 * string keys like the three above, NOT a versioned blob, so no version bump
 * or old-size load path is needed: a board that predates them reads NOT_FOUND
 * and gets empty strings (= "DNS follows the gateway"). */
#define NVS_KEY_STATIC_DNS "static_dns"
#define NVS_KEY_STATIC_DNS2 "static_dns2"
NVS_KEY_LEN_CHECK(NVS_KEY_IP_MODE);
NVS_KEY_LEN_CHECK(NVS_KEY_STATIC_IP);
NVS_KEY_LEN_CHECK(NVS_KEY_STATIC_NETMASK);
NVS_KEY_LEN_CHECK(NVS_KEY_STATIC_GW);
NVS_KEY_LEN_CHECK(NVS_KEY_STATIC_DNS);
NVS_KEY_LEN_CHECK(NVS_KEY_STATIC_DNS2);

#define WIFI_PROV_NVS_ALL_KEYS_INIT                                                                       \
    { NVS_KEY_SSID, NVS_KEY_PASS, NVS_KEY_HAS_CREDS, NVS_KEY_SAVED_NETS, NVS_KEY_MODE, NVS_KEY_LOCAL_ONLY, \
      NVS_KEY_AP_SSID, NVS_KEY_HAS_AP_SSID, NVS_KEY_AP_PASS, NVS_KEY_HAS_AP_PASS, NVS_KEY_IP_MODE,         \
      NVS_KEY_STATIC_IP, NVS_KEY_STATIC_NETMASK, NVS_KEY_STATIC_GW, NVS_KEY_STATIC_DNS, NVS_KEY_STATIC_DNS2 }

#endif // WIFI_PROV_NVS_KEYS_H

// WIFI (task 11) bridge task -- split out of uart_bridge_ext.c 2026-09-04
// (ROADMAP.md M15, 1500-line rule). See uart_bridge_ext_internal.h for the
// full file map and the shared infrastructure (little-endian/reply helpers,
// retry-task-create) this file uses via uart_bridge_ext_* names. Unlike
// CONTROL/PROFILES/AUTOTUNE, this task never touches the flash-safe worker
// -- wifi_prov_* post to the wifi_prov owner task, which has an ordinary
// internal stack, and the write happens there (see uart_bridge_ext.c's
// HAZARD comment). MOVE-ONLY: no logic, ordering, or visibility change
// beyond the widening rename the split required.
#include "uart_bridge.h"
#include "uart_bridge_ext_internal.h"

#include <string.h>

#include "esp_log.h"
#include "hal_time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "uart_task_ids.h"
#include "wifi_prov.h"

/* ==========================================================================
 * WIFI (task 11)
 * ======================================================================== */

typedef struct {
    uart_protocol_t *proto;
    QueueHandle_t inbox;
} wifi_ctx_t;

static size_t wifi_build_status(uint8_t *out)
{
    size_t o = 0;
    out[o++] = WIFI_CMD_GET_STATUS;
    out[o++] = (uint8_t)wifi_prov_get_mode();
    out[o++] = (uint8_t)wifi_prov_get_state();
    bool connected = wifi_prov_is_sta_connected();
    out[o++] = connected ? 1 : 0;
    o = uart_bridge_ext_put_lstring(out, BRIDGE_REPLY_MAX, o, wifi_prov_get_saved_ssid());
    o = uart_bridge_ext_put_lstring(out, BRIDGE_REPLY_MAX, o, wifi_prov_get_ap_ssid());
    /* Owner decision 2026-09-21: GET_STATUS used to emit the board's AP
     * Wi-Fi password verbatim, in the clear, over the USB serial link --
     * this bridge has no auth concept at all (see the file banner), so
     * anyone with physical access to the port could read it. Nothing
     * downstream ever needed the real value: mcp_server_wifi.py's status
     * render already collapsed it to a "[set]"/"[unset]" marker
     * (ap_password_state) and devices_common.py's redact_secret_fields()
     * does the same for get_board_state()'s dump -- both PC-side, both
     * applied only AFTER the secret had already crossed the wire. Fixed at
     * the source instead: the wire field stays the same length-prefixed
     * ASCII string (uart_bridge_ext_put_lstring) it always was, so the
     * protocol version/layout is unchanged and wifi_uart.py's decode
     * (devices_wifi_uart.py's parse_wifi_uart_response) and every existing
     * consumer keep working unmodified -- only the content changes, from
     * the real password to a "[set]"/"" presence marker. */
    o = uart_bridge_ext_put_lstring(out, BRIDGE_REPLY_MAX, o,
                                     (wifi_prov_get_ap_password()[0] != '\0') ? "[set]" : "");
    char sta_ip[16] = {0};
    if (connected) {
        wifi_prov_get_sta_ip(sta_ip, sizeof(sta_ip));
    }
    o = uart_bridge_ext_put_lstring(out, BRIDGE_REPLY_MAX, o, sta_ip);
    out[o++] = (uint8_t)wifi_prov_get_sta_rssi();
    out[o++] = wifi_prov_get_ap_client_count();
    return o;
}

static size_t wifi_build_scan(uint8_t *out)
{
    wifi_prov_scan_result_t results[WIFI_WIRE_MAX_SCAN_ENTRIES + 4];
    size_t count = 0;
    esp_err_t err = wifi_prov_scan(results, sizeof(results) / sizeof(results[0]), &count);
    size_t o = 0;
    out[o++] = WIFI_CMD_SCAN;
    size_t count_pos = o++;
    size_t trunc_pos = o++;
    uint8_t emitted = 0;
    bool truncated = false;
    if (err == ESP_OK) {
        for (size_t i = 0; i < count; i++) {
            if (emitted >= WIFI_WIRE_MAX_SCAN_ENTRIES) {
                truncated = true;
                break;
            }
            size_t before = o;
            o = uart_bridge_ext_put_lstring(out, BRIDGE_REPLY_MAX, o, results[i].ssid);
            if (o + 2 > BRIDGE_REPLY_MAX) {
                o = before;
                truncated = true;
                break;
            }
            out[o++] = (uint8_t)results[i].rssi;
            out[o++] = results[i].secure ? 1 : 0;
            emitted++;
        }
        if (count > emitted) {
            truncated = true;
        }
    }
    out[count_pos] = emitted;
    out[trunc_pos] = truncated ? 1 : 0;
    return o;
}

/* Scan cache for wifi_build_networks() only.
 *
 * WIFI_CMD_GET_NETWORKS lists the SAVED networks; the scan exists solely to
 * annotate each with in-range/RSSI. It was doing a full blocking radio scan on
 * EVERY call, which measured 2.7s per request against 0.2s for every other
 * bridge command on this link -- the scan was the entire difference. Repeating
 * a 2.7s radio scan to refresh a decoration on a list that changes rarely is
 * not a good trade, especially for a UI that polls.
 *
 * Cached for WIFI_NETWORKS_SCAN_CACHE_US. The cost is that in-range/RSSI can
 * be up to that stale; the saved-network list itself is always live, since
 * only the annotation comes from here. An explicit WIFI_CMD_SCAN still forces
 * a fresh scan -- when the user asks to scan, they get a real scan.
 *
 * `static` also deliberately moves this 20-entry array (~720 bytes) OFF the
 * stack. This function runs on wifi_uart_bridge, whose stack overflowed and
 * rebooted the board (see uart_bridge_start_wifi_task); this array was one of
 * the larger contributors.
 *
 * No locking: touched only from wifi_task(), which is single-threaded. Do not
 * call wifi_build_networks() from anywhere else without revisiting that. */
#define WIFI_NETWORKS_SCAN_CACHE_US (30 * 1000 * 1000)

static wifi_prov_scan_result_t s_networks_scan[20];
static size_t                  s_networks_scan_count;
static int64_t                 s_networks_scan_us; /* 0 = never scanned */

static size_t wifi_build_networks(uint8_t *out)
{
    wifi_prov_saved_network_t saved[8];
    size_t saved_count = 0;
    wifi_prov_get_saved_networks(saved, sizeof(saved) / sizeof(saved[0]), &saved_count);

    int64_t now_us = (int64_t)hal_time_now_us();
    if (s_networks_scan_us == 0 || (now_us - s_networks_scan_us) >= WIFI_NETWORKS_SCAN_CACHE_US) {
        size_t    fresh_count = 0;
        esp_err_t scan_err = wifi_prov_scan(s_networks_scan,
                                            sizeof(s_networks_scan) / sizeof(s_networks_scan[0]),
                                            &fresh_count);
        /* On failure keep whatever the previous scan found rather than
         * dropping every annotation: a stale RSSI is more useful than none,
         * and a failed scan is usually transient (radio busy). The timestamp
         * is still advanced so a persistently failing scan cannot turn this
         * back into a scan-on-every-call path. */
        if (scan_err == ESP_OK) {
            s_networks_scan_count = fresh_count;
        }
        s_networks_scan_us = now_us;
    }

    const wifi_prov_scan_result_t *scanned = s_networks_scan;
    size_t                         scan_count = s_networks_scan_count;

    const char *active_ssid = wifi_prov_get_saved_ssid();
    bool sta_connected = wifi_prov_is_sta_connected();

    size_t o = 0;
    out[o++] = WIFI_CMD_GET_NETWORKS;
    size_t count_pos = o++;
    size_t trunc_pos = o++;
    uint8_t emitted = 0;
    bool truncated = false;

    for (size_t i = 0; i < saved_count && !truncated; i++) {
        const char *ssid = saved[i].ssid;
        bool in_range = false;
        int8_t rssi = 0;
        bool secure = false;
        for (size_t j = 0; j < scan_count; j++) {
            if (strcmp(ssid, scanned[j].ssid) == 0) {
                in_range = true;
                rssi = scanned[j].rssi;
                secure = scanned[j].secure;
                break;
            }
        }
        bool connected = sta_connected && strcmp(ssid, active_ssid) == 0;
        if (emitted >= WIFI_WIRE_MAX_NETWORK_ENTRIES) {
            truncated = true;
            break;
        }
        size_t before = o;
        o = uart_bridge_ext_put_lstring(out, BRIDGE_REPLY_MAX, o, ssid);
        if (o + 5 > BRIDGE_REPLY_MAX) {
            o = before;
            truncated = true;
            break;
        }
        out[o++] = 1; /* saved */
        out[o++] = in_range ? 1 : 0;
        out[o++] = (uint8_t)rssi;
        out[o++] = secure ? 1 : 0;
        out[o++] = connected ? 1 : 0;
        emitted++;
    }
    if (saved_count > emitted) {
        truncated = true;
    }

    for (size_t j = 0; j < scan_count && !truncated; j++) {
        bool already_saved = false;
        for (size_t i = 0; i < saved_count; i++) {
            if (strcmp(scanned[j].ssid, saved[i].ssid) == 0) {
                already_saved = true;
                break;
            }
        }
        if (already_saved) {
            continue;
        }
        if (emitted >= WIFI_WIRE_MAX_NETWORK_ENTRIES) {
            truncated = true;
            break;
        }
        bool connected = sta_connected && strcmp(scanned[j].ssid, active_ssid) == 0;
        size_t before = o;
        o = uart_bridge_ext_put_lstring(out, BRIDGE_REPLY_MAX, o, scanned[j].ssid);
        if (o + 5 > BRIDGE_REPLY_MAX) {
            o = before;
            truncated = true;
            break;
        }
        out[o++] = 0; /* not saved */
        out[o++] = 1; /* in_range */
        out[o++] = (uint8_t)scanned[j].rssi;
        out[o++] = scanned[j].secure ? 1 : 0;
        out[o++] = connected ? 1 : 0;
        emitted++;
    }

    out[count_pos] = emitted;
    out[trunc_pos] = truncated ? 1 : 0;
    return o;
}

static void wifi_task(void *arg)
{
    wifi_ctx_t *ctx = (wifi_ctx_t *)arg;
    uart_proto_message_t msg;
    uint8_t reply[BRIDGE_REPLY_MAX];

    while (true) {
        if (uart_protocol_receive(ctx->inbox, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.length < 1) {
            ESP_LOGW(UART_BRIDGE_EXT_TAG, "wifi: empty payload -- rejected");
            continue;
        }
        uint8_t subcmd = msg.payload[0];

        switch (subcmd) {
            case WIFI_CMD_GET_STATUS: {
                size_t len = wifi_build_status(reply);
                uart_bridge_ext_reply(ctx->proto, &msg, UART_TASK_ID_WIFI, reply, len);
                break;
            }
            case WIFI_CMD_SCAN: {
                size_t len = wifi_build_scan(reply);
                uart_bridge_ext_reply(ctx->proto, &msg, UART_TASK_ID_WIFI, reply, len);
                break;
            }
            case WIFI_CMD_ADD_NETWORK: {
                if (!uart_bridge_ext_args_ok("wifi", &msg, 2)) break;
                uint8_t ssid_len = msg.payload[1];
                if (!uart_bridge_ext_args_ok("wifi", &msg, (size_t)2 + ssid_len + 1)) break;
                char ssid[WIFI_PROV_SSID_MAX_LEN + 1];
                if (ssid_len > WIFI_PROV_SSID_MAX_LEN) {
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, false, "ssid too long");
                    break;
                }
                memcpy(ssid, &msg.payload[2], ssid_len);
                ssid[ssid_len] = '\0';
                size_t p = 2 + ssid_len;
                uint8_t pass_len = msg.payload[p++];
                if (!uart_bridge_ext_args_ok("wifi", &msg, p + pass_len)) break;
                char password[WIFI_PROV_PASSWORD_MAX_LEN + 1];
                if (pass_len > WIFI_PROV_PASSWORD_MAX_LEN) {
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, false,
                                                 "password too long");
                    break;
                }
                memcpy(password, &msg.payload[p], pass_len);
                password[pass_len] = '\0';
                esp_err_t err = wifi_prov_add_network(ssid, ssid_len, password, pass_len);
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, err == ESP_OK,
                                             err == ESP_ERR_NO_MEM ? "saved network list is full"
                                                                    : (err == ESP_OK ? NULL
                                                                                     : "could not save credentials"));
                break;
            }
            case WIFI_CMD_SET_MODE: {
                if (!uart_bridge_ext_args_ok("wifi", &msg, 2)) break;
                wifi_prov_mode_t mode = (msg.payload[1] == 1) ? WIFI_PROV_MODE_AP : WIFI_PROV_MODE_HOME;
                esp_err_t err = wifi_prov_set_mode(mode);
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, err == ESP_OK,
                                             err == ESP_OK ? NULL : "could not change mode");
                break;
            }
            case WIFI_CMD_SET_AP_IDENTITY: {
                if (!uart_bridge_ext_args_ok("wifi", &msg, 2)) break;
                size_t p = 1;
                bool has_ssid = msg.payload[p++] != 0;
                char ssid[WIFI_PROV_SSID_MAX_LEN + 1];
                size_t ssid_len = 0;
                if (has_ssid) {
                    if (!uart_bridge_ext_args_ok("wifi", &msg, p + 1)) break;
                    ssid_len = msg.payload[p++];
                    if (ssid_len > WIFI_PROV_SSID_MAX_LEN || !uart_bridge_ext_args_ok("wifi", &msg, p + ssid_len)) {
                        uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, false,
                                                     "ap_ssid too long");
                        break;
                    }
                    memcpy(ssid, &msg.payload[p], ssid_len);
                    ssid[ssid_len] = '\0';
                    p += ssid_len;
                }
                if (!uart_bridge_ext_args_ok("wifi", &msg, p + 1)) break;
                bool has_password = msg.payload[p++] != 0;
                char password[WIFI_PROV_PASSWORD_MAX_LEN + 1];
                size_t pass_len = 0;
                if (has_password) {
                    if (!uart_bridge_ext_args_ok("wifi", &msg, p + 1)) break;
                    pass_len = msg.payload[p++];
                    if (pass_len > WIFI_PROV_PASSWORD_MAX_LEN || !uart_bridge_ext_args_ok("wifi", &msg, p + pass_len)) {
                        uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, false,
                                                     "ap_password too long");
                        break;
                    }
                    memcpy(password, &msg.payload[p], pass_len);
                    password[pass_len] = '\0';
                }
                esp_err_t err = ESP_OK;
                const char *fail_msg = NULL;
                if (has_ssid) {
                    err = wifi_prov_set_ap_ssid(ssid, ssid_len);
                    if (err != ESP_OK) fail_msg = "ap_ssid must be 1-32 characters";
                }
                if (err == ESP_OK && has_password) {
                    err = wifi_prov_set_ap_password(password, pass_len);
                    if (err != ESP_OK) fail_msg = "ap_password must be empty or 8-63 characters";
                }
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, err == ESP_OK, fail_msg);
                break;
            }
            case WIFI_CMD_GET_NETWORKS: {
                size_t len = wifi_build_networks(reply);
                uart_bridge_ext_reply(ctx->proto, &msg, UART_TASK_ID_WIFI, reply, len);
                break;
            }
            case WIFI_CMD_FORGET: {
                if (!uart_bridge_ext_args_ok("wifi", &msg, 2)) break;
                uint8_t ssid_len = msg.payload[1];
                if (!uart_bridge_ext_args_ok("wifi", &msg, (size_t)2 + ssid_len) || ssid_len > WIFI_PROV_SSID_MAX_LEN) {
                    uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, false,
                                                 "ssid missing or too long");
                    break;
                }
                char ssid[WIFI_PROV_SSID_MAX_LEN + 1];
                memcpy(ssid, &msg.payload[2], ssid_len);
                ssid[ssid_len] = '\0';
                esp_err_t err = wifi_prov_forget_network(ssid, ssid_len);
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, err == ESP_OK,
                                             err == ESP_OK ? NULL : "could not forget network");
                break;
            }
            default:
                ESP_LOGW(UART_BRIDGE_EXT_TAG, "wifi: unknown subcmd 0x%02X -- rejected", subcmd);
                uart_bridge_ext_reply_ok_err(ctx->proto, &msg, UART_TASK_ID_WIFI, subcmd, false,
                                             "unknown subcommand");
                break;
        }
    }
}

esp_err_t uart_bridge_start_wifi_task(uart_protocol_t *proto)
{
    if (!proto) {
        return ESP_ERR_INVALID_ARG;
    }
    static wifi_ctx_t ctx;
    ctx.proto = proto;

    esp_err_t err = uart_protocol_register_task(proto, UART_TASK_ID_WIFI, BRIDGE_INBOX_LEN, &ctx.inbox);
    if (err != ESP_OK) {
        return err;
    }
    /* 8192, raised from 3072. THIS TASK'S STACK OVERFLOW REBOOTED THE BOARD.
     *
     * It is the "spontaneous reboot" this project chased for days. Caught
     * 2026-08-20 by the first coredump the firmware ever successfully wrote:
     *
     *   Panic reason: ***ERROR*** A stack overflow in task wifi_uart_bridg
     *                 has been detected.
     *          TCB             NAME PRIO C/B  STACK USED/FREE
     *   0x3fcc0a7c  wifi_uart_bridg      7/5         3440/376
     *
     * 3440 bytes used against a 3072 request. The earlier estimate quoted
     * here -- "real stack depth is modest (BRIDGE_REPLY_MAX reply buffer
     * plus small fixed SSID/password copies)" -- counted only THIS file's
     * own locals and missed the ESP-IDF Wi-Fi driver call chain underneath
     * WIFI_CMD_SCAN / status queries, which dominates. Reasoning about stack
     * depth from the visible frame is how this got sized wrong; the figure
     * above is measured.
     *
     * Sized at 8192, not 4096: 4096 is only ~19% above the observed
     * high-water, and that high-water came from an ordinary bench session,
     * not a worst case (a scan returning the full WIFI_WIRE_MAX_SCAN_ENTRIES
     * set with a deeper driver path can only be larger). The stack comes
     * from PSRAM via uart_bridge_ext_retry_task_create_pinned(), so the
     * extra 4KB costs no internal SRAM whatsoever -- there is no reason to
     * be thrifty here, and being thrifty is exactly what rebooted the kiln
     * controller. */
    BaseType_t created = uart_bridge_ext_retry_task_create_pinned(wifi_task, "wifi_uart_bridge", 8192, &ctx, 5);
    if (created != pdPASS) {
        uart_protocol_unregister_task(proto, UART_TASK_ID_WIFI);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

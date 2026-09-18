// See ota_http_util.h for why these functions live in their own
// dependency-light file. Verbatim move out of ota_http.c -- see that
// header's doc comment.
#include "ota_http_util.h"

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_rollback_result.h"

void ota_http_hex_encode(const uint8_t *in, size_t len, char *out /* 2*len + 1 bytes */)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[(in[i] >> 4) & 0xFu];
        out[2 * i + 1] = digits[in[i] & 0xFu];
    }
    out[2 * len] = '\0';
}

bool ota_http_hex_decode(const char *hex, size_t hex_len, uint8_t *out)
{
    if (hex_len % 2 != 0) {
        return false;
    }
    for (size_t i = 0; i < hex_len / 2; i++) {
        int hi = -1, lo = -1;
        char ch = hex[2 * i];
        if (ch >= '0' && ch <= '9') hi = ch - '0';
        else if (ch >= 'a' && ch <= 'f') hi = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') hi = ch - 'A' + 10;
        ch = hex[2 * i + 1];
        if (ch >= '0' && ch <= '9') lo = ch - '0';
        else if (ch >= 'a' && ch <= 'f') lo = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') lo = ch - 'A' + 10;
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

const char *ota_http_esp_phase_str(ota_http_esp_phase_t phase)
{
    switch (phase) {
        case OTA_HTTP_ESP_PHASE_IDLE:       return "idle";
        case OTA_HTTP_ESP_PHASE_VERIFYING:  return "verifying";
        case OTA_HTTP_ESP_PHASE_WRITING:    return "writing";
        case OTA_HTTP_ESP_PHASE_FINALIZING: return "finalizing";
        case OTA_HTTP_ESP_PHASE_DONE:       return "done";
        case OTA_HTTP_ESP_PHASE_FAILED:     return "failed";
        default:                            return "unknown";
    }
}

const char *ota_http_verify_result_str(ota_http_verify_result_t r)
{
    switch (r) {
        case OTA_HTTP_VERIFY_OK: return "ok";
        case OTA_HTTP_VERIFY_LOCKED_OUT: return "locked out -- too many recent wrong-password attempts";
        case OTA_HTTP_VERIFY_NO_VALID_NONCE:
            return "no valid challenge -- GET /api/ota/challenge first, then POST within 30 s";
        case OTA_HTTP_VERIFY_BAD_MAC: return "wrong password";
        case OTA_HTTP_VERIFY_NO_AP_PASSWORD:
            return "this board's AP password is empty -- set an AP password to update it over HTTP, "
                   "or use the physical BOOT-button recovery window (hold BOOT during boot)";
        default: return "authentication failed";
    }
}

const char *ota_http_pico_rollback_reason_str(uint8_t reason_code)
{
    switch (reason_code) {
        case KILNLINK_ROLLBACK_RESULT_REASON_ARMED:
            return "relay is ARMED -- rollback is refused while ARMED";
        case KILNLINK_ROLLBACK_RESULT_REASON_NO_METADATA:
            return "no bootloader metadata to roll back from";
        case KILNLINK_ROLLBACK_RESULT_REASON_SLOT_INVALID:
            return "the other bootloader slot is not currently valid to fall back to";
        case KILNLINK_ROLLBACK_RESULT_REASON_STORAGE:
            return "the safety processor's flash write failed";
        case KILNLINK_ROLLBACK_RESULT_REASON_UNKNOWN:
        default:
            return "unknown reason";
    }
}

int ota_http_pico_rollback_format_body(safety_link_rollback_outcome_t outcome, uint8_t reason_code,
                                        char *body, size_t cap)
{
    switch (outcome) {
        case SAFETY_LINK_ROLLBACK_OUTCOME_LINK_DOWN:
            return snprintf(body, cap, "{\"ok\":false,\"status\":\"link_down\","
                             "\"detail\":\"the safety link is down; the request was not sent\"}");
        case SAFETY_LINK_ROLLBACK_OUTCOME_SEND_FAILED:
            return snprintf(body, cap, "{\"ok\":false,\"status\":\"send_failed\","
                             "\"detail\":\"could not send the rollback request over the safety link\"}");
        case SAFETY_LINK_ROLLBACK_OUTCOME_REFUSED:
            return snprintf(body, cap,
                             "{\"ok\":false,\"status\":\"refused\",\"reason_code\":%u,\"detail\":\"%s\"}",
                             (unsigned)reason_code, ota_http_pico_rollback_reason_str(reason_code));
        case SAFETY_LINK_ROLLBACK_OUTCOME_UNKNOWN_TIMEOUT:
            // Still ok:true: the request DID reach the point of being sent
            // (this is not a local failure), but this driver genuinely
            // cannot say what happened -- an old Pico that predates
            // ROLLBACK_RESULT cannot report a refusal even if it refused.
            // Honest, not a false success.
            return snprintf(body, cap, "{\"ok\":true,\"status\":\"unknown\","
                             "\"detail\":\"no reply within the wait window; this safety processor build "
                             "predates ROLLBACK_RESULT and cannot confirm accept or refuse -- watch for "
                             "a link reconnect\"}");
        case SAFETY_LINK_ROLLBACK_OUTCOME_ACCEPTED:
        default:
            // opus-review finding 2: this outcome is now only reported once
            // BOTH the peer's boot_id and its build identity have been
            // observed to change (safety_link_rollback_reboot_confirmed()),
            // so "rebooted into a different image" is what was actually
            // seen on the wire -- but this handler still never watched the
            // reboot complete or verified which slot came up, so "into the
            // previous image" specifically (as opposed to "an update landed
            // mid-watch" or some other different-image case) is more than
            // this evidence proves. Worded to claim only what was observed.
            return snprintf(body, cap, "{\"ok\":true,\"status\":\"rebooting\","
                             "\"detail\":\"accepted -- the safety processor rebooted into a different "
                             "firmware image (boot_id and build identity both changed)\"}");
    }
}

bool ota_http_client_ip_finalize(char *out, size_t out_len, const char *formatted_addr)
{
    if (formatted_addr != NULL) {
        snprintf(out, out_len, "%s", formatted_addr);
        return true;
    }
    snprintf(out, out_len, "unknown");
    return false;
}

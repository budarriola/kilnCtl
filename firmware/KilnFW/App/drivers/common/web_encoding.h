// web_encoding -- shared Accept-Encoding content negotiation for the
// pre-gzipped embedded web pages (TODO.md 10.6a).
//
// Every *_page.html (and theme.css) is gzipped at configure time by
// App/drivers/CMakeLists.txt and embedded ONLY in compressed form: the raw
// pages total 162,728 bytes against 74,720 bytes (5%) free in the app
// partition, so a second uncompressed copy does not fit, and a runtime
// inflater was rejected for the same budget reason. That makes gzip the
// only representation this server can produce for those routes.
//
// RFC 9110 s12.5.3: a request with NO Accept-Encoding header means any
// content coding is acceptable, so serving gzip to such a client is
// correct and must not be warned about. The real (narrow) defect this
// module fixes is a client that DOES send Accept-Encoding and excludes
// gzip ("identity", "deflate", "gzip;q=0") -- it used to get a gzip body it
// told us it could not decode. Those requests now get 406 instead.
//
// Deliberately NOT header-only static inline (unlike http_form.h): with the
// parser inlined into all five *_http.c translation units it cost ~2.8 KB
// of flash, which is not affordable at 5% free. Prototypes here, one copy
// of the code in web_encoding.c.
#ifndef WEB_ENCODING_H
#define WEB_ENCODING_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Content negotiation for the gzip-only pages.
 *
 *  - Accept-Encoding absent          -> true  (RFC 9110 s12.5.3)
 *  - "gzip" listed with q != 0       -> true
 *  - "*"    listed with q != 0       -> true (when gzip isn't named)
 *  - "gzip;q=0" / "*;q=0" / neither  -> false
 *
 * Whitespace-tolerant, case-insensitive, fixed-size buffer, no heap. A
 * header longer than the buffer is treated as absent (acceptable) rather
 * than as a refusal. */
bool web_client_accepts_gzip(httpd_req_t *req);

/* Sends an uncompressed text/plain 406 body (no Content-Encoding header)
 * and logs one ESP_LOGW naming the offending header value. Call only after
 * web_client_accepts_gzip() returned false -- which, by construction, means
 * the client sent an Accept-Encoding header that excluded gzip. */
esp_err_t web_send_gzip_not_acceptable(httpd_req_t *req, const char *tag, const char *page_name);

/* Sets the cache policy for an embedded asset (page HTML, theme.css,
 * nav.js, app.js). Call once per asset response, alongside the
 * Content-Encoding header.
 *
 * These assets have no URL versioning and no validators, and this server
 * sent no Cache-Control at all until 2026-08-22. HTTP lets a client apply
 * HEURISTIC freshness to a 200 with no explicit policy (RFC 9111 s4.2.2),
 * so browsers were free to keep serving a copy from before the last
 * firmware update -- and did. That produced a genuinely confusing class of
 * bug report: the owner saw a dashboard row rendering old markup ("86.6
 * °FR1: off", with none of the styling the current page applies) while the
 * flashed binary provably contained the corrected page. Verified by
 * extracting main_page.html.gz straight out of the build tree: the fix was
 * present in the firmware the browser was talking to.
 *
 * "no-cache" (revalidate before reuse), NOT "no-store" (never keep a copy):
 * these assets are 3-20 KB each over a link this board can serve quickly,
 * and revalidation still lets the browser reuse its copy on a 304 rather
 * than re-downloading. What matters is that a stale copy can never be used
 * WITHOUT asking, so a reflash always reaches the browser. */
void web_set_asset_cache_headers(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif /* WEB_ENCODING_H */

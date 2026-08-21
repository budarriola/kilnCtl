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

#ifdef __cplusplus
}
#endif

#endif /* WEB_ENCODING_H */

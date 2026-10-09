// http_body_recv.h -- read a whole request body, looping httpd_req_recv().
//
// httpd_req_recv() may return fewer bytes than asked before EOF (a body split
// over several TCP segments), so a single call can silently truncate a body.
// HTTP audit E2 #1: POST /api/auth/bootstrap_password stored a truncated admin
// password that way. This is the shared loop for handlers that parse the body
// in place; it returns false (caller answers 400) on any recv <= 0 before
// content_len bytes arrived, and always NUL-terminates at content_len.
#ifndef HTTP_BODY_RECV_H
#define HTTP_BODY_RECV_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_http_server.h"

// buf must hold content_len + 1 bytes. Returns true only when exactly
// content_len bytes were read. On failure the partial bytes are wiped (the
// body may be a plaintext password) and buf[0] is NUL.
static inline bool http_body_recv_full(httpd_req_t *req, char *buf, size_t content_len)
{
    size_t received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, buf + received, content_len - received);
        if (ret <= 0) {
            memset(buf, 0, content_len + 1);
            return false;
        }
        received += (size_t)ret;
    }
    buf[received] = '\0';
    return true;
}

#endif

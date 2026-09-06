/* test_fake_uart.c -- standalone MSVC host test for hwAbstraction/host/fake_uart.c.
 * Compiled and run by test_host_fakes.ps1.
 */
#include <stdio.h>
#include <string.h>

#include "fake_uart.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond) \
    do { \
        if (cond) { g_pass++; } \
        else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

int main(void)
{
    fake_uart_reset_all();

    /* --- init: NULL args -> HAL_INVALID_ARG --- */
    hal_uart_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0; cfg.tx_io = 1; cfg.rx_io = 2; cfg.baud = 230400;
    hal_uart_t u;
    memset(&u, 0, sizeof(u));
    CHECK(hal_uart_init(NULL, &cfg) == HAL_INVALID_ARG);
    CHECK(hal_uart_init(&u, NULL) == HAL_INVALID_ARG);

    /* --- uninitialized handle: every call -> HAL_NOT_READY / 0 --- */
    hal_uart_t garbage;
    memset(&garbage, 0xAB, sizeof(garbage)); /* never went through init */
    CHECK(fake_uart_is_live(&garbage) == false);
    CHECK(hal_uart_send(&garbage, (const uint8_t *)"x", 1) == HAL_NOT_READY);
    CHECK(hal_uart_send_blocking(&garbage, (const uint8_t *)"x", 1, 100) == HAL_NOT_READY);
    CHECK(hal_uart_deinit(&garbage) == HAL_NOT_READY);
    CHECK(hal_uart_restart(&garbage) == HAL_NOT_READY);
    CHECK(fake_uart_script_rx(&garbage, (const uint8_t *)"x", 1) == HAL_NOT_READY);
    uint8_t rxbuf[8];
    CHECK(hal_uart_recv(&garbage, rxbuf, sizeof(rxbuf)) == 0);
    CHECK(hal_uart_recv_blocking(&garbage, rxbuf, sizeof(rxbuf), 10) == 0);
    CHECK(hal_uart_get_rx_error_count(&garbage) == 0);
    CHECK(hal_uart_get_tx_dropped(&garbage) == 0);
    CHECK(hal_uart_get_task_handle(&garbage) == NULL);

    /* --- init succeeds; handle becomes live --- */
    CHECK(hal_uart_init(&u, &cfg) == HAL_OK);
    CHECK(fake_uart_is_live(&u) == true);

    /* --- send / send_blocking append to the same tx capture, in order --- */
    CHECK(hal_uart_send(&u, (const uint8_t *)"AB", 2) == HAL_OK);
    CHECK(hal_uart_send_blocking(&u, (const uint8_t *)"CD", 2, 50) == HAL_OK);
    size_t tx_len = 0;
    const uint8_t *tx = fake_uart_tx_capture(&u, &tx_len);
    CHECK(tx != NULL);
    CHECK(tx_len == 4);
    CHECK(tx != NULL && tx_len == 4 && memcmp(tx, "ABCD", 4) == 0);

    /* --- recv: round trip via scripted rx --- */
    CHECK(fake_uart_script_rx(&u, (const uint8_t *)"hello", 5) == HAL_OK);
    CHECK(fake_uart_rx_pending_count(&u) == 5);
    memset(rxbuf, 0, sizeof(rxbuf));
    size_t got = hal_uart_recv(&u, rxbuf, 3); /* non-blocking, partial read */
    CHECK(got == 3);
    CHECK(memcmp(rxbuf, "hel", 3) == 0);
    CHECK(fake_uart_rx_pending_count(&u) == 2);
    got = hal_uart_recv(&u, rxbuf, sizeof(rxbuf));
    CHECK(got == 2);
    CHECK(memcmp(rxbuf, "lo", 2) == 0);
    CHECK(fake_uart_rx_pending_count(&u) == 0);
    CHECK(hal_uart_recv(&u, rxbuf, sizeof(rxbuf)) == 0); /* nothing more buffered */

    /* --- recv_blocking: host fake has no time model, so it's just recv()
     * under the hood -- scripted bytes come back immediately, nothing
     * scripted returns 0 (the HAL_TIMEOUT-equivalent case) --- */
    CHECK(fake_uart_script_rx(&u, (const uint8_t *)"Q", 1) == HAL_OK);
    memset(rxbuf, 0, sizeof(rxbuf));
    CHECK(hal_uart_recv_blocking(&u, rxbuf, sizeof(rxbuf), 100) == 1);
    CHECK(rxbuf[0] == 'Q');
    CHECK(hal_uart_recv_blocking(&u, rxbuf, sizeof(rxbuf), 100) == 0);
    CHECK(hal_uart_recv_blocking(&u, NULL, sizeof(rxbuf), 100) == 0);
    CHECK(hal_uart_recv_blocking(&u, rxbuf, 0, 100) == 0);

    /* --- get_task_handle: host fake has no owner task --- */
    CHECK(hal_uart_get_task_handle(&u) == NULL);

    /* --- fake_uart_script_rx error paths --- */
    CHECK(fake_uart_script_rx(&u, NULL, 3) == HAL_INVALID_ARG);
    CHECK(fake_uart_script_rx(&u, NULL, 0) == HAL_OK); /* zero-length is a no-op */

    /* --- restart is RX-only: clears rx state, must NOT touch tx capture --- */
    CHECK(fake_uart_script_rx(&u, (const uint8_t *)"xyz", 3) == HAL_OK);
    CHECK(fake_uart_rx_pending_count(&u) == 3);
    size_t tx_len_before_restart = 0;
    const uint8_t *tx_before = fake_uart_tx_capture(&u, &tx_len_before_restart);
    (void)tx_before;
    CHECK(hal_uart_restart(&u) == HAL_OK);
    CHECK(fake_uart_rx_pending_count(&u) == 0); /* rx cleared */
    size_t tx_len_after_restart = 0;
    const uint8_t *tx_after = fake_uart_tx_capture(&u, &tx_len_after_restart);
    CHECK(tx_len_after_restart == tx_len_before_restart); /* tx untouched */
    CHECK(tx_after != NULL && tx_len_after_restart == 4 && memcmp(tx_after, "ABCD", 4) == 0);

    /* --- rx ring overflow: drop-oldest, counted --- */
    {
        hal_uart_t u2;
        memset(&u2, 0, sizeof(u2));
        CHECK(hal_uart_init(&u2, &cfg) == HAL_OK);
        uint8_t big[FAKE_UART_RX_RING_CAP + 100];
        for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i & 0xFF);
        CHECK(fake_uart_script_rx(&u2, big, sizeof(big)) == HAL_OK);
        CHECK(fake_uart_rx_pending_count(&u2) == FAKE_UART_RX_RING_CAP);
        CHECK(fake_uart_rx_dropped_count(&u2) == 100);
        /* the oldest 100 bytes were dropped, so the ring now starts at
         * big[100] */
        uint8_t first;
        CHECK(hal_uart_recv(&u2, &first, 1) == 1);
        CHECK(first == (uint8_t)(100 & 0xFF));
    }

    /* --- tx overflow via hal_uart_send(): whole-buffer-or-HAL_BUSY,
     * documented drop-counter behavior --- */
    {
        hal_uart_t u3;
        memset(&u3, 0, sizeof(u3));
        CHECK(hal_uart_init(&u3, &cfg) == HAL_OK);
        uint8_t chunk[FAKE_UART_TX_CAPTURE_CAP];
        memset(chunk, 0x5A, sizeof(chunk));
        CHECK(hal_uart_send(&u3, chunk, sizeof(chunk)) == HAL_OK); /* fills capture exactly */
        CHECK(hal_uart_get_tx_dropped(&u3) == 0);
        CHECK(hal_uart_send(&u3, (const uint8_t *)"X", 1) == HAL_BUSY); /* buffer overflow */
        CHECK(hal_uart_get_tx_dropped(&u3) == 1);
        /* send_blocking overflow is a distinct status: capacity exhaustion,
         * not transient backpressure. */
        CHECK(hal_uart_send_blocking(&u3, (const uint8_t *)"X", 1, 10) == HAL_INVALID_SIZE);
    }

    /* --- deinit: handle stops being live; further calls -> HAL_NOT_READY --- */
    {
        hal_uart_t u4;
        memset(&u4, 0, sizeof(u4));
        CHECK(hal_uart_init(&u4, &cfg) == HAL_OK);
        CHECK(hal_uart_deinit(&u4) == HAL_OK);
        CHECK(fake_uart_is_live(&u4) == false);
        CHECK(hal_uart_send(&u4, (const uint8_t *)"x", 1) == HAL_NOT_READY);
        CHECK(hal_uart_deinit(&u4) == HAL_NOT_READY); /* double-deinit */
    }

    /* --- pool exhaustion: HAL_NO_MEM once FAKE_UART_MAX_INSTANCES is used --- */
    fake_uart_reset_all();
    {
        hal_uart_t handles[FAKE_UART_MAX_INSTANCES];
        for (int i = 0; i < FAKE_UART_MAX_INSTANCES; i++) {
            memset(&handles[i], 0, sizeof(handles[i]));
            CHECK(hal_uart_init(&handles[i], &cfg) == HAL_OK);
        }
        hal_uart_t one_too_many;
        memset(&one_too_many, 0, sizeof(one_too_many));
        CHECK(hal_uart_init(&one_too_many, &cfg) == HAL_NO_MEM);
        /* freeing one slot makes room again */
        CHECK(hal_uart_deinit(&handles[0]) == HAL_OK);
        CHECK(hal_uart_init(&one_too_many, &cfg) == HAL_OK);
    }

    /* --- hal_uart_attach: transitional Phase-1b entry point (interface/
     * hal_uart.h) -- attach a handle to a "port already installed elsewhere"
     * without going through hal_uart_init()/cfg, then prove sends work
     * exactly as they do on an init'd handle. --- */
    fake_uart_reset_all();
    {
        hal_uart_t attached;
        memset(&attached, 0, sizeof(attached));
        CHECK(hal_uart_attach(NULL, 7) == HAL_INVALID_ARG);
        CHECK(hal_uart_attach(&attached, 7) == HAL_OK);
        CHECK(fake_uart_is_live(&attached) == true);

        CHECK(hal_uart_send(&attached, (const uint8_t *)"hi", 2) == HAL_OK);
        CHECK(hal_uart_send_blocking(&attached, (const uint8_t *)"!", 1, 50) == HAL_OK);
        size_t attach_tx_len = 0;
        const uint8_t *attach_tx = fake_uart_tx_capture(&attached, &attach_tx_len);
        CHECK(attach_tx != NULL && attach_tx_len == 3 && memcmp(attach_tx, "hi!", 3) == 0);

        CHECK(hal_uart_deinit(&attached) == HAL_OK);
        CHECK(fake_uart_is_live(&attached) == false);
    }

    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

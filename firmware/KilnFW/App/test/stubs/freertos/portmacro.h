// Host-test stub for freertos/portmacro.h -- ESP-IDF's real header defines
// portMUX_TYPE/portENTER_CRITICAL/portEXIT_CRITICAL/portMUX_INITIALIZER_
// UNLOCKED as the SMP spinlock primitives relay_authority.c (App/drivers/)
// uses to guard its shared heat-claim state. Host tests are single-threaded
// (same reasoning as this directory's FreeRTOS.h/semphr.h stand-ins and
// test_safety_link_compile.c's own xSemaphoreTake redirect comment) so a
// critical section here needs no real mutual exclusion -- these expand to
// nothing at all, which is sufficient and correct for a program with no
// concurrent execution.
#ifndef KILNCTL_TEST_STUB_PORTMACRO_H
#define KILNCTL_TEST_STUB_PORTMACRO_H

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux)  ((void)(mux))

// taskENTER_CRITICAL/taskEXIT_CRITICAL -- the FreeRTOS task-level spelling of
// the same primitive (ota_pico_relay.c's s_status_mux guard uses these, not
// the port-level names above). Added for test_ota_pico_relay.c, the first
// host test to compile a file that calls them; same "expands to nothing,
// sufficient for a single-threaded host test" reasoning as portENTER_CRITICAL
// above.
#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux)  ((void)(mux))

#endif // KILNCTL_TEST_STUB_PORTMACRO_H

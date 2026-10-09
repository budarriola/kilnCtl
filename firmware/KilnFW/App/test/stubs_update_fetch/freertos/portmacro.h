// update_fetch host test: a real (spin) critical section, since the writer and TLS tasks are threads.
#ifndef UF_STUB_PORTMACRO_H
#define UF_STUB_PORTMACRO_H
#define KILNCTL_TEST_STUB_PORTMACRO_H
typedef volatile long portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void fr_crit_enter(portMUX_TYPE *m);
void fr_crit_exit(portMUX_TYPE *m);
#define portENTER_CRITICAL(m) fr_crit_enter(m)
#define portEXIT_CRITICAL(m) fr_crit_exit(m)
#define taskENTER_CRITICAL(m) fr_crit_enter(m)
#define taskEXIT_CRITICAL(m) fr_crit_exit(m)
#endif

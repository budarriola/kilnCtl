#ifndef CT_LEAK_ALARM_SERVICE_H
#define CT_LEAK_ALARM_SERVICE_H
#include "kiln_io.h"
#include "safety_link.h"

/* Bind the relay owner whose FULL shadow defines "every relay off". Until
 * bound, ct_leak_alarm_service() is a no-op. */
void ct_leak_alarm_service_bind(kiln_io_t *io_or_null);

/* One evaluation pass; self-throttled to 2 Hz. Called from safety_poll_task. */
void ct_leak_alarm_service(SafetyLinkClass *link);
#endif

// uart_owner_tx_policy.c -- see uart_owner_tx_policy.h.
#include "uart_owner_tx_policy.h"

bool uart_owner_tx_send_is_self_start_failure(uint32_t primed_this_call, uint32_t remainder)
{
    return primed_this_call == 0u && remainder > 0u;
}

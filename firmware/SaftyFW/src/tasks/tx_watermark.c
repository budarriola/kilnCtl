// tx_watermark.c -- see tx_watermark.h.
#include "tx_watermark.h"

bool tx_watermark_should_drop_log(float fill, float reserve_fraction)
{
    return fill >= reserve_fraction;
}

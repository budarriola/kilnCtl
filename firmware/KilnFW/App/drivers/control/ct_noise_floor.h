#ifndef CT_NOISE_FLOOR_H
#define CT_NOISE_FLOOR_H
/* The CT normal-current noise floor, shared by the current sweep engine
 * (zones_current_sweep_engine.c, which carries the full bench derivation in
 * the comment above its #include of this header) and the H9 CT alarm
 * (ct_leak_alarm.c). One definition on purpose: a mirrored copy is exactly
 * the drift class this repo keeps paying for. */
#include <math.h>

#define ZONE_SWEEP_NORMAL_NOISE_FLOOR_A 0.045f
/* The k_ct_v_per_a the floor above was derived against. */
#define ZONE_SWEEP_NORMAL_NOISE_FLOOR_REF_K_CT 1.0f

/* The floor rescaled to a channel's live committed k_ct_v_per_a, the same
 * rule the sweep engine applies inline: counts->amps is inversely
 * proportional to k_ct, so a smaller live k_ct makes the same noise read as
 * more amps. An uncommissioned (0), negative or non-finite k_ct falls back to
 * the unscaled floor. */
static inline float ct_noise_floor_a(float live_k_ct_v_per_a)
{
    if (isfinite(live_k_ct_v_per_a) && live_k_ct_v_per_a > 0.0f) {
        return ZONE_SWEEP_NORMAL_NOISE_FLOOR_A * (ZONE_SWEEP_NORMAL_NOISE_FLOOR_REF_K_CT / live_k_ct_v_per_a);
    }
    return ZONE_SWEEP_NORMAL_NOISE_FLOOR_A;
}
#endif

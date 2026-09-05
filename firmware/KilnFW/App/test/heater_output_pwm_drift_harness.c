/* heater_output_pwm_drift_harness.c -- links the REAL, unmodified
 * ../drivers/heater_output.c and drives heater_output_duty() with test
 * vectors read from stdin, one per line:
 *
 *   window_ms min_on_ms min_off_ms duty dt_ms n_ticks
 *
 * For each vector it resets a fresh heater_output_state_t and runs
 * n_ticks calls to heater_output_duty() with the SAME duty/dt_ms every
 * tick (matching plant_sim._pwm_render's calling convention: one call per
 * simulation tick), printing one output line per tick:
 *
 *   relay_on(0/1) cycle_count
 *
 * so the Python side can replay the identical tick sequence through
 * _pwm_render and diff every tick, not just the final state -- a
 * mid-run divergence (e.g. a window-boundary quantization difference)
 * that happens to self-correct by the last tick would otherwise hide.
 *
 * See heater_output_pwm_drift_check.py for why this harness exists.
 */
#include <stdio.h>
#include <string.h>

#include "../drivers/heater_output.h"

int main(void)
{
    char line[256];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        if (line[0] == '\n' || line[0] == '\0') {
            continue;
        }
        double window_ms, min_on_ms, min_off_ms, duty, dt_ms;
        int n_ticks;
        int n = sscanf(line, "%lf %lf %lf %lf %lf %d",
                        &window_ms, &min_on_ms, &min_off_ms, &duty, &dt_ms, &n_ticks);
        if (n != 6) {
            fprintf(stderr, "harness: bad input line: %s", line);
            return 1;
        }

        heater_output_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.window_ms = (uint32_t)window_ms;
        cfg.min_on_ms = (uint32_t)min_on_ms;
        cfg.min_off_ms = (uint32_t)min_off_ms;

        heater_output_state_t state;
        memset(&state, 0, sizeof(state));

        for (int i = 0; i < n_ticks; i++) {
            bool on = heater_output_duty(&state, &cfg, (float)duty, (uint32_t)dt_ms);
            printf("%d %u\n", on ? 1 : 0, state.cycle_count);
        }
        fflush(stdout);
    }
    return 0;
}

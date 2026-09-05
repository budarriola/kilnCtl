/* pid_fuzzy_drift_harness.c -- standalone driver for pid_fuzzy_adjust(),
 * built for pid_fuzzy_drift_check.py's numerical-equivalence comparison
 * against tools/PcTools/src/kilnctrl/fuzzy_band_probe.py's Python port.
 *
 * pid_fuzzy.c has no FreeRTOS/ESP-IDF/stub dependencies at all (see its own
 * header comment: "Pure C, no FreeRTOS, no ESP-IDF, no logging, no I/O, no
 * globals"), so this links directly against the real driver source with no
 * host-test stub tree required -- the ONLY thing this harness does is call
 * the real, unmodified pid_fuzzy_adjust() with each test vector read from
 * stdin and print the result, so a numeric mismatch against the Python side
 * can only be the C and Python math actually disagreeing, never a stub
 * standing in for something real.
 *
 * Input (stdin): one test vector per line, 8 whitespace-separated floats:
 *   error_c error_rate_c_per_s error_band_c rate_band_c_per_s base_kp
 *   base_ki base_kd strength_pct
 * "nan"/"inf"/"-inf" (case-insensitive) are accepted for the first two
 * fields, matching the NaN/inf test vectors test_pid_fuzzy.c already
 * exercises.
 *
 * Output (stdout): one line per input, 3 space-separated floats printed
 * with full round-trip precision (%.9g, more digits than a 32-bit float
 * needs) -- out_kp out_ki out_kd.
 */
#include "../drivers/pid_fuzzy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int main(void)
{
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == '\0' || line[0] == '\n' || line[0] == '#') {
            continue;
        }
        float error_c, error_rate, error_band, rate_band, base_kp, base_ki, base_kd, strength_f;
        int n = sscanf(line, "%f %f %f %f %f %f %f %f",
                        &error_c, &error_rate, &error_band, &rate_band,
                        &base_kp, &base_ki, &base_kd, &strength_f);
        if (n != 8) {
            continue;
        }
        uint8_t strength_pct = (uint8_t)(strength_f < 0.0f ? 0.0f : (strength_f > 255.0f ? 255.0f : strength_f));
        float out_kp = 0.0f, out_ki = 0.0f, out_kd = 0.0f;
        pid_fuzzy_adjust(error_c, error_rate, error_band, rate_band,
                          base_kp, base_ki, base_kd, strength_pct,
                          &out_kp, &out_ki, &out_kd);
        printf("%.9g %.9g %.9g\n", (double)out_kp, (double)out_ki, (double)out_kd);
    }
    return 0;
}

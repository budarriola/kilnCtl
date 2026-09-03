#include "cone_table.h"

#include <math.h>
#include <string.h>

// Gas constant, J/(mol*K).
#define CONE_TABLE_GAS_CONSTANT_J_PER_MOL_K 8.314f

// Activation energy for the heat-work Arrhenius approximation, J/mol. See
// cone_table.h's top-of-file comment for the full rationale -- this is an
// engineering approximation, not certified Orton kinetics data.
#define CONE_TABLE_EA_J_PER_MOL 300000.0f

#define CONE_TABLE_ABS_ZERO_C (-273.15f)

// Orton self-supporting cone equivalents at 108 F/hr (60 C/hr), converted
// from Orton's published Fahrenheit chart to Celsius. See cone_table.h for
// the source and heating-rate rationale. Spacing is intentionally
// non-uniform -- do not smooth or re-round these.
static const cone_table_entry_t s_cones[CONE_TABLE_COUNT] = {
    {"022", 586.1f},
    {"021", 600.0f},
    {"020", 626.1f},
    {"019", 677.8f},
    {"018", 717.2f},
    {"017", 747.2f},
    {"016", 803.9f},
    {"015", 826.7f},
    {"014", 842.8f},
    {"013", 857.2f},
    {"012", 872.8f},
    {"011", 893.9f},
    {"010", 902.8f},
    {"09", 920.0f},
    {"08", 942.2f},
    {"07", 976.1f},
    {"06", 997.8f},
    {"05", 1031.1f},
    {"04", 1062.8f},
    {"03", 1086.1f},
    {"02", 1102.2f},
    {"01", 1118.9f},
    {"1", 1137.2f},
    {"2", 1142.2f},
    {"3", 1152.2f},
    {"4", 1162.2f},
    {"5", 1186.1f},
    {"6", 1222.2f},
    {"7", 1238.9f},
    {"8", 1248.9f},
    {"9", 1260.0f},
    {"10", 1285.0f},
    {"11", 1293.9f},
    {"12", 1306.1f},
    {"13", 1323.9f},
    {"14", 1346.1f},
};

static bool is_finite_f(float v)
{
    return !isnan(v) && !isinf(v);
}

int cone_table_count(void)
{
    return CONE_TABLE_COUNT;
}

const cone_table_entry_t *cone_table_get(int index)
{
    if (index < 0 || index >= CONE_TABLE_COUNT) {
        return NULL;
    }
    return &s_cones[index];
}

cone_table_status_t cone_table_temp_c_for_cone(const char *cone_label, float *out_temp_c)
{
    if (cone_label == NULL || out_temp_c == NULL) {
        return CONE_TABLE_ERR_INVALID_INPUT;
    }
    for (int i = 0; i < CONE_TABLE_COUNT; i++) {
        if (strcmp(s_cones[i].cone_label, cone_label) == 0) {
            *out_temp_c = s_cones[i].temp_c;
            return CONE_TABLE_OK;
        }
    }
    return CONE_TABLE_ERR_INVALID_INPUT;
}

cone_table_status_t cone_table_cone_for_temp_c(float temp_c, int *out_index)
{
    if (out_index == NULL || !is_finite_f(temp_c)) {
        return CONE_TABLE_ERR_INVALID_INPUT;
    }
    if (temp_c < s_cones[0].temp_c) {
        return CONE_TABLE_ERR_OUT_OF_RANGE_LOW;
    }
    // Hottest cone whose equivalent temperature is <= temp_c. Table is
    // sorted ascending by construction, so walk from the top down.
    for (int i = CONE_TABLE_COUNT - 1; i >= 0; i--) {
        if (temp_c >= s_cones[i].temp_c) {
            *out_index = i;
            return CONE_TABLE_OK;
        }
    }
    // Unreachable given the low-range check above, but keep the function
    // total rather than falling off the end.
    return CONE_TABLE_ERR_OUT_OF_RANGE_LOW;
}

cone_table_status_t cone_table_band_bottom_c(float target_c, float *out_band_bottom_c)
{
    if (out_band_bottom_c == NULL || !is_finite_f(target_c)) {
        return CONE_TABLE_ERR_INVALID_INPUT;
    }
    if (target_c <= s_cones[0].temp_c) {
        // No lower cone to measure a band against.
        return CONE_TABLE_ERR_OUT_OF_RANGE_LOW;
    }
    if (target_c > s_cones[CONE_TABLE_COUNT - 1].temp_c) {
        return CONE_TABLE_ERR_OUT_OF_RANGE_HIGH;
    }

    // Find the bracketing lower cone: the hottest table entry strictly
    // below target_c. target_c may be an exact table entry (lower cone is
    // the previous entry) or fall between two entries (lower cone is the
    // entry directly below it) -- both cases are the same search.
    int lower_idx = -1;
    for (int i = CONE_TABLE_COUNT - 1; i >= 0; i--) {
        if (s_cones[i].temp_c < target_c) {
            lower_idx = i;
            break;
        }
    }
    if (lower_idx < 0) {
        // target_c <= s_cones[0].temp_c already handled above, so this
        // should not be reachable -- guard anyway.
        return CONE_TABLE_ERR_OUT_OF_RANGE_LOW;
    }

    float lower_temp_c = s_cones[lower_idx].temp_c;
    *out_band_bottom_c = target_c - (target_c - lower_temp_c) / 2.0f;
    return CONE_TABLE_OK;
}

// exp(-Ea / (R * T)), T in Kelvin. Caller guarantees T > 0.
static float arrhenius_rate(float temp_k)
{
    return expf(-CONE_TABLE_EA_J_PER_MOL / (CONE_TABLE_GAS_CONSTANT_J_PER_MOL_K * temp_k));
}

cone_table_status_t cone_table_heat_work_weight(float current_c, float target_c, float *out_weight)
{
    if (out_weight == NULL || !is_finite_f(current_c) || !is_finite_f(target_c)) {
        return CONE_TABLE_ERR_INVALID_INPUT;
    }
    if (current_c <= CONE_TABLE_ABS_ZERO_C || target_c <= CONE_TABLE_ABS_ZERO_C) {
        return CONE_TABLE_ERR_INVALID_INPUT;
    }

    float band_bottom_c;
    cone_table_status_t st = cone_table_band_bottom_c(target_c, &band_bottom_c);
    if (st != CONE_TABLE_OK) {
        return st;
    }
    if (band_bottom_c <= CONE_TABLE_ABS_ZERO_C) {
        return CONE_TABLE_ERR_INVALID_INPUT;
    }

    if (current_c >= target_c) {
        *out_weight = 1.0f;
        return CONE_TABLE_OK;
    }
    if (current_c <= band_bottom_c) {
        *out_weight = 0.0f;
        return CONE_TABLE_OK;
    }

    float t_target_k = target_c - CONE_TABLE_ABS_ZERO_C;
    float t_bottom_k = band_bottom_c - CONE_TABLE_ABS_ZERO_C;
    float t_current_k = current_c - CONE_TABLE_ABS_ZERO_C;

    float rate_target = arrhenius_rate(t_target_k);
    float rate_bottom = arrhenius_rate(t_bottom_k);
    float rate_current = arrhenius_rate(t_current_k);

    float denom = rate_target - rate_bottom;
    if (denom <= 0.0f || !is_finite_f(denom)) {
        // Degenerate band (target and band bottom collapsed to the same
        // temperature, or a numeric overflow) -- there is no meaningful
        // rate span to normalise against. Treat as "no credit" rather than
        // risk a divide-by-zero or a >1/negative result.
        *out_weight = 0.0f;
        return CONE_TABLE_OK;
    }

    float weight = (rate_current - rate_bottom) / denom;
    if (weight < 0.0f) {
        weight = 0.0f;
    } else if (weight > 1.0f) {
        weight = 1.0f;
    }
    *out_weight = weight;
    return CONE_TABLE_OK;
}

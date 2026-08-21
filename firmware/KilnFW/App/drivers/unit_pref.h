// unit_pref -- the single shared "does this board show Celsius or
// Fahrenheit" preference (ROADMAP.md / TODO.md: LCD and web UI must genuinely
// agree, not just each independently ship a client-side toggle).
//
// WHY THIS EXISTS. Until this module, no unit-of-measure preference existed
// anywhere on this board (verified 2026-08-19/21 by grepping every
// ui_page_*.c, board_temps.{c,h}, zones_http.c and every NVS key for
// unit/fahrenheit/celsius -- only false positives: "duty units", "padding
// units", LVGL internals). The LCD was Celsius-only, and the web UI shipped a
// client-side °C/°F toggle in app.js persisted to localStorage -- a
// per-browser setting that could disagree with the LCD sitting right next to
// it, and that a second browser (or a cleared cache) would silently forget.
// This module makes the device the one source of truth: the LCD and every
// browser that asks GET /api/status see the same answer, and a POST from
// either one is what the other sees on its next read.
//
// THE ONE HARD RULE THIS MODULE IS RESPONSIBLE FOR NOT VIOLATING: this is a
// DISPLAY-ONLY preference. It never touches how a temperature is
// transmitted, stored, or interpreted anywhere else in this firmware --
// profile segment targets, zone max/min temp, and the autotune setpoint are
// always Celsius on the wire and on flash, full stop, regardless of what this
// module answers. A setpoint that silently changed units depending on a
// display preference would be a genuine hazard on a kiln (a "cone 6" 1222C
// target read back as if it were 1222F would run the load nearly 700C over
// its actual target). Every call site that uses unit_pref_c_to_display()
// below is therefore a read-only rendering of an already-Celsius number, never
// a step in an input/storage path. Conversion RATES (ramp C/hr, hysteresis
// half-width, the plant model's C/duty gain) must never be passed through
// unit_pref_c_to_display() at all -- it is an ABSOLUTE-reading conversion
// (C*9/5+32), and applying that same affine map to a delta/rate silently
// changes its meaning (a rate has no "+32" term to add). No call site in this
// pass does this; if a future one is tempted to, that is the bug to catch in
// review, not something this module can enforce structurally.
#ifndef KILNCTL_UNIT_PREF_H
#define KILNCTL_UNIT_PREF_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UNIT_PREF_CELSIUS = 0,    // shipped default -- every existing board/blob
                               // with no saved preference reads as this.
    UNIT_PREF_FAHRENHEIT = 1,
} unit_pref_t;

// Loads the persisted preference from NVS (kiln_nvs partition, "kiln_cfg"
// namespace -- the same partition/namespace zones_http.c and
// profiles_builtin.c already use for small board-wide config, so this does
// not acquire a fourth place to look). Missing key (fresh board) or a
// corrupt/out-of-range stored value both fall back to UNIT_PREF_CELSIUS
// rather than failing app_main -- an operator staring at a board that will
// not boot over a units *display* preference is a worse failure mode than
// silently defaulting to the unit this firmware always used before this
// module existed.
esp_err_t unit_pref_start(void);

// Current in-RAM preference. O(1), no NVS access -- every renderer on the hot
// LCD refresh path (ui_page_home.c's 2s tick, etc.) calls this directly.
unit_pref_t unit_pref_get(void);

// Validates and persists a new preference, same "reject invalid outright,
// never partially apply" discipline as zones_http.c's zones_config_set_*()
// functions. Updates the in-RAM value first (live for the very next LCD
// redraw / API poll) then writes NVS, matching zones_config_set_pid()'s
// "bumped before the NVS write" reasoning -- a save failure here means the
// choice will not survive a reboot, not that it failed to take effect now.
esp_err_t unit_pref_set(unit_pref_t pref);

// "C" or "F" -- the suffix every LCD temperature label appends after
// unit_pref_c_to_display() converts the number itself.
const char *unit_pref_suffix(unit_pref_t pref);

// Which affine map unit_pref_convert() below applies. Made a required
// argument (not a per-call-site judgement call) specifically because an
// earlier draft of this module had exactly one function and left "is this an
// absolute reading or a rate" to whoever called it -- precisely the kind of
// thing a later edit gets wrong under time pressure. The compiler now forces
// every call site to say which one it means.
typedef enum {
    // C*9/5+32. For a real physical temperature: a sensor reading, a
    // setpoint, a max/min limit. NEVER for a rate -- 32 is a fixed point on
    // the Celsius scale (freezing), and a rate has no such point to add.
    UNIT_PREF_KIND_ABSOLUTE,
    // C*9/5, no offset. For a DELTA: ramp_c_per_hr, sanity_rate_c_per_min,
    // guard_wrong_dir_rate_c_per_min, guard_runaway_rate_c_per_min, the FOPDT
    // model's C-per-duty gain, a hysteresis half-width. Converts the
    // magnitude of a temperature difference, never an absolute reading --
    // and the caller MUST update the unit's label alongside the number (e.g.
    // "C/hr" -> "F/hr"): a rate scaled without its label changing is silent
    // corruption, not a conversion. As of 2026-08-21 no call site in this
    // codebase actually renders a rate in the display unit -- every rate
    // field on both the LCD and the web UI is left in C/hr with its C/hr
    // label (unit_pref.h's "if in doubt, leave a rate in C" rule) -- this
    // variant exists so that decision is structural (a rate conversion is
    // only ever one explicit function argument away from an absolute one,
    // never the default) rather than because anything currently calls it.
    UNIT_PREF_KIND_RATE,
} unit_pref_kind_t;

// Converts an already-Celsius value to the given display unit, per `kind`
// above. Celsius is always a no-op regardless of kind.
//
// Every call site in this codebase as of 2026-08-21 passes
// UNIT_PREF_KIND_ABSOLUTE with a live sensor reading (board_temps/dashboard
// channel/CJ/safety/enclosure temperatures) -- never a rate, and never with
// UNIT_PREF_KIND_RATE (see that enumerator's comment for why one exists
// anyway).
float unit_pref_convert(float value_c, unit_pref_t pref, unit_pref_kind_t kind);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_UNIT_PREF_H

# Credits & Third-Party Attributions

This file lists third-party source files, libraries, or reference designs that are
**copied or vendored directly into this repository** (not merely installed as a package
manager dependency at build time — those don't need an entry here). It exists so the
origin and license terms of anything copied into this tree stays traceable. **Keep it up
to date**: whenever you copy or vendor a new third-party file, library, or reference
design into the repo, add an entry here in the same pull request.

- **[SamacSys / Ultra Librarian component libraries](https://componentsearchengine.com/)**
  — SamacSys Ltd. (component data now distributed via the Ultra Librarian / Supplyframe
  ecosystem) — Auto-generated KiCad schematic symbols, footprints, and 3D step models
  copied into `mainBoard/parts/SamacSys_Parts.kicad_sym`,
  `mainBoard/parts/SamacSys_Parts.pretty/`, and `mainBoard/parts/SamacSys_Parts.3dshapes/`
  (identifiable by the `(generator SamacSys_ECAD_Model)` tag in the `.kicad_sym` file);
  used for parts without a suitable symbol/footprint in KiCad's own bundled libraries.
- **[BIGTREETECH TFT35-SPI](https://github.com/bigtreetech/TFT35-SPI)** — BigTreeTech —
  Reference hardware design (schematics, 3D models, user manual) for the TFT35 SPI
  display, vendored as a git submodule at `mainBoard/parts/TFT35-SPI`; used as a hardware
  reference for the display interface.
- **[mykicadMcp](https://github.com/budarriola/mykicadMcp)** — budarriola (this
  project's own author) — separate git submodule at `tools/mykicadMcp` holding the KiCad
  MCP server; own repository/history, so listed here for traceability even though it is
  not third-party. Licence: see that submodule's own repo.
- **[Digital Fire firing schedules](https://digitalfire.com/schedule)** — Digital Fire
  Corporation — 28 published cone-fire schedules, reproduced for convenience as the
  built-in (read-only) firing profiles shipped in `KilnFW` flash
  (`profiles_builtin.c` + the generated `profiles_builtin_table.inc`); credited on the
  web Profiles page and per entry via a source link. Third-party published data, not
  vendored source code, but listed here for the same traceability reason.
- **[Orton pyrometric cone temperature equivalents](https://www.ortonceramic.com/)** —
  The Edward Orton Jr. Ceramic Foundation — self-supporting cone temperature table
  (cone 022 through 14) at Orton's published 108°F/hr (60°C/hr) "medium speed"
  reference heating rate, transcribed in
  `firmware/KilnFW/App/drivers/cone_table.c`/`.h`. Cone temperatures are only
  meaningful with a stated heating rate, so that rate is part of the attribution, not
  incidental. The module's `cone_table_heat_work_weight()` Arrhenius-form weighting
  (Ea = 300 kJ/mol) is **not** Orton data — it is this project's own engineering
  approximation of cone kinetics, as the file's own header states; do not attribute it
  to Orton.
- **[LVGL](https://github.com/lvgl/lvgl)** — LVGL Kft — MIT licence — graphics/widget
  library vendored as a git submodule at `firmware/KilnFW/components/lvgl`; drives the
  KilnFW front-panel display (`lvgl_port.c`, `ui_page_*.c`).

- **PID tuning rules implemented in `firmware/KilnFW/App/drivers/pid_autotune.c`** —
  classic control-theory formulas coded directly into `pid_autotune_tune_from_fopdt()`
  and `pid_autotune_tune_from_relay()` (selectable via `autotune_rule_t`, surfaced in
  `zones_page.html` and `tools/PcTools/src/kilnctrl/`), not merely referenced:
  - **SIMC** — S. Skogestad, *"Simple analytic rules for model reduction and PID
    controller tuning"*, 2003 — the default step-test rule; `Kc = tau/(K*(lambda+L))`.
  - **Cohen-Coon** — G. H. Cohen & G. A. Coon, *"Theoretical Consideration of Retarded
    Control"*, ASME Transactions, 1953 — opt-in, more aggressive step-test rule.
  - **Ziegler-Nichols** and **Tyreus-Luyben** — J. G. Ziegler & N. B. Nichols,
    *"Optimum Settings for Automatic Controllers"*, ASME Transactions, 1942; B. D.
    Tyreus & W. L. Luyben, *"Tuning PI Controllers for Integrator/Dead Time Processes"*,
    1992 — relay-test rules derived from the identified ultimate gain/period (Ku, Tu);
    Tyreus-Luyben is the relay-path default, Ziegler-Nichols is opt-in.
  - **Relay-feedback identification (describing function)** — K. J. Åström & T.
    Hägglund, *"Automatic Tuning of Simple Regulators with Specifications on Phase and
    Amplitude Margins"*, Automatica, 1984 — `pid_autotune_fit_relay()`'s
    `Ku = 4d/(pi*sqrt(a^2-h^2))` describing-function fit for a relay test with
    hysteresis.
- **Research papers informing KilnFW's PID control design** — various authors — **cited,
  not vendored.** The PDFs are deliberately *not* in this repo (`/docs/research/` is
  gitignored): they are third-party published papers whose redistribution terms nobody
  here has cleared, and they are large binaries no clone needs. Only these citations are
  tracked, so anyone can re-fetch a paper from its source. Ten were read while writing
  `firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`; the five below are the ones whose findings
  actually informed a recommendation or shipped code (see that plan's §1-2 for how each is
  used):
  - Josefin Berner, *PhD thesis on relay-feedback PID autotuning* — Lund University,
    <http://lup.lub.lu.se/search/ws/files/33100749/ThesisJosefinBerner.pdf> — decentralized
    relay-feedback identification for coupled loops; informs the cross-zone coupling
    extension (§2c).
  - Nichols Philips et al., *Application of Auto Tuner Fuzzy PID Controller* — fuzzy
    cascade PID adapting to a changing process model; informs the fuzzy-PID secondary
    recommendation (§2b), implemented as `App/drivers/pid_fuzzy.c`.
  - *Implementation of Fuzzy PID Controller on [a PT326 heating rig]* — fuzzy PID over a
    system-identified ARX model, validated against real hardware; also informs §2b.
  - *Research on temperature control with numerical methods* — Cohen-Coon vs.
    Ziegler-Nichols vs. hysteresis on a real electric resistance furnace; motivates the
    Cohen-Coon tuning rule added to `pid_autotune_tune_from_fopdt()`.
  - *Tuning Optimization of Hybrid controller* — hybrid PI + feedforward on a shell-and-tube
    heat exchanger; corroborates this codebase's existing model-based feedforward term.

  The other five read (self-tuning fuzzy PID for HVAC, two fractional-order PID reviews, a
  Q-learning/GA fuzzy hybrid, and a metaheuristic DC-motor PID optimizer) were considered
  and explicitly *not* recommended — see the plan's §2 — so they are cited there but not
  credited here as the source of any implemented technique.

---

*Last updated: 2026-09-02. If you copy or vendor a new third-party file, library, or
reference design into this repo, add an entry here in the same pull request. Plain
package-manager dependencies (pip packages, ESP-IDF managed components, etc.) don't need
an entry — only things actually copied into the tree.*

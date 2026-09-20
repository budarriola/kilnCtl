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
  `firmware/KilnFW/App/drivers/control/cone_table.c`/`.h`. Cone temperatures are only
  meaningful with a stated heating rate, so that rate is part of the attribution, not
  incidental. The module's `cone_table_heat_work_weight()` Arrhenius-form weighting
  (Ea = 300 kJ/mol) is **not** Orton data — it is this project's own engineering
  approximation of cone kinetics, as the file's own header states; do not attribute it
  to Orton. Ten of the 36 entries (cones 011-018 and 13/14) originally shipped wrong
  (15-56°C off, in one case sourced from the wrong chart column entirely) and were
  corrected against Orton's own Self-Supporting 108°F/hr chart as mirrored at
  `hotkilns.com/sites/default/files/pdf/cone-chart.pdf` (rendered and read directly,
  not OCR'd) — see commit `9362e84` and `firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`
  §7.3.1 for the correction detail.
- **[LVGL](https://github.com/lvgl/lvgl)** — LVGL Kft — MIT licence — graphics/widget
  library vendored as a git submodule at `firmware/KilnFW/components/lvgl`; drives the
  KilnFW front-panel display (`lvgl_port.c`, `ui_page_*.c`). Bundles **TJpgDec** (JPEG
  decoder, `components/lvgl/src/libs/tjpgd/`) — © ChaN, 2021, permissive
  "no restriction on use, retain the copyright notice" licence (see that directory's
  own `LICENSE.txt`) — enabled via `LV_USE_TJPGD`/`CONFIG_LV_USE_TJPGD` for on-device
  JPEG decoding.
- **Vendor datasheets copied into `firmware/KilnFW/Datasheets/`** — respective
  manufacturers — datasheets for ICs with a digital comm interface (I2C/SPI/UART) that
  KilnFW talks to, kept alongside the firmware for reference; see that directory's own
  `README.md` for the full per-file table and sources. Currently: `MAX31856.pdf`
  (Maxim/Analog Devices, SPI thermocouple ADC), `SX1509.pdf` (Semtech, I2C GPIO
  expander), `RaspberryPi_Pico.pdf` (Raspberry Pi Foundation, UART peer MCU),
  `ILI9488.pdf` (Ilitek, SPI TFT controller), `XPT2046.pdf` (SPI touch controller,
  mirrored copy), `ESP32-S3_datasheet.pdf` and `ESP32-S3-DevKitC-1_user_guide.pdf`
  (Espressif), and the curated `4.0inch_SPI_Module_ST7796_MSP4030_MSP4031_V1.0_Keep/`
  directory (LCDWIKI/Elecrow ST7796S + FT6336U candidate-display reference package,
  pruned from an untracked ~394 MB vendor drop to ~11 MB of tracked files — see
  `firmware/KilnFW/docs/DISPLAY_ST7796_PLAN.md`).

- **PID tuning rules implemented in `firmware/KilnFW/App/drivers/control/pid_autotune.c`** —
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
    recommendation (§2b), implemented as `App/drivers/control/pid_fuzzy.c`.
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

## Research and control literature (scenario-simulation and coupling-model passes)

Sources actually consulted in `docs/research/` and `docs/SCENARIO_SIMULATION_PLAN.md`
§3, beyond the PID-autotune papers already credited above. Full detail, quotations, and
retrieval caveats (abstract-only, search-summary-only, paywalled) live in the two
`docs/research/*.md` files themselves — this entry only records which findings actually
reached a design or shipped code, per the owner's 2026-08-30 instruction to credit only
what was used.

**Used — informed an adopted design or shipped code:**

- Watlow Electric Manufacturing Co., *"Sensor placement in a thermal system"*
  (engineering knowledge-base article, no publication date given on the page),
  <https://www.watlow.com/resources-and-support/engineering-tools/knowledge-base/sensor-placement-in-a-thermal-system>
  — states the near-source-vs-near-load sensor-placement tradeoff (loop stability vs.
  load fidelity) that `docs/SCENARIO_SIMULATION_PLAN.md` §2.1's three-node sensor model
  (`sensor_bias_p`, exercised by `firmware/KilnFW/App/test/sim_plant.c`) is built around.
  Per §3.1, the source supports the tradeoff qualitatively only — it gives no
  conductance ratio; the plan's own 5:1 numeric interpretation is this project's
  inference, not the source's.
- RTP (rapid thermal processing) multi-zone gain-scheduling literature — Schaper, C.D.
  and Edgar, T.F. & Breedijk, T. (1994), title/venue not independently verified this
  session; found only via secondary summaries at
  <https://www.researchgate.net/publication/234065612_Modeling_and_Control_of_Rapid_Thermal_Processing>
  and <https://ir.lib.nycu.edu.tw/bitstream/11536/29999/1/000166627500015.pdf> (**gap:**
  primary papers not read in full, so the exact title/journal of the 1994 works is not
  confirmed from this repo's own research pass — see
  `docs/research/multizone_thermal_modelling_literature_2026-09-11.md` §"3. Setpoint/
  level-dependent gain scheduling"). The RTP literature's practice of scheduling a
  multi-lamp-zone interaction-gain matrix by operating point/setpoint is the recommended
  ("candidate 3", ranked #1) replacement for kilnCtl's refuted additive linear coupling
  model, and informs the setpoint-dependent zone-interaction-matrix design in
  `firmware/KilnFW/App/drivers/control/zone_coupling_solve.c`. Note the literature
  supports scheduling coupling *magnitude* by level, not the coupling *sign reversal*
  this project separately measured on its own bench data — that reversal is this
  project's own finding, not the RTP source's.

**Surveyed, not used** (kept here only so a reader can tell the difference from the list
above; do not treat these as informing any shipped design):

- Jin, Renjie, *"Research on Optimized Fuzzy PID Temperature Control Strategy Based on
  Improved Particle Swarm Optimization"*, arXiv:2609.00001 (2026) —
  <https://arxiv.org/abs/2609.00001> — abstract only; per
  `docs/SCENARIO_SIMULATION_PLAN.md` §3.2, its reported effect sizes are for a
  full-authority fuzzy design and do not transfer to this project's bounded ±50% nudge
  (`pid_fuzzy.c`).
- Comparative expert-adjustable-fuzzy-control study (injection-molding temperature
  control), <https://pmc.ncbi.nlm.nih.gov/articles/PMC9252661/> — search-summary only,
  same non-transferability conclusion, §3.2.
- Visioli, A., *"Fuzzy logic based set-point weight tuning of PID controllers"*, IEEE
  SMC, 1999, <https://ieeexplore.ieee.org/document/798062/>, and its Springer follow-up
  (*"Fuzzy rule-based set point weighting for fuzzy PID controller"*,
  <https://link.springer.com/article/10.1007/s42452-021-04626-0>) — motivated a
  "consider tuning it, for overshoot, not ramp tracking" recommendation for the
  already-present `PID_SETPOINT_WEIGHT_B` constant in
  `docs/research/fuzzy_ramp_tracking_2026-09-13.md` §6. Not adopted: the constant is
  still `1.0f` (`firmware/KilnFW/App/drivers/control/profile_executor_internal.h:253`,
  as of this writing), i.e. unused/inert.
- The remaining furnace/fuzzy-PID and setpoint-weighting sources listed in
  `docs/research/fuzzy_ramp_tracking_2026-09-13.md`'s own "Sources" bibliography —
  surveyed to answer whether the fuzzy layer could also serve ramp tracking; the
  document's conclusion was "leave the rule table alone," so none of them changed
  shipped code. See that file for the full list and per-source notes.
- Sonta, Simmons, et al., *"Data-driven identification of a thermal network in
  multi-zone building"*, arXiv:1810.07400 (2018), and Cen et al., *"Lumped Parameter
  Thermal Network Modeling and Thermal Optimization Design of an Aerial Camera"*,
  PMC/NCBI (2024), <https://www.ncbi.nlm.nih.gov/pmc/articles/PMC11207309/> — shared-node
  RC-network coupling model ("candidate 1" in
  `docs/research/multizone_thermal_modelling_literature_2026-09-11.md`), ranked below
  the adopted candidate 3 and not implemented.
- Bilinear (input × state) coupling-term literature (arXiv:1802.06165 and
  ResearchGate 343151731) — "candidate 2" in the same document, not implemented.
- Deep-learning multi-zone furnace prediction (graph attention + GRU), ScienceDirect,
  <https://www.sciencedirect.com/science/article/abs/pii/S0735193325010504> — the
  document's own "Does not apply, and why" section; not implemented.

---

*Last updated: 2026-09-20. If you copy or vendor a new third-party file, library, or
reference design into this repo, add an entry here in the same pull request. Plain
package-manager dependencies (pip packages, ESP-IDF managed components, etc.) don't need
an entry — only things actually copied into the tree.*

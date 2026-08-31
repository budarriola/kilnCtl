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
- **[Digital Fire firing schedules](https://digitalfire.com/schedule)** — Digital Fire
  Corporation — 28 published cone-fire schedules, reproduced for convenience as the
  built-in (read-only) firing profiles shipped in `KilnFW` flash
  (`profiles_builtin.c` + the generated `profiles_builtin_table.inc`); credited on the
  web Profiles page and per entry via a source link. Third-party published data, not
  vendored source code, but listed here for the same traceability reason.

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

*Last updated: 2026-08-30. If you copy or vendor a new third-party file, library, or
reference design into this repo, add an entry here in the same pull request. Plain
package-manager dependencies (pip packages, ESP-IDF managed components, etc.) don't need
an entry — only things actually copied into the tree.*

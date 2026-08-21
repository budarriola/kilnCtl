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

---

*Last updated: 2026-08-20. If you copy or vendor a new third-party file, library, or
reference design into this repo, add an entry here in the same pull request. Plain
package-manager dependencies (pip packages, ESP-IDF managed components, etc.) don't need
an entry — only things actually copied into the tree.*

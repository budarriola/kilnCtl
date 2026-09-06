# firmware/hwAbstraction

One hardware-abstraction tree shared by KilnFW (ESP32-S3) and SaftyFW
(RP2040). Full design and phased rollout: `docs/HW_ABSTRACTION_PLAN.md`.

```
hwAbstraction/
  interface/   portable headers only (backend-independent). DONE (Phase 0).
  common/      vendor-neutral shared code (hal_status.c).
  esp/         ESP-IDF backends. Empty -- Phase 1a/1b.
  pico/        pico-sdk backends. Empty -- Phase 1a/1b.
  host/        fake backends for MSVC host tests. Empty -- Phase 2.
  test/        compile_headers.ps1 -- MSVC syntax/ABI check for interface/.
```

Status: Phase 0 (scaffold + contract) only. Nothing here is wired into any
CMakeLists or build yet; no caller has changed. See the plan's "Phases"
section for what each later phase adds.

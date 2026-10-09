# One-step-at-a-time config migration (pending work only)

Policy, step signature, quarantine rules, testing, expiry (D1/D2/D4),
enforcement checks and the PC converter's landed scope live in
`docs/CONFIG_MIGRATION_CHAIN.md`. Owner requirement (2026-09-16): each new
firmware supports migration of the nearest configuration forward, one way, one
step at a time. All four original open decisions are settled there (section 6).

Pending:

- **PC converter: `zones_cfg_t` v1..v20.** `tools/PcTools/src/kilnctrl/config_convert.py`
  refuses these by name. v20->v21 grew `settings_source` mid-struct and
  v1..v6 predate `crc32`, so the tail-append trick used for v21..v25 does not
  reach them. Gate: a concrete need to convert such a blob, plus a blob of that
  exact version (a firmware host test emitting one per historical struct, or a
  captured one) to verify the port against. D2 already expires the pre-v26
  tail on firmware, so do not port speculatively.
- **Caveat, hardware-gated:** the host `esp_crc32_le` is a C stub, so no test
  proves the on-target ROM routine matches the Python CRC; no
  hardware-captured blob has been compared. Close by comparing one blob
  captured from a board.

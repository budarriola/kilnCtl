# docs/research/

Literature-review notes written while investigating specific control-design questions.
These two files are tracked despite `/docs/research/` being gitignored (that ignore rule
targets the PID-planning PDFs, not these two markdown reviews).

- `multizone_thermal_modelling_literature_2026-09-11.md` — search for a replacement for
  the refuted additive linear zone-coupling model in `sim_plant.c`.
- `fuzzy_ramp_tracking_2026-09-13.md` — whether the fuzzy PID layer (`pid_fuzzy.c`) can
  also help ramp tracking, not just overshoot.

**Which findings actually reached shipped code or an adopted design — and which were
surveyed and rejected — is tracked in the top-level `CREDITS.md`'s "Research and control
literature" section, not here.** Read that section first if you're trying to find out
whether a specific paper matters to the current implementation; these files are the full
research trail, including dead ends, abstract-only citations, and unverified claims.

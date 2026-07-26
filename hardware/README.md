# hardware/

Hardware design sources for MOTO-CTRL, licensed under CC BY-NC-SA 4.0 (see
[`../LICENSE-HARDWARE`](../LICENSE-HARDWARE) and
[`../LICENSE-NOTE.md`](../LICENSE-NOTE.md)).

- [`PINOUT.md`](PINOUT.md) — the firmware ↔ hardware contract: every GPIO
  assignment on the current board revision. This is the single source of
  truth firmware pin definitions must be generated from (see
  [`../CONTRIBUTING.md`](../CONTRIBUTING.md)).
- `releases/<rev>/` — per-hardware-revision design exports (EasyEDA Pro
  project export, schematic PDF, `gerbers/`, `bom/`, `pnp/`), tagged in git
  to match the revision. `PINOUT.md` documents the current/latest revision;
  each `releases/<rev>/` snapshot is what actually shipped for that
  revision.

## Revisions

| Revision | Status | Notes |
|----------|--------|-------|
| v2 ("Integrated V2") | current | See `PINOUT.md`, dated 2026-07-18. Design files to be added to `releases/v2/`. |

Only one PROFET may have DEN high at a time, GPIO3/GPIO46 are strapping
pins used safely by design (see `PINOUT.md`'s warning) — if you're revising
the hardware, re-read `PINOUT.md` in full before changing pin assignments,
since firmware depends on it directly.

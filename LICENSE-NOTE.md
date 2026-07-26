# Licensing — plain-English summary

This file is a convenience summary. It is **not** a substitute for the actual
license texts, which are the legally binding documents:

- [`LICENSE-CODE`](LICENSE-CODE) — PolyForm Noncommercial 1.0.0 — covers everything
  in `firmware/`, `app/`, `tools/`, and any other source code in this repository.
- [`LICENSE-HARDWARE`](LICENSE-HARDWARE) — Creative Commons
  Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0) — covers
  everything in `hardware/` and `enclosure/` (schematics, PCB layouts, gerbers,
  BOMs, STL/STEP files) and the prose in `docs/`.

If the two licenses ever appear to conflict for a given file, the license text
that governs that file's directory controls; this note does not override either
license.

## What this means in practice

- **You can build one for yourself.** Download the design, fabricate or order
  boards, assemble, flash, and run MOTO-CTRL on your own bike. That's a permitted
  noncommercial personal use under both licenses.
- **You can modify it.** Fork it, change the firmware, redesign the enclosure,
  adapt the PCB for your own bike — for noncommercial purposes.
- **You can share your noncommercial work.** Publish your fork, write about it,
  post it to a forum — as long as it stays noncommercial and (for hardware/docs)
  is shared under the same CC BY-NC-SA terms with attribution.
- **You cannot sell boards, kits, or assemblies built from this design.** Selling
  assembled MOTO-CTRL boards, PCBs fabricated from these files, enclosures printed
  from these files, or bundles/kits containing them is a commercial use and is not
  permitted under either license.
- **You cannot sell derivatives either.** A modified fork of the firmware, app, or
  hardware is still subject to the noncommercial restriction — changing the design
  does not create a right to sell it.
- **Assembled boards are sold only by the MOTO-CTRL project.** The only authorized
  commercial source of assembled MOTO-CTRL boards is the MOTO-CTRL project itself
  (Speeduino-style: open design, commercial boards sold by the project maintainer).
  This is how the project stays funded while remaining fully open-source.
- **No warranty, use at your own risk.** See [`DISCLAIMER.md`](DISCLAIMER.md) — this
  is not a certified automotive safety device.

If you want to use MOTO-CTRL commercially in some way not covered above (e.g. a
shop offering installation services, a fleet operator, an educational program),
open an issue or contact the maintainer — commercial licensing arrangements may be
possible outside of these default terms.

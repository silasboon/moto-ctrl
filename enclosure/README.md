# enclosure/

Enclosure design files for MOTO-CTRL, licensed under CC BY-NC-SA 4.0 (see
[`../LICENSE-HARDWARE`](../LICENSE-HARDWARE)).

- FDM ASA — prototype/hobbyist printing.
- MJF PA12 — production.

Two printed parts per revision — a base and a separate lid:

```
enclosure/
└── v1/
    ├── moto-ctrl-enclosure-v1.step   # base, source/editable
    ├── moto-ctrl-enclosure-v1.stl    # base, print-ready
    ├── moto-ctrl-lid-v1.step         # lid, source/editable
    └── moto-ctrl-lid-v1.stl          # lid, print-ready
```

Per-revision print settings and material notes (layer height, walls,
infill, orientation, supports, and what differs between the ASA prototype
and the MJF production part) are **not written up yet** — print the STLs
with sensible ASA defaults until they are.

Enclosure revisions should track hardware revisions in `hardware/releases/`
— an enclosure for board rev v1 assumes the connector/mounting layout of
`hardware/releases/v1/`.

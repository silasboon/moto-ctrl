# enclosure/

Enclosure design files for MOTO-CTRL, licensed under CC BY-NC-SA 4.0 (see
[`../LICENSE-HARDWARE`](../LICENSE-HARDWARE)).

- FDM ASA — prototype/hobbyist printing.
- MJF PA12 — production.

Not yet populated. Expected layout once added:

```
enclosure/
├── v2/
│   ├── moto-ctrl-enclosure-v2.step   # source, editable
│   ├── moto-ctrl-enclosure-v2.stl    # print-ready
│   └── README.md                     # print settings, material notes
```

Enclosure revisions should track hardware revisions in `hardware/releases/`
— an enclosure for board rev v2 assumes the connector/mounting layout of
`hardware/releases/v2/`.

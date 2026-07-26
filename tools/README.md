# tools/

Scripts supporting development and releases.

- [`bootstrap-app-native.sh`](bootstrap-app-native.sh) — generates
  `app/ios/` and `app/android/`, the native React Native project files not
  committed by default in this scaffold (see
  [`../app/NATIVE_SETUP.md`](../app/NATIVE_SETUP.md)).
- [`sign-firmware.py`](sign-firmware.py) — OTA release tooling
  (`../docs/PROTOCOL.md` §10.4-10.5). Requires PyNaCl (`pip install
  pynacl`, Apache-2.0). Two steps, run from a maintainer's machine:

  1. **Once ever**: `sign-firmware.py gen-key <path>` generates the OTA
     release Ed25519 keypair. Store `<path>` (the private key) **outside
     this repo** — a password manager or encrypted volume, never committed,
     never in a hosted CI secrets store — and paste the printed public key
     into `../firmware/components/core/mc_ota_release_key.c`, replacing the
     placeholder there (see that file's header comment). Losing this key
     means generating a new one and re-flashing every device's public key
     over UART — there is no recovery.
  2. **Per release**: after building `moto_ctrl_firmware.bin`,
     `sign-firmware.py sign --input moto_ctrl_firmware.bin --key <path>
     --output moto-ctrl-X.Y.Z.mcota --manifest manifest.json --version
     X.Y.Z --bundle-url <where you'll host the .mcota file>` produces the
     signed `.mcota` bundle the app's firmware-update screen downloads and
     flashes, plus the small `manifest.json` the app's baked-in
     update-check URL serves (see `../CONTRIBUTING.md`'s "No cloud, no
     telemetry, no accounts" section for the exact constraints on that
     exception). Upload both to wherever `--bundle-url` points (e.g. a
     GitHub Release).

  `firmware/sim` never uses the real release key — it has its own fixed
  TEST keypair (`../firmware/sim/src/main.c`) so CI/itest/app tests can
  exercise OTA without this script or the real key ever being involved.

Not yet added:

- Config export/import scripts (JSON config round-trip) — the config JSON
  is already forward/backward tolerant by design (see `mc_config.c`'s
  header comment), so this would be a convenience wrapper around the
  existing CONFIG channel, not a required migration tool.
- Release packaging for app builds and hardware release bundles under
  `hardware/releases/<rev>/`.

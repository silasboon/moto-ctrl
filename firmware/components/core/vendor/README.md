# Vendored third-party libraries

These are unmodified upstream sources, vendored so the portable core builds
identically on the ESP32 target and the host simulator without depending on
whatever the platform happens to provide. Both are permissively licensed
(compatible with this project's noncommercial model — see
[`../../../../LICENSE-NOTE.md`](../../../../LICENSE-NOTE.md); neither is
copyleft).

## tweetnacl/ — TweetNaCl

- **Source:** <https://tweetnacl.cr.yp.to/> (`20140427` release,
  `tweetnacl.c` / `tweetnacl.h`).
- **License:** Public domain (dedicated to the public domain by its authors:
  Bernstein, van Gent, Lange, Schwabe, et al.).
- **Why:** Ed25519 sign/verify for the phone-as-key challenge-response.
  Chosen because it is the same primitive/implementation family as
  `tweetnacl-js`, so signatures produced on the phone (JS) verify on the
  device (C) with no interop surprises. On the device firmware only the
  *verify* path (`crypto_sign_open`) is exercised — the phone holds the
  private key and signs; the device stores only public keys.
- **Note:** TweetNaCl requires the integrator to supply a `randombytes()`
  symbol. We provide it in [`../mc_crypto.c`](../mc_crypto.c) (backed by
  `esp_random()` on target, `/dev/urandom` on host). Do not add a second
  definition.
- **Do not edit these files.** If they ever need updating, re-vendor from
  upstream and note the new release here.

## cjson/ — cJSON

- **Source:** <https://github.com/DaveGamble/cJSON> (`cJSON.c` / `cJSON.h`);
  upstream `LICENSE` preserved here as `LICENSE`.
- **License:** MIT.
- **Why:** JSON encode/decode for the config service, which the protocol
  spec defines as chunked JSON (see [`../../../../docs/PROTOCOL.md`](../../../../docs/PROTOCOL.md)).
  Vendored rather than using ESP-IDF's bundled `json` component so the same
  code runs in host unit tests.
- **Do not edit these files.** Re-vendor from upstream to update.

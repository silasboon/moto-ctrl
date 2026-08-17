#!/usr/bin/env python3
"""sign-firmware.py -- generate the OTA release keypair and sign firmware
images for MOTO-CTRL's OTA channel (docs/PROTOCOL.md §10, §10.4-10.5).

Produces the same shapes the firmware and app already speak:

  - A `.mcota` bundle: a small header (magic, format version, image_size,
    sha512, signature) followed by the raw image bytes. The header's three
    protocol fields map straight onto MC_OP_OTA_BEGIN's payload -- both
    firmware/sim/itest/moto-client.mjs's otaTransfer() and the app's
    MotoClient.uploadFirmware() parse this exact layout.
  - An optional update-manifest.json (docs/PROTOCOL.md §10.5), for the app's
    baked-in update-check URL.

Two subcommands:

  sign-firmware.py gen-key <path>
      Generates an Ed25519 keypair, once, for the project maintainer.
      Writes the 64-byte TweetNaCl-layout secret key (seed || public) to
      <path> (chmod 600) and the 32-byte public key to <path>.pub (hex).
      Also prints the public key as a C array to paste into
      firmware/components/core/mc_ota_release_key.c's
      MC_OTA_RELEASE_PUBKEY, replacing the placeholder there. Run this
      ONCE; store <path> outside this repo (see tools/README.md) -- never
      commit it, never put it in a hosted CI secrets store.

  sign-firmware.py sign --input <fw.bin> --key <path> --output <out.mcota>
                         [--manifest <manifest.json> --version X.Y.Z
                          --bundle-url URL [--changelog TEXT]]
      Signs a built moto_ctrl_firmware.bin and writes the .mcota bundle.
      With --manifest/--version/--bundle-url, also writes a manifest.json
      pointing at --bundle-url (wherever the maintainer uploads the
      resulting .mcota file, e.g. a GitHub Release asset).

Requires PyNaCl (`pip install pynacl`, Apache-2.0 licensed -- a dev-only
tooling dependency, never shipped in firmware or the app).
"""
import argparse
import hashlib
import json
import os
import stat
import sys

try:
    from nacl.signing import SigningKey
except ImportError:
    sys.exit("sign-firmware.py requires PyNaCl: pip install pynacl")

MCOTA_MAGIC = b"MCOT"
MCOTA_FORMAT_VERSION = 1
SHA512_BYTES = 64
SIGNATURE_BYTES = 64
SECRETKEY_BYTES = 64  # TweetNaCl/libsodium layout: 32-byte seed || 32-byte public key


def gen_key(path: str) -> None:
    if os.path.exists(path):
        sys.exit(f"refusing to overwrite existing key file: {path}")

    signing_key = SigningKey.generate()
    seed = bytes(signing_key)  # 32-byte seed
    public_key = bytes(signing_key.verify_key)  # 32 bytes
    secret_key = seed + public_key  # 64-byte TweetNaCl/mc_crypto layout

    with open(path, "wb") as f:
        f.write(secret_key)
    os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)  # 0600 -- private key, owner-only

    pub_path = path + ".pub"
    with open(pub_path, "w") as f:
        f.write(public_key.hex() + "\n")

    c_array = ",\n".join(
        "    " + ", ".join(f"0x{b:02x}" for b in public_key[i : i + 8])
        for i in range(0, len(public_key), 8)
    )
    print(f"Private key written to {path} (0600) -- store OUTSIDE this repo.")
    print(f"Public key (hex) written to {pub_path}")
    print()
    print("Paste this over MC_OTA_RELEASE_PUBKEY in")
    print("firmware/components/core/mc_ota_release_key.c:")
    print()
    print(f"const uint8_t MC_OTA_RELEASE_PUBKEY[MC_CRYPTO_PUBKEY_BYTES] = {{\n{c_array},\n}};")


def sign(args: argparse.Namespace) -> None:
    with open(args.key, "rb") as f:
        secret_key = f.read()
    if len(secret_key) != SECRETKEY_BYTES:
        sys.exit(f"{args.key}: expected a {SECRETKEY_BYTES}-byte key file (got {len(secret_key)})")
    seed = secret_key[:32]
    signing_key = SigningKey(seed)

    with open(args.input, "rb") as f:
        image = f.read()
    if len(image) == 0:
        sys.exit(f"{args.input}: empty file")

    digest = hashlib.sha512(image).digest()
    signature = signing_key.sign(digest).signature  # detached 64-byte Ed25519 signature

    header = bytearray()
    header += MCOTA_MAGIC
    header.append(MCOTA_FORMAT_VERSION)
    header += b"\x00\x00\x00"  # reserved
    header += len(image).to_bytes(4, "little")
    header += digest
    header += signature
    assert len(header) == 140, len(header)

    with open(args.output, "wb") as f:
        f.write(header)
        f.write(image)

    print(f"Wrote {args.output}: {len(image)}-byte image, sha512={digest.hex()[:16]}...")

    if args.manifest:
        if not args.version or not args.bundle_url:
            sys.exit("--manifest requires --version and --bundle-url")
        with open(args.output, "rb") as f:
            bundle_bytes = f.read()
        manifest = {
            "version": args.version,
            "changelog": args.changelog or "",
            "bundle_url": args.bundle_url,
            "bundle_sha512": hashlib.sha512(bundle_bytes).hexdigest(),
            "bundle_size": len(bundle_bytes),
        }
        with open(args.manifest, "w") as f:
            json.dump(manifest, f, indent=2)
            f.write("\n")
        print(f"Wrote {args.manifest} (upload {args.output} to {args.bundle_url})")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p_gen = sub.add_parser("gen-key", help="generate the OTA release keypair (run once)")
    p_gen.add_argument("path", help="output path for the private key file")

    p_sign = sub.add_parser("sign", help="sign a firmware image into a .mcota bundle")
    p_sign.add_argument("--input", required=True, help="path to moto_ctrl_firmware.bin")
    p_sign.add_argument("--key", required=True, help="path to the private key file (from gen-key)")
    p_sign.add_argument("--output", required=True, help="output .mcota path")
    p_sign.add_argument("--manifest", help="also write an update-manifest.json (docs/PROTOCOL.md §10.5)")
    p_sign.add_argument("--version", help="version string for the manifest, e.g. 1.2.0")
    p_sign.add_argument("--bundle-url", help="the URL the .mcota file will be uploaded to")
    p_sign.add_argument("--changelog", help="short changelog text for the manifest")

    args = parser.parse_args()
    if args.command == "gen-key":
        gen_key(args.path)
    elif args.command == "sign":
        sign(args)


if __name__ == "__main__":
    main()

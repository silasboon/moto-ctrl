#!/usr/bin/env python3
"""bump_version.py -- bumps the firmware version everywhere it's recorded.

Usage: bump_version.py X.Y.Z

The firmware version lives in exactly two places, and both have to move
together or the sim integration suite fails on the very next run:

  - firmware/components/core/include/mc_status.h's MC_FW_VERSION_MAJOR/
    MINOR/PATCH -- what actually ships in the STATUS wire reply
    (docs/PROTOCOL.md Sec5), read by both real firmware and firmware/sim.
  - firmware/sim/itest/integration.test.mjs's hardcoded st.fw assertion,
    which is that same wire value round-tripped and re-asserted.

Does NOT touch app/package.json's version -- the app and firmware are
versioned independently.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STATUS_H = ROOT / "firmware/components/core/include/mc_status.h"
ITEST = ROOT / "firmware/sim/itest/integration.test.mjs"


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit("Usage: bump_version.py X.Y.Z")
    version = sys.argv[1]
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", version)
    if not m:
        sys.exit(f"'{version}' is not a X.Y.Z version (three numeric parts)")
    major, minor, patch = m.groups()

    status_src = STATUS_H.read_text()
    new_status_src, n = re.subn(
        r"#define MC_FW_VERSION_MAJOR \d+\n"
        r"#define MC_FW_VERSION_MINOR \d+\n"
        r"#define MC_FW_VERSION_PATCH \d+",
        f"#define MC_FW_VERSION_MAJOR {major}\n"
        f"#define MC_FW_VERSION_MINOR {minor}\n"
        f"#define MC_FW_VERSION_PATCH {patch}",
        status_src,
    )
    if n != 1:
        sys.exit(f"Could not find the MC_FW_VERSION_* block in {STATUS_H}")
    STATUS_H.write_text(new_status_src)

    itest_src = ITEST.read_text()
    new_itest_src, n = re.subn(
        r"assert\.equal\(st\.fw, '\d+\.\d+\.\d+'\);",
        f"assert.equal(st.fw, '{version}');",
        itest_src,
    )
    if n != 1:
        sys.exit(f"Could not find the st.fw assertion in {ITEST}")
    ITEST.write_text(new_itest_src)

    print(f"Bumped firmware version to {version} in:")
    print(f"  {STATUS_H.relative_to(ROOT)}")
    print(f"  {ITEST.relative_to(ROOT)}")


if __name__ == "__main__":
    main()

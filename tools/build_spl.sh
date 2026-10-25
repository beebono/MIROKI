#!/usr/bin/env bash
#
# Build the cfw SPL (ext/boot/spl, its build.sh "cfw" variant: SPL straight
# to Linux from eMMC, with the EL3 monitor and no SML, TOS or u-boot) and
# copy the flashable image into build/cfw.
#
# The SPL tree builds into its own out/ (gitignored); read its README.md
# before touching chipram.
#
# Output: build/cfw/splloader.img. Flash it to the eMMC "splloader"
# partition with spd_dump. A bad SPL bricks the device short of BootROM
# recovery.
#
# Environment:
#   CROSS_COMPILE  default aarch64-linux-gnu-
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SPL_DIR="$REPO_ROOT/ext/boot/spl"
OUT_DIR="$REPO_ROOT/build/cfw"

[ -x "$SPL_DIR/build.sh" ] || { echo "ERROR: the SPL is not checked out at $SPL_DIR" >&2; exit 1; }

"$SPL_DIR/build.sh" cfw
install -D -m 0644 "$SPL_DIR/out/cfw/spl_a_cfw.img" "$OUT_DIR/splloader.img"
echo "Built: $OUT_DIR/splloader.img"
echo "Flash it to the eMMC \"splloader\" partition."

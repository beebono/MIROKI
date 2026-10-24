#!/usr/bin/env bash
#
# Assemble the initramfs tree that tools/build_cfw_boot.sh builds into the
# kernel Image: busybox from ext/system/busybox and this directory's /init,
# which mounts the squashfs rootfs and switch_roots into it. Firmware and
# modules live in the rootfs (tools/build_rootfs.sh).
#
# Output: $OUT_DIR/root (default build/initramfs/root). Nothing generated is
# written under system/initramfs.
#
# Environment:
#   BUSYBOX_DIR    busybox tree (default: ext/system/busybox)
#   CONFIG         busybox .config (default: system/initramfs/busybox.config)
#   OUT_DIR        output directory (default: build/initramfs)
#   CROSS_COMPILE  default aarch64-linux-gnu-
#   JOBS           parallel make jobs (default: nproc)
#
set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SRC_DIR/../.." && pwd)"
BUSYBOX_DIR="${BUSYBOX_DIR:-$REPO_ROOT/ext/system/busybox}"
CONFIG="${CONFIG:-$SRC_DIR/busybox.config}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/build/initramfs}"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
ROOT_DIR="$OUT_DIR/root"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"

[ -f "$BUSYBOX_DIR/Makefile" ] || { echo "ERROR: busybox not checked out at $BUSYBOX_DIR (run: git submodule update --init ext/system/busybox)" >&2; exit 1; }

# --- busybox from the submodule with the tracked config ---
cp "$CONFIG" "$BUSYBOX_DIR/.config"
make -C "$BUSYBOX_DIR" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" oldconfig
make -C "$BUSYBOX_DIR" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" -j"$JOBS"

# --- the tree ---
rm -rf "$ROOT_DIR"
mkdir -p "$ROOT_DIR"
# busybox 'install' lays down bin/, sbin/, usr/ and the applet symlinks
make -C "$BUSYBOX_DIR" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" CONFIG_PREFIX="$ROOT_DIR" install

# runtime mount points (init mounts proc/sys/devtmpfs onto these, and the
# rootfs onto newroot)
mkdir -p "$ROOT_DIR"/{dev,proc,sys,tmp,etc,newroot}
ln -sf bin/busybox "$ROOT_DIR/linuxrc"

cp "$SRC_DIR/init" "$ROOT_DIR/init"
chmod +x "$ROOT_DIR/init"

echo "Built: $ROOT_DIR"

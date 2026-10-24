#!/usr/bin/env bash
#
# Build the raw eMMC "boot" partition image for the cfw boot path: the cfw
# SPL (ext/boot/spl, `./build.sh cfw`) loads it straight from eMMC, with no
# SML/TOS/u-boot and no ramdisk loader, so the initramfs is built into the
# kernel Image.
#
# The initramfs comes from system/initramfs/build-initramfs.sh (busybox, the
# firmware, and the bring-up init: a shell on the USB serial gadget, no
# rootfs).
#
# Output: build/cfw/boot.img. Flash it to the eMMC "boot" partition with
# spd_dump; the partition layout is tools/cfw-emmc-partitions.xml.
#
# Environment:
#   KERNEL_DIR  kernel tree (default: ext/boot/linux)
#   KCONFIG     kernel config (default: tools/cfw-kernel.config)
#   JOBS        parallel make jobs (default: nproc)
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL_DIR="${KERNEL_DIR:-$REPO_ROOT/ext/boot/linux}"
KCONFIG="${KCONFIG:-$REPO_ROOT/tools/cfw-kernel.config}"
JOBS="${JOBS:-$(nproc)}"
OUT_DIR="$REPO_ROOT/build/cfw"
INITRAMFS_DIR="$REPO_ROOT/system/initramfs"
DTB_NAME="ums512-rg-rotate-cfw"

export ARCH=arm64
export CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
export JOBS

# --- initramfs tree ---
OUT_DIR="$OUT_DIR/initramfs" "$INITRAMFS_DIR/build-initramfs.sh"
ROOT_DIR="$OUT_DIR/initramfs/root"

# --- kernel with the tree built in ---
cd "$KERNEL_DIR"
cp "$KCONFIG" .config

# The directory plus a gen_init_cpio list for the device nodes /init needs
# before devtmpfs is up (a non-root build cannot mknod into the directory).
scripts/config --set-str INITRAMFS_SOURCE "$ROOT_DIR $INITRAMFS_DIR/devnodes.list"
# No modules in this initramfs: build in the audio DSP boot driver so it
# finds agdsp.bin in the initramfs's /lib/firmware.
scripts/config --enable SPRD_AUDCP_BOOT
make olddefconfig

make -j"$JOBS" Image
make -j"$JOBS" "sprd/$DTB_NAME.dtb"
cd "$REPO_ROOT"

# --- pack the raw boot partition ---
python3 "$REPO_ROOT/ext/boot/spl/scripts/mk_cfw_boot.py" \
	"$KERNEL_DIR/arch/arm64/boot/Image" \
	"$KERNEL_DIR/arch/arm64/boot/dts/sprd/$DTB_NAME.dtb" \
	"$OUT_DIR/boot.img"

echo "Built: $OUT_DIR/boot.img"
echo "Flash it to the eMMC \"boot\" partition, with the cfw SPL in splloader."

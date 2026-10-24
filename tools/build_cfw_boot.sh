#!/usr/bin/env bash
#
# Build the raw eMMC "boot" partition image for the cfw boot path: the cfw
# SPL (ext/boot/spl, `./build.sh cfw`) loads it straight from eMMC, with no
# SML/TOS/u-boot and no ramdisk loader, so the initramfs is built into the
# kernel Image.
#
# The initramfs comes from system/initramfs/build-initramfs.sh (busybox and
# an init that switch_roots into the squashfs on the "rootfs" partition). The
# partition also carries the EL3 monitor (ext/boot/spl/monitor), which gives
# Linux PSCI.
#
# The kernel's modules and the out-of-tree mali_kbase (ext/system/mali-kbase)
# are built from the same tree and staged, with depmod run, for
# tools/build_rootfs.sh to put in the rootfs.
#
# Output: build/cfw/boot.img, and the module tree in build/cfw/modules. Flash
# boot.img to the eMMC "boot" partition with spd_dump; the partition layout is
# tools/cfw-emmc-partitions.xml.
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
KBASE_SRC="$REPO_ROOT/ext/system/mali-kbase/product/kernel"
MOD_DIR="$OUT_DIR/modules"
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
make olddefconfig

make -j"$JOBS" Image modules
make -j"$JOBS" "sprd/$DTB_NAME.dtb"
KREL="$(make -s kernelrelease)"

# --- modules, staged for the rootfs ---
rm -rf "$MOD_DIR"
make INSTALL_MOD_PATH="$MOD_DIR" INSTALL_MOD_STRIP=1 modules_install
rm -f "$MOD_DIR/lib/modules/$KREL/build"
cd "$REPO_ROOT"

# mali_kbase, the kernel half of the libmali blobs. Built from a copy so the
# submodule stays clean; options as ROCKNIX's T618 (devicetree platform, real
# hardware, devfreq).
mkdir -p "$OUT_DIR/mali-kbase"
rsync -a "$KBASE_SRC/" "$OUT_DIR/mali-kbase/"
KBASE_BUILD="$OUT_DIR/mali-kbase/drivers/gpu/arm/midgard"
make -C "$KBASE_BUILD" -j"$JOBS" KDIR="$KERNEL_DIR" \
	CONFIG_MALI_MIDGARD=m CONFIG_MALI_PLATFORM_NAME=devicetree \
	CONFIG_MALI_REAL_HW=y CONFIG_MALI_DEVFREQ=y CONFIG_MALI_GATOR_SUPPORT=y
install -D -m 0644 "$KBASE_BUILD/mali_kbase.ko" \
	"$MOD_DIR/lib/modules/$KREL/extra/mali_kbase.ko"
"${CROSS_COMPILE}strip" --strip-debug "$MOD_DIR/lib/modules/$KREL/extra/mali_kbase.ko"
depmod -b "$MOD_DIR" "$KREL"

# --- EL3 monitor ---
make -C "$REPO_ROOT/ext/boot/spl/monitor" O="$OUT_DIR/monitor" \
	CROSS_COMPILE="$CROSS_COMPILE"

# --- pack the raw boot partition ---
python3 "$REPO_ROOT/ext/boot/spl/scripts/mk_cfw_boot.py" \
	"$KERNEL_DIR/arch/arm64/boot/Image" \
	"$KERNEL_DIR/arch/arm64/boot/dts/sprd/$DTB_NAME.dtb" \
	"$OUT_DIR/boot.img" \
	"$OUT_DIR/monitor/monitor.bin"

echo "Built: $OUT_DIR/boot.img (modules for $KREL in $MOD_DIR)"
echo "Flash it to the eMMC \"boot\" partition, with the cfw SPL in splloader."

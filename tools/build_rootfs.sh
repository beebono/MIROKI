#!/usr/bin/env bash
#
# Build the squashfs for the eMMC "rootfs" partition: busybox (the same
# static build and config as the initramfs), system/rootfs (inittab, rcS),
# the kernel modules tools/build_cfw_boot.sh staged in build/cfw/modules, the
# firmware, and exfatprogs for the data partition.
#
# Run tools/build_cfw_boot.sh first: the modules must come from the kernel
# build that is flashed.
#
# Output: build/cfw/rootfs.squashfs. Flash it to the eMMC "rootfs" partition
# with spd_dump.
#
# Environment:
#   CROSS_COMPILE  default aarch64-linux-gnu-
#   JOBS           parallel make jobs (default: nproc)
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$REPO_ROOT/build/cfw"
ROOT_DIR="$OUT_DIR/rootfs"
MOD_DIR="$OUT_DIR/modules"
BUSYBOX_DIR="$REPO_ROOT/ext/system/busybox"
BUSYBOX_CONFIG="$REPO_ROOT/system/initramfs/busybox.config"
EXFAT_SRC="$REPO_ROOT/ext/system/exfatprogs"
EXFAT_BUILD="$OUT_DIR/exfatprogs"
FIRMWARE_DIR="$REPO_ROOT/system/prebuilts/firmware"
SKEL_DIR="$REPO_ROOT/system/rootfs"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"

[ -d "$MOD_DIR/lib/modules" ] || { echo "ERROR: no staged modules in $MOD_DIR (run tools/build_cfw_boot.sh)" >&2; exit 1; }
[ -f "$EXFAT_SRC/configure.ac" ] || { echo "ERROR: exfatprogs not checked out at $EXFAT_SRC" >&2; exit 1; }

# --- exfatprogs, static, built from a copy (autogen writes into the tree).
# libtool takes plain -static to mean "no shared libtool libs" and still links
# glibc dynamically; -all-static is libtool-only, so it goes to make. ---
mkdir -p "$EXFAT_BUILD"
rsync -a --exclude=.git "$EXFAT_SRC/" "$EXFAT_BUILD/"
if [ ! -f "$EXFAT_BUILD/Makefile" ]; then
	(cd "$EXFAT_BUILD" && ./autogen.sh && \
	 ./configure --host="${CROSS_COMPILE%-}" --prefix=/usr --disable-shared)
fi
make -C "$EXFAT_BUILD" -j"$JOBS" LDFLAGS=-all-static

# --- busybox ---
cp "$BUSYBOX_CONFIG" "$BUSYBOX_DIR/.config"
make -C "$BUSYBOX_DIR" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" oldconfig
make -C "$BUSYBOX_DIR" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" -j"$JOBS"

# --- the tree ---
rm -rf "$ROOT_DIR"
mkdir -p "$ROOT_DIR"
make -C "$BUSYBOX_DIR" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" CONFIG_PREFIX="$ROOT_DIR" install
rm -f "$ROOT_DIR/linuxrc"

# mount points: the initramfs moves dev/proc/sys over, rcS mounts the rest
mkdir -p "$ROOT_DIR"/{dev,proc,sys,run,tmp,data,etc,lib/firmware}

cp -a "$SKEL_DIR/." "$ROOT_DIR/"
chmod +x "$ROOT_DIR/etc/init.d/rcS"
cp -a "$MOD_DIR/lib/modules" "$ROOT_DIR/lib/"
cp -a "$FIRMWARE_DIR/." "$ROOT_DIR/lib/firmware/"
for t in mkfs.exfat fsck.exfat; do
	d="${t%%.*}"
	install -m 0755 "$EXFAT_BUILD/$d/$t" "$ROOT_DIR/usr/sbin/$t"
	"${CROSS_COMPILE}strip" "$ROOT_DIR/usr/sbin/$t"
done

# --- squashfs ---
mksquashfs "$ROOT_DIR" "$OUT_DIR/rootfs.squashfs" -noappend -all-root -comp zstd -quiet
echo "Built: $OUT_DIR/rootfs.squashfs ($(stat -c%s "$OUT_DIR/rootfs.squashfs") bytes)"
echo "Flash it to the eMMC \"rootfs\" partition."

#!/usr/bin/env bash
#
# Build the squashfs for the eMMC "rootfs" partition: busybox (the same
# static build and config as the initramfs), system/rootfs (inittab, rcS),
# the kernel modules tools/build_cfw_boot.sh staged in build/cfw/modules, the
# firmware, exfatprogs for the data partition, the libmali GPU stack,
# libudev-zero, the input aggregator (system/aggregator), and RetroArch
# (tools/build_retroarch.sh) with the shared libraries it needs.
#
# glibc and the C++ runtime come from the cross toolchain everything is
# linked with; any other library is pulled from the host's arm64 multiarch
# packages by walking DT_NEEDED, after the libmali shims are in place so
# their sonames (libEGL.so.1, libgbm.so.1, ...) never resolve to mesa.
#
# Run tools/build_cfw_boot.sh and tools/build_retroarch.sh first: the
# modules must come from the kernel build that is flashed.
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
UDEV0_SRC="$REPO_ROOT/ext/system/libudev-zero"
UDEV0_BUILD="$OUT_DIR/libudev-zero"
AGG_DIR="$REPO_ROOT/system/aggregator"
FIRMWARE_DIR="$REPO_ROOT/system/prebuilts/firmware"
SKEL_DIR="$REPO_ROOT/system/rootfs"
BLOBS_DIR="$REPO_ROOT/system/prebuilts/libmali-blobs"
RETROARCH="$OUT_DIR/retroarch/retroarch"
TOOLCHAIN_LIB="/usr/aarch64-linux-gnu/lib"
MULTIARCH_LIB="/usr/lib/aarch64-linux-gnu"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"

[ -d "$MOD_DIR/lib/modules" ] || { echo "ERROR: no staged modules in $MOD_DIR (run tools/build_cfw_boot.sh)" >&2; exit 1; }
[ -f "$EXFAT_SRC/configure.ac" ] || { echo "ERROR: exfatprogs not checked out at $EXFAT_SRC" >&2; exit 1; }
[ -f "$UDEV0_SRC/udev.c" ] || { echo "ERROR: libudev-zero not checked out at $UDEV0_SRC" >&2; exit 1; }
[ -x "$RETROARCH" ] || { echo "ERROR: no $RETROARCH (run tools/build_retroarch.sh)" >&2; exit 1; }
[ -d "$MULTIARCH_LIB" ] || { echo "ERROR: no arm64 multiarch libraries in $MULTIARCH_LIB" >&2; exit 1; }

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

# --- libudev-zero, built from a copy (its Makefile builds in the tree) ---
mkdir -p "$UDEV0_BUILD"
rsync -a --exclude=.git "$UDEV0_SRC/" "$UDEV0_BUILD/"
# The version script gives the LIBUDEV_183 symbol versions programs linked
# against systemd's libudev (RetroArch, here) reference, as its meson build does.
make -C "$UDEV0_BUILD" -j"$JOBS" CC="${CROSS_COMPILE}gcc" CFLAGS=-O2 \
	LDFLAGS="-Wl,--version-script=libudev.sym" libudev.so.1

# --- input aggregator, tmix for the audio routing, tone for testing it ---
make -C "$AGG_DIR" O="$OUT_DIR/aggregator" CROSS_COMPILE="$CROSS_COMPILE"
make -C "$REPO_ROOT/system/tmix" O="$OUT_DIR/tmix" CROSS_COMPILE="$CROSS_COMPILE"
make -C "$REPO_ROOT/system/tone" O="$OUT_DIR/tone" CROSS_COMPILE="$CROSS_COMPILE"

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

# --- glibc: the dynamic linker sits where PT_INTERP points ---
mkdir -p "$ROOT_DIR/usr/lib"
cp -L "$TOOLCHAIN_LIB/ld-linux-aarch64.so.1" "$ROOT_DIR/lib/"

# --- libmali: GLES/EGL/GBM wrapper shims at the canonical sonames, the
# blob they dispatch into, and Vulkan (loader, ICD, WSI layer) ---
cp -a "$BLOBS_DIR/lib"/libmali*.so* "$BLOBS_DIR/lib"/libMaliVulkan.so* \
	"$BLOBS_DIR/lib"/libvulkan.so* "$BLOBS_DIR/lib"/libVkLayer_window_system_integration.so \
	"$ROOT_DIR/usr/lib/"
cp -a "$BLOBS_DIR/lib/mali/." "$ROOT_DIR/usr/lib/"
install -D -m 0644 "$BLOBS_DIR/icd.d/mali.json" "$ROOT_DIR/usr/share/vulkan/icd.d/mali.json"
install -D -m 0644 "$BLOBS_DIR/implicit_layer.d/VkLayer_window_system_integration.json" \
	"$ROOT_DIR/usr/share/vulkan/implicit_layer.d/VkLayer_window_system_integration.json"

# --- ALSA's config tree (alsa-lib needs alsa.conf); UCM stays out until the
# board's own profile goes in ---
mkdir -p "$ROOT_DIR/usr/share/alsa"
rsync -a --exclude=ucm --exclude=ucm2 /usr/share/alsa/ "$ROOT_DIR/usr/share/alsa/"

# --- input: libudev-zero stands in for systemd's libudev (no udevd here:
# it derives ID_INPUT_* from each device's evdev capabilities), and the
# aggregator that merges the gamepad and vibrator into one pad ---
install -m 0755 "$UDEV0_BUILD/libudev.so.1" "$ROOT_DIR/usr/lib/libudev.so.1"
"${CROSS_COMPILE}strip" --strip-unneeded "$ROOT_DIR/usr/lib/libudev.so.1"
install -m 0755 "$OUT_DIR/aggregator/miroki-inputd" "$ROOT_DIR/usr/bin/miroki-inputd"
install -m 0755 "$OUT_DIR/tmix/tmix" "$ROOT_DIR/usr/bin/tmix"
install -m 0755 "$OUT_DIR/tone/tone" "$ROOT_DIR/usr/bin/tone"

# --- RetroArch ---
install -m 0755 "$RETROARCH" "$ROOT_DIR/usr/bin/retroarch"

# --- shared library closure: copy every missing DT_NEEDED, toolchain first,
# then multiarch, until nothing new turns up ---
while :; do
	added=0
	needed=$(find "$ROOT_DIR" -path "$ROOT_DIR/lib/modules" -prune -o \
		     -path "$ROOT_DIR/lib/firmware" -prune -o -type f -print |
		 while read -r f; do "${CROSS_COMPILE}readelf" -d "$f" 2>/dev/null; done |
		 sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort -u)
	for lib in $needed; do
		[ -e "$ROOT_DIR/usr/lib/$lib" ] || [ -e "$ROOT_DIR/lib/$lib" ] && continue
		for dir in "$TOOLCHAIN_LIB" "$MULTIARCH_LIB"; do
			if [ -e "$dir/$lib" ]; then
				cp -L "$dir/$lib" "$ROOT_DIR/usr/lib/"
				echo "  + $lib (from $dir)"
				added=1
				break
			fi
		done
		[ -e "$ROOT_DIR/usr/lib/$lib" ] || { echo "ERROR: cannot resolve $lib" >&2; exit 1; }
	done
	[ $added -eq 1 ] || break
done

# --- squashfs ---
mksquashfs "$ROOT_DIR" "$OUT_DIR/rootfs.squashfs" -noappend -all-root -comp zstd -quiet
echo "Built: $OUT_DIR/rootfs.squashfs ($(stat -c%s "$OUT_DIR/rootfs.squashfs") bytes)"
echo "Flash it to the eMMC \"rootfs\" partition."

#!/usr/bin/env bash
#
# Build RetroArch (ext/retroarch/launcher) for the rootfs: KMS + EGL with
# GLES 3.2 on the libmali blobs, ALSA, udev/evdev input. No X11, Wayland, Qt,
# SDL, Vulkan or networking.
#
# It builds inside the submodule (RetroArch's .gitignore covers config.h,
# config.mk and obj-unix/) against the host's arm64 multiarch libraries
# (Ubuntu's libasound2-dev:arm64, libdrm-dev:arm64, ...). It links against
# their libEGL/libGLESv2/libgbm, whose sonames the libmali wrapper shims
# provide on the device.
#
# Output: build/cfw/retroarch/retroarch, which tools/build_rootfs.sh installs.
#
# Environment:
#   CROSS_COMPILE  default aarch64-linux-gnu-
#   JOBS           parallel make jobs (default: nproc)
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RA_DIR="$REPO_ROOT/ext/retroarch/launcher"
OUT_DIR="$REPO_ROOT/build/cfw/retroarch"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"

[ -f "$RA_DIR/configure" ] || { echo "ERROR: RetroArch not checked out at $RA_DIR" >&2; exit 1; }
command -v "${CROSS_COMPILE}pkg-config" >/dev/null || { echo "ERROR: ${CROSS_COMPILE}pkg-config not found" >&2; exit 1; }

cd "$RA_DIR"
CFLAGS="-O2 -mcpu=cortex-a75.cortex-a55" CXXFLAGS="-O2 -mcpu=cortex-a75.cortex-a55" \
./configure --host="${CROSS_COMPILE%-}" --prefix=/usr \
	--enable-kms --enable-egl \
	--enable-opengles --enable-opengles3 --enable-opengles3_1 --enable-opengles3_2 \
	--disable-opengl1 --disable-vulkan \
	--enable-alsa --enable-udev --enable-threads \
	--disable-x11 --disable-wayland --disable-qt --disable-sdl --disable-sdl2 \
	--disable-pulse --disable-pipewire --disable-jack --disable-oss \
	--disable-roar --disable-rsound --disable-tinyalsa \
	--disable-ffmpeg --disable-v4l2 --disable-libusb --disable-systemd \
	--disable-freetype --disable-cdrom --disable-crtswitchres --disable-xdelta \
	--disable-slang --disable-glslang --disable-spirv_cross \
	--disable-networking --disable-discord --disable-cheevos \
	--disable-online_updater --disable-update_cores \
	--disable-update_core_info --disable-update_assets \
	--disable-materialui --disable-xmb
make -j"$JOBS"

mkdir -p "$OUT_DIR"
install -m 0755 retroarch "$OUT_DIR/retroarch"
"${CROSS_COMPILE}strip" "$OUT_DIR/retroarch"
echo "Built: $OUT_DIR/retroarch"

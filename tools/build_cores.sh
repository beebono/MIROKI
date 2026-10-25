#!/usr/bin/env bash
#
# Build the libretro cores for the rootfs: mupen64pow2 (N64), pcsx-rearmed
# (PS1) and yabasanshiro-pirry (Saturn), all in ext/retroarch/. Each builds
# in its own tree with its libretro Makefile (their .gitignores cover the
# objects and the .so), cross-compiled with the aarch64 toolchain.
#
# The CPU flags ride along in CC/CXX: the Makefiles assign CFLAGS with +=,
# which a command-line CFLAGS= would clobber.
#
# Output: build/cfw/cores/*_libretro.so, stripped, which tools/build_rootfs.sh
# installs to /usr/lib/libretro.
#
# Environment:
#   CROSS_COMPILE  default aarch64-linux-gnu-
#   JOBS           parallel make jobs (default: nproc)
#   CORES          space separated subset to build (default: all three)
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RA_DIR="$REPO_ROOT/ext/retroarch"
OUT_DIR="$REPO_ROOT/build/cfw/cores"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"
CORES="${CORES:-mupen64pow2 pcsx-rearmed yabasanshiro-pirry}"

CPUFLAGS="-mcpu=cortex-a75.cortex-a55"
CC="${CROSS_COMPILE}gcc $CPUFLAGS"
CXX="${CROSS_COMPILE}g++ $CPUFLAGS"
STRIP="${CROSS_COMPILE}strip"

command -v "${CROSS_COMPILE}gcc" >/dev/null || { echo "ERROR: ${CROSS_COMPILE}gcc not found" >&2; exit 1; }
mkdir -p "$OUT_DIR"

install_core() {
	[ -f "$1" ] || { echo "ERROR: $1 was not built" >&2; exit 1; }
	install -m 0644 "$1" "$OUT_DIR/$(basename "$1")"
	"$STRIP" --strip-unneeded "$OUT_DIR/$(basename "$1")"
	echo "Built: $OUT_DIR/$(basename "$1")"
}

for core in $CORES; do
	echo "=== $core"
	case "$core" in
	mupen64pow2)
		# Software RDP (foir) and arMLEt only; there is no GL in this fork, so
		# GL_LIB is never linked whatever platform=unix picks.
		make -C "$RA_DIR/mupen64pow2" -j"$JOBS" \
			platform=unix ARCH=aarch64 CC="$CC" CXX="$CXX" CC_AS="$CC"
		install_core "$RA_DIR/mupen64pow2/mupen64plus_pow2_libretro.so"
		;;
	pcsx-rearmed)
		# ARCH_DETECTED comes from `$(CC) -dumpmachine`, so aarch64 picks the
		# ari64 dynarec and the NEON GPU by itself. Nested submodules
		# (libchdr, lightrec, libpicofe, ...) must be checked out.
		[ -f "$RA_DIR/pcsx-rearmed/deps/lightrec/lightrec.h" ] ||
			{ echo "ERROR: run 'git submodule update --init --recursive' in $RA_DIR/pcsx-rearmed" >&2; exit 1; }
		make -C "$RA_DIR/pcsx-rearmed" -f Makefile.libretro -j"$JOBS" \
			platform=unix CC="$CC" CXX="$CXX" CC_AS="$CC"
		install_core "$RA_DIR/pcsx-rearmed/pcsx_rearmed_libretro.so"
		;;
	yabasanshiro-pirry)
		# platform=arm64 picks the aarch64 SH-2 dynarec; FORCE_GLES renders
		# through the GLES 3.1 context the core asks RetroArch for. libchdr
		# is bundled. The generated Musashi sources are checked in.
		make -C "$RA_DIR/yabasanshiro-pirry/yabause/src/libretro" -j"$JOBS" \
			platform=arm64 FORCE_GLES=1 CC="$CC" CXX="$CXX" CC_AS="$CC"
		install_core "$RA_DIR/yabasanshiro-pirry/yabause/src/libretro/yabasanshiro_libretro.so"
		;;
	*)
		echo "ERROR: unknown core '$core'" >&2; exit 1
		;;
	esac
done

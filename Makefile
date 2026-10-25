# MIROKI build. Each target runs its script in tools/; everything lands in
# build/cfw. `make` builds all of it, in order.
#
# Pass JOBS=n to limit parallelism, CROSS_COMPILE=... for another toolchain.
# The scripts each run make with -j themselves, so this file never runs two
# of them at once.

export JOBS CROSS_COMPILE

.NOTPARALLEL:
.PHONY: all spl boot retroarch cores rootfs clean distclean help

all: spl boot retroarch cores rootfs

# the SPL that boots Linux straight from eMMC, for the "splloader" partition
spl:
	tools/build_spl.sh

# kernel with its initramfs, the dtb and the EL3 monitor, for "boot"; also
# stages the kernel modules for the rootfs
boot:
	tools/build_cfw_boot.sh

retroarch:
	tools/build_retroarch.sh

cores:
	tools/build_cores.sh

# squashfs for "rootfs": needs boot (its modules), retroarch and cores built
rootfs:
	tools/build_rootfs.sh

# build/ holds every output and every out-of-tree build
clean:
	rm -rf build

# also the builds that happen inside the submodules (their .gitignores keep
# the trees clean, but the objects take space and can go stale)
distclean: clean
	-$(MAKE) -C ext/boot/linux ARCH=arm64 mrproper
	-$(MAKE) -C ext/system/busybox distclean
	-$(MAKE) -C ext/retroarch/launcher clean
	-$(MAKE) -C ext/retroarch/mupen64pow2 platform=unix clean
	-$(MAKE) -C ext/retroarch/pcsx-rearmed -f Makefile.libretro platform=unix clean
	-$(MAKE) -C ext/retroarch/yabasanshiro-pirry/yabause/src/libretro platform=arm64 clean
	rm -rf ext/boot/spl/out

help:
	@echo "make            build everything into build/cfw"
	@echo "make spl        build/cfw/splloader.img     (eMMC splloader)"
	@echo "make boot       build/cfw/boot.img          (eMMC boot)"
	@echo "make retroarch  build/cfw/retroarch/"
	@echo "make cores      build/cfw/cores/"
	@echo "make rootfs     build/cfw/rootfs.squashfs   (eMMC rootfs)"
	@echo "make clean      remove build/"
	@echo "make distclean  also clean the builds inside the submodules"

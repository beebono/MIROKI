# tmix, the ALSA control tool rcS sets the audio routing with.
# tools/build_rootfs.sh builds it into the rootfs:
#   make -C system/tmix O=<out dir>
CROSS_COMPILE ?= aarch64-linux-gnu-
O ?= .

CC := $(CROSS_COMPILE)gcc
STRIP := $(CROSS_COMPILE)strip
CFLAGS := -std=gnu11 -O2 -Wall -Wextra

$(O)/tmix: tmix.c
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -o $@ $<
	$(STRIP) --strip-unneeded $@

clean:
	rm -f $(O)/tmix

.PHONY: clean

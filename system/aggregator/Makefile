# MIROKI input aggregator. tools/build_rootfs.sh builds it into the rootfs:
#   make -C system/aggregator O=<out dir>
CROSS_COMPILE ?= aarch64-linux-gnu-
O ?= .

CC := $(CROSS_COMPILE)gcc
STRIP := $(CROSS_COMPILE)strip
CFLAGS := -std=c11 -O2 -Wall -Wextra -Wno-unused-result

$(O)/miroki-inputd: main.c
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -o $@ $<
	$(STRIP) --strip-unneeded $@

clean:
	rm -f $(O)/miroki-inputd

.PHONY: clean

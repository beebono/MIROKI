# tone, a test sine through ALSA's default PCM. Links the host's arm64
# multiarch libasound. tools/build_rootfs.sh builds it into the rootfs:
#   make -C system/tone O=<out dir>
CROSS_COMPILE ?= aarch64-linux-gnu-
O ?= .

CC := $(CROSS_COMPILE)gcc
STRIP := $(CROSS_COMPILE)strip
CFLAGS := -std=gnu11 -O2 -Wall -Wextra

$(O)/tone: tone.c
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -o $@ $< -lasound -lm
	$(STRIP) --strip-unneeded $@

clean:
	rm -f $(O)/tone

.PHONY: clean

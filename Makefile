PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

VERSION_TAG := $(shell git describe --abbrev=6 --dirty --always --tags 2>/dev/null || echo unknown)

# Standard Flags (No extra libraries)
CFLAGS := -O2 -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Werror=strict-prototypes -Werror=missing-prototypes -D_BSD_SOURCE -std=gnu11 -Iinclude -Isrc
CFLAGS += -DSHADOWMOUNT_VERSION=\"$(VERSION_TAG)\"

# Linker
LDFLAGS :=

# Standard Libraries Only
LIBS := -lkernel_sys -lSceNotification -lSceSystemService -lSceUserService -lSceAppInstUtil -lsqlite3

ASSET_SRCS := src/notify_icon_asset.c
SRCS := src/main.c $(wildcard src/sm_*.c) $(ASSET_SRCS)
OBJS := $(SRCS:.c=.o)
HEADERS := $(wildcard include/*.h)

# Targets
all: shadowmountplus.elf

# Build Daemon
shadowmountplus.elf: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	$(PS5_PAYLOAD_SDK)/bin/prospero-strip --strip-debug $@
	rm -f src/notify_icon_asset.c

src/notify_icon_asset.c: smp_icon.png
	xxd -i $< > $@

src/%.o: src/%.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f shadowmountplus.elf kill.elf src/*.o src/notify_icon_asset.c

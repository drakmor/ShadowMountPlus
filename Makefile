PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

VERSION_TAG := $(shell git describe --abbrev=6 --dirty --always --tags 2>/dev/null || echo unknown)
BUILD_TIME := $(shell date -u +%Y-%m-%dT%H:%M:%SZ)

# Compiler and dependency flags
HOMEBREW_ROOT := $(PS5_PAYLOAD_SDK)/target/user/homebrew
JSON_CFLAGS := -I$(HOMEBREW_ROOT)/include
MHD_LIB := $(HOMEBREW_ROOT)/lib/libmicrohttpd.a
CFLAGS := -O3 -flto=thin -DNDEBUG -ffunction-sections -fdata-sections -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Werror=strict-prototypes -Werror=missing-prototypes -D_BSD_SOURCE -std=gnu11 -Iinclude -Isrc
CFLAGS += -DSHADOWMOUNT_VERSION=\"$(VERSION_TAG)\"

# Linker
LDFLAGS := -flto=thin -Wl,--gc-sections

# Libraries
LIBS := -lSceNotification -lSceSystemService -lSceUserService -lSceAppInstUtil -lSceNet -lSceSsl -lSceHttp2 -lsqlite3 $(HOMEBREW_ROOT)/lib/libjson-c.a $(MHD_LIB) -lpthread -lm
PS5_SCE_STUBS_DIR ?= $(PS5_PAYLOAD_SDK)/src/sce_stubs
KERNEL_SYS_STUB_SO := src/libkernel_sys_ext.so
KERNEL_SYS_STUB_SRCS := $(PS5_SCE_STUBS_DIR)/libkernel_sys.c src/libkernel_sys_ext.c

ASSET_SRCS := src/notify_icon_asset.c src/config_ini_example_asset.c
SRCS := src/main.c $(wildcard src/sm_*.c) $(ASSET_SRCS)
ASM_SRCS := src/sm_shellcore_bridge.S
OBJS := $(SRCS:.c=.o) $(ASM_SRCS:.S=.o)
HEADERS := $(wildcard include/*.h)
L10N_CATALOGS := $(wildcard include/lang/*.inc)

# Targets
all: shadowmountplus.elf

usb-info.elf: tools/usb_info.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lSceNotification
	$(PS5_PAYLOAD_SDK)/bin/prospero-strip --strip-all $@

# Build Daemon
shadowmountplus.elf: $(OBJS) $(KERNEL_SYS_STUB_SO)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(KERNEL_SYS_STUB_SO) $(LIBS)
	$(PS5_PAYLOAD_SDK)/bin/prospero-strip --strip-all $@
	rm -f src/notify_icon_asset.c src/config_ini_example_asset.c

$(KERNEL_SYS_STUB_SO): $(KERNEL_SYS_STUB_SRCS)
	test -f "$(PS5_SCE_STUBS_DIR)/libkernel_sys.c"
	$(CC) -shared -Wl,-soname=libkernel_sys.sprx -o $@ $^

src/notify_icon_asset.c: smp_icon.png
	xxd -i $< > $@

src/config_ini_example_asset.c: config.ini.example
	xxd -i $< > $@

src/sm_api_service.o: CFLAGS += $(JSON_CFLAGS)
src/main.o src/sm_image_index.o: CFLAGS += -DSHADOWMOUNT_BUILD_TIME=\"$(BUILD_TIME)\"
src/main.o src/sm_image_index.o: FORCE

src/sm_l10n.o: $(L10N_CATALOGS)
src/sm_ampr_updater.o: src/sm_ampr_ca.inc

.PHONY: FORCE
FORCE:

src/%.o: src/%.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

src/%.o: src/%.S
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f shadowmountplus.elf usb-info.elf api-test.elf kill.elf src/*.o $(KERNEL_SYS_STUB_SO) src/notify_icon_asset.c src/config_ini_example_asset.c

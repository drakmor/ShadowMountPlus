PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

# Standard Flags (No extra libraries)
CFLAGS := -O2 -Wall -D_BSD_SOURCE -std=gnu11 -Isrc -I$(INCDIR)

# Linker
LDFLAGS := -L$(LIBdir)

# Standard Libraries Only
LIBS := -lkernel_sys -lSceSystemService -lSceUserService -lSceAppInstUtil

# Targets
all: shadowmountplus.elf

# Build Daemon
shadowmountplus.elf: src/main.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f shadowmountplus.elf kill.elf src/*.o
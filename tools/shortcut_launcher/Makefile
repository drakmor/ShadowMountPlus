# Builds the shortcut launcher (assets/shortcut_launcher.bin) with
# BlackBearReloaded's ps5-native-app-boilerplate: its ps5-native-tool, payload
# SDK, linker script and clean-room libc.prx loader shim, which the app loader
# needs in sce_module/; it goes in gzipped (assets/shortcut_libc.prx.gz). NATIVE
# names a checkout whose tools are built. Both files are embedded in
# shadowmountplus.elf, which puts them into shortcut folders.
NATIVE ?= $(error set NATIVE to a built ps5-native-app-boilerplate checkout)
SDK := $(NATIVE)/.deps/native/ps5-payload-sdk
NATIVE_TOOL := $(NATIVE)/build/host/ps5-native-tool
CC := clang -target x86_64-sie-ps5 -fvisibility-nodllstorageclass=default \
	-isysroot $(SDK) -isystem $(SDK)/target/include \
	-fno-stack-protector -fno-plt -femulated-tls -ffunction-sections -fdata-sections -O2 -Wall -Wextra
STUBS := build/stubs
OUT := ../../assets/shortcut_launcher.bin
LIBC_OUT := ../../assets/shortcut_libc.prx.gz

all: $(OUT) $(LIBC_OUT)

$(STUBS)/.sdk:
	mkdir -p $(STUBS)
	cp $(SDK)/target/lib/*.so $(STUBS)/
	touch $@

build/llvm-pie.elf: launcher.c crt0.c $(STUBS)/.sdk
	$(CC) -c launcher.c -o build/launcher.o
	$(CC) -c crt0.c -o build/crt0.o
	$(SDK)/bin/prospero-lld -T $(NATIVE)/tooling/native/ps5-pie.ld --eh-frame-hdr --gc-sections \
		-e _start -o $@ build/crt0.o build/launcher.o --as-needed $(STUBS)/*.so

build/eboot.elf: build/llvm-pie.elf
	$(NATIVE_TOOL) link --in $< --out $@ --stub-dir $(STUBS) \
		--module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf

$(OUT): build/eboot.elf
	$(NATIVE_TOOL) self --sign --in $< --out $@ --magic 0x1D3D154F

$(LIBC_OUT): $(NATIVE)/runtime/libc.prx
	gzip -9 -n -c $< > $@

clean:
	rm -rf build

.PHONY: all clean

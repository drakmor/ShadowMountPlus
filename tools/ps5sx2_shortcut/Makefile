# Builds the PS5SX2 shortcut launcher (eboot.bin) with BlackBearReloaded's
# ps5-native-app-boilerplate: its ps5-native-tool, payload SDK, linker script
# and clean-room libc.prx. NATIVE names a checkout whose tools are built
# (tools/fetch-native-tools.sh or its own build script).
NATIVE ?= $(error set NATIVE to a built ps5-native-app-boilerplate checkout)
SDK := $(NATIVE)/.deps/native/ps5-payload-sdk
NATIVE_TOOL := $(NATIVE)/build/host/ps5-native-tool
RUNTIME := $(NATIVE)/runtime/libc.prx
CC := clang -target x86_64-sie-ps5 -fvisibility-nodllstorageclass=default \
	-isysroot $(SDK) -isystem $(SDK)/target/include \
	-fno-stack-protector -fno-plt -femulated-tls -ffunction-sections -fdata-sections -O2 -Wall -Wextra
STUBS := build/stubs

all: dist/eboot.bin dist/libc.prx

$(STUBS)/.sdk:
	mkdir -p $(STUBS)
	cp $(SDK)/target/lib/*.so $(STUBS)/
	touch $@

build/llvm-pie.elf: src/launcher.c src/crt0.c $(STUBS)/.sdk
	$(CC) -c src/launcher.c -o build/launcher.o
	$(CC) -c src/crt0.c -o build/crt0.o
	$(SDK)/bin/prospero-lld -T $(NATIVE)/tooling/native/ps5-pie.ld --eh-frame-hdr --gc-sections \
		-e _start -o $@ build/crt0.o build/launcher.o --as-needed $(STUBS)/*.so

build/eboot.elf: build/llvm-pie.elf
	$(NATIVE_TOOL) link --in $< --out $@ --stub-dir $(STUBS) \
		--module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf

dist/eboot.bin: build/eboot.elf
	mkdir -p dist
	$(NATIVE_TOOL) self --sign --in $< --out $@ --magic 0x1D3D154F

dist/libc.prx: $(RUNTIME)
	mkdir -p dist
	cp $< $@

clean:
	rm -rf build dist

.PHONY: all clean

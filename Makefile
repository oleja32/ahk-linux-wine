CC ?= gcc
AHKD ?= build/ahkd
WINE_INC ?= /usr/include/wine
WINE_LIB ?= /usr/lib/wine/x86_64-windows
CLANG_INC := $(shell clang -print-resource-dir)/include
all: build/ahkd build/version.dll
build/ahkd: daemon/ahkd.c daemon/kwin.js proto.h
	@mkdir -p build
	$(CC) -O2 -Wall -Wextra -Wno-missing-field-initializers -o $@ $< -lsystemd -lxkbcommon
build/version.dll: dll/bridge.c dll/version.def proto.h
	@mkdir -p build
	clang --target=x86_64-w64-mingw32 -O2 -Wall -Wno-missing-braces -fno-stack-protector -fno-builtin -nostdinc -isystem $(CLANG_INC) -isystem $(WINE_INC)/windows -isystem $(WINE_INC)/msvcrt -c dll/bridge.c -o build/bridge.o
	lld-link /dll /entry:DllMain /nodefaultlib /def:dll/version.def /out:$@ build/bridge.o /libpath:$(WINE_LIB) libkernel32.a libuser32.a libws2_32.a
# ready-made archive for a release; AHKD=<ahkd built on an old distribution> runs on newer ones too
dist: all
	rm -rf build/ahk-linux-wine && mkdir -p build/ahk-linux-wine/prebuilt build/ahk-linux-wine/launcher
	cp README.md LICENSE VERSION check.sh setup.sh install.sh uninstall.sh build/ahk-linux-wine/
	cp launcher/ahk launcher/detect.ahk launcher/icons.py build/ahk-linux-wine/launcher/
	cp $(AHKD) build/version.dll build/ahk-linux-wine/prebuilt/
	test "$$(cut -d" " -f2 VERSION)" = "$$(sed -n "s/.*define AHKB_PROTO \([0-9]*\).*/\1/p" proto.h)"
	tar -C build --owner=0 --group=0 -czf build/ahk-linux-wine.tar.gz ahk-linux-wine
	cd build && sha256sum ahk-linux-wine.tar.gz > ahk-linux-wine.tar.gz.sha256
clean:
	rm -rf build
.PHONY: all dist clean

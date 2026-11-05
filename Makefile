MINGW_CC ?= x86_64-w64-mingw32-gcc
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu17

all: build/wpcap.dll build/questlog-ctl.exe build/questlog-capture

build/wpcap.dll: wpcap/wpcap.c
	@mkdir -p build
	$(MINGW_CC) $(CFLAGS) -shared -static-libgcc -s -o $@ $< -lws2_32

build/questlog-ctl.exe: ctl/questlog-ctl.c
	@mkdir -p build
	$(MINGW_CC) $(CFLAGS) -municode -static-libgcc -s -o $@ $< -lshell32

build/questlog-capture: capture/questlog-capture.c
	@mkdir -p build
	$(CC) $(CFLAGS) -D_FORTIFY_SOURCE=2 -fstack-protector-strong -o $@ $< -lpcap

# Windows test programs: run them under Wine in a throwaway prefix (never the game prefix); each prints PASS or FAIL
tests: build/tests/zorder-test.exe build/tests/fake-game.exe build/tests/tray-test.exe

build/tests/%.exe: tests/%.c
	@mkdir -p build/tests
	$(MINGW_CC) $(CFLAGS) -static-libgcc -o $@ $< -lshell32

clean:
	rm -rf build

.PHONY: all tests clean

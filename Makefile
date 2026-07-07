# LoRa serial-transport — standard build entry points.
#
# Thin wrapper around PlatformIO so the project builds the conventional `make`
# way. PlatformIO (the actual build system) does the real work; see
# platformio.ini. Install it once with:  pipx install platformio
#
# Common usage:
#   make            # build the firmware image
#   make test       # run the native unit/sim test suite (no hardware)
#   make flash                       # auto-detect & flash the connected board
#   make flash BOARD=l1              # name a board (xiao|l1) if two are plugged in
#   make format-check                # fail if any source line exceeds 80 cols
#   make clean
#
# Each board runs the image for its chip (xiao -> node_raw, l1 -> wio_l1) and
# auto-elects its half-duplex role (initiator/responder) + 1-byte address from
# its chip MAC (+ proximity pairing) at runtime, so you never pick a role or a
# per-board build — and `make flash` auto-detects which board is on which port.
# See "Roles: initiator & responder" in the README.

PIO    ?= pio
BOARD  ?=                # xiao | l1 (empty = auto-detect from USB identity)
PORT   ?=                # /dev/ttyACMx (empty = auto-detect from USB identity)
FLASH  := ./tools/upload_flash.sh

# Source files we hold to the 80-column rule (vendored libs are exempt).
FMT_FILES := $(wildcard src/*.cpp src/*.h lib/linklayer/*.h lib/linklayer/*.cpp \
                        test/test_link/*.cpp test/test_modem/*.cpp \
                        test/test_sim/*.cpp tools/*.py)

.PHONY: all build test flash boards monitor format-check clean help

all: build

## build: compile the firmware image (node_raw)
build:
	$(PIO) run -e node_raw

## test: run the native unit/simulation test suite (no hardware needed)
test:
	$(PIO) test -e native

## flash: auto-detect & flash the connected board (BOARD=xiao|l1, PORT=/dev/ttyXXX
## to narrow it when several are plugged in). `tools/upload_flash.sh --help`.
flash:
	$(FLASH) $(BOARD) $(PORT)

## boards: list connected LoRa-Serial boards (name, env, port, chip serial)
boards:
	$(FLASH) --list

## monitor: open the serial monitor (PORT=/dev/ttyXXX, or PlatformIO auto-picks)
monitor:
	$(PIO) device monitor $(if $(strip $(PORT)),-p $(PORT),) -b 115200

## format-check: fail if any of our (non-vendored) source lines exceed 80 columns
format-check:
	@python3 -c "import sys; \
bad=[(f,n,len(l.rstrip())) for f in '$(FMT_FILES)'.split() \
for n,l in enumerate(open(f,encoding='utf-8'),1) if len(l.rstrip('\n'))>80]; \
[print(f'{f}:{n}: {w} cols') for f,n,w in bad]; \
print('OK: all lines <= 80 columns') if not bad else sys.exit(1)"

## clean: remove PlatformIO build artifacts
clean:
	$(PIO) run -t clean ; rm -rf .pio/build

## help: list the available targets
help:
	@echo "LoRa serial-transport — make targets:"
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/^## /  /'

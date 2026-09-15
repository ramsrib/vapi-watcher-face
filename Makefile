# Convenience wrapper around ESP-IDF's idf.py.
#
#   make deps       # one-time: clone Seeed's SDK into deps/
#   make setup      # one-time: create .env (then fill in WiFi + Vapi keys)
#   make run        # build + flash + monitor
#
# Requires ESP-IDF v5.5.1 or newer. Point IDF_PATH at your install, or export it
# once in your shell — the usual `. $HOME/esp/esp-idf/export.sh` also works.
#
# Override the port if auto-detect picks the wrong device:
#   make PORT=/dev/cu.usbmodem101 run
#
# Run `make help` for the full list of targets.

SHELL := /bin/bash

# ../_sdk/esp-idf is checked too, matching where WATCHER_SDK is looked for, so
# a machine that keeps both SDKs beside the project needs no configuration.
ifeq ($(origin IDF_PATH), undefined)
  IDF_PATH := $(firstword $(wildcard $(HOME)/esp/esp-idf $(HOME)/esp-idf ../_sdk/esp-idf ../../_sdk/esp-idf))
endif
EXPORT := $(IDF_PATH)/export.sh

TARGET ?= esp32s3

# Seeed's SDK. Cloned by `make deps`; override to share one checkout between
# projects:  make WATCHER_SDK=~/src/SenseCAP-Watcher-Firmware build
# Absolute, because CMake resolves a relative path against the build directory,
# not the project — which fails in a way that reads as "SDK not found".
WATCHER_SDK ?= $(abspath $(firstword $(wildcard deps/SenseCAP-Watcher-Firmware ../_sdk/SenseCAP-Watcher-Firmware)))
SDK_URL := https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware.git

# The Watcher exposes TWO USB serial endpoints: the ESP32 console and the
# Himax's own console. They are adjacent and which is which is not guessable
# from the name, so auto-detect takes the last and you override when wrong.
# `make monitor` on the wrong one is silent or mojibake, not an error.
PORT ?= $(lastword $(wildcard /dev/cu.usbmodem* /dev/ttyACM* /dev/cu.usbserial* /dev/ttyUSB*))

IDF = source "$(EXPORT)" >/dev/null && WATCHER_SDK="$(WATCHER_SDK)" idf.py $(if $(PORT),-p "$(PORT)",)

.DEFAULT_GOAL := build

.PHONY: help
help: ## Show this help
	@grep -hE '^[a-zA-Z_-]+:.*?## ' $(MAKEFILE_LIST) | \
	  awk 'BEGIN{FS=":.*?## "}{printf "  \033[36m%-14s\033[0m %s\n", $$1, $$2}'

.PHONY: check-idf
check-idf:
	@if [ -z "$(IDF_PATH)" ] || [ ! -f "$(EXPORT)" ]; then \
	  echo "ESP-IDF not found."; \
	  echo "Install v5.5.1+ (https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/get-started/),"; \
	  echo "then either export IDF_PATH, or run:  make IDF_PATH=/path/to/esp-idf $(MAKECMDGOALS)"; \
	  exit 1; \
	fi

.PHONY: check-sdk
check-sdk:
	@if [ ! -d "$(WATCHER_SDK)/components/sensecap-watcher" ]; then \
	  echo "SenseCAP-Watcher-Firmware not found."; \
	  echo "Run:  make deps"; \
	  echo "  (or point at an existing checkout: make WATCHER_SDK=/path/to/it $(MAKECMDGOALS))"; \
	  exit 1; \
	fi

.PHONY: deps
deps: ## Clone Seeed's SDK into deps/ (one-time, ~large)
	@if [ -d deps/SenseCAP-Watcher-Firmware/components/sensecap-watcher ]; then \
	  echo "deps/SenseCAP-Watcher-Firmware already present."; \
	else \
	  mkdir -p deps && git clone --recursive $(SDK_URL) deps/SenseCAP-Watcher-Firmware; \
	fi

.PHONY: ensure-target
ensure-target: check-idf check-sdk
	@if [ ! -f build/CMakeCache.txt ] || ! grep -q "IDF_TARGET:STRING=$(TARGET)" build/CMakeCache.txt 2>/dev/null; then \
	  echo ">> configuring target $(TARGET) (first run — this also fetches components, and can take a few minutes)…"; \
	  source "$(EXPORT)" >/dev/null && WATCHER_SDK="$(WATCHER_SDK)" idf.py set-target $(TARGET); \
	fi

# Regenerate env_config.generated.h from .env before every build, so edits to
# .env always take effect. The generator only rewrites when content changed, so
# this stays a no-op when nothing moved.
.PHONY: gen-env
gen-env:
	@python3 tools/gen_env_header.py .env main/env_config.generated.h

.PHONY: check-port
check-port:
	@if [ -z "$(PORT)" ]; then \
	  echo "No serial port auto-detected."; \
	  echo "Plug in the board, or pass one: make PORT=/dev/cu.usbmodem101 $(MAKECMDGOALS)"; \
	  exit 1; \
	fi

.PHONY: setup
setup: ## One-time setup: create .env (edit it after) and configure the target
	@if [ -f .env ]; then echo ".env already present."; \
	else cp .env.example .env && echo ">> created .env — fill in WIFI_SSID / WIFI_PASSWORD / VAPI_API_KEY / VAPI_ASSISTANT_ID, then: make run"; fi
	@$(MAKE) --no-print-directory ensure-target

.PHONY: build
build: gen-env ensure-target ## Build the firmware
	$(IDF) build

# NOT idf.py flash. The Watcher's CH342 USB-serial bridge silently drops bytes
# on writes larger than 256 bytes, so esptool's defaults fail on the first
# block while reads work perfectly. tools/flash.py caps every loader's write
# size. See the README.
.PHONY: flash
flash: build check-port ## Build + flash (CH342-safe; never use idf.py flash here)
	python3 tools/flash.py $(if $(PORT),"$(PORT)",)

.PHONY: monitor
monitor: check-idf check-port ## Open the serial monitor
	$(IDF) monitor

.PHONY: run flashmonitor
run flashmonitor: flash ## Build + flash + monitor (the usual one)
	$(IDF) monitor

.PHONY: menuconfig
menuconfig: ensure-target ## Open the IDF configuration menu (alternative to .env)
	source "$(EXPORT)" >/dev/null && WATCHER_SDK="$(WATCHER_SDK)" idf.py menuconfig

.PHONY: frame
frame: check-port ## Save what the camera sees as frame.jpg (needs VISION_DUMP_FRAME)
	python3 tools/grab_frame.py --reset $(if $(PORT),--port "$(PORT)",)

.PHONY: size
size: gen-env ensure-target ## Show binary size breakdown
	$(IDF) size

.PHONY: clean
clean: check-idf ## Remove build output
	source "$(EXPORT)" >/dev/null && idf.py clean

.PHONY: fullclean
fullclean: ## Remove build/, managed_components/, and the dependency lock
	@if [ -n "$(IDF_PATH)" ] && [ -f "$(EXPORT)" ]; then \
	  source "$(EXPORT)" >/dev/null && idf.py fullclean || true; \
	fi
	rm -rf build managed_components dependencies.lock

# There is deliberately no `erase` target.
#
# `idf.py erase-flash` wipes the whole chip, including the nvsfactory partition
# at 0x9000 that holds this device's provisioned identity — its EUI and SenseCraft
# credentials. Those are written at the factory and cannot be regenerated. The
# sibling AtomS3R project has an erase target because there is nothing there to
# lose; here there is.

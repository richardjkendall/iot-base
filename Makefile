# Command-line build/flash for the ESP-01 bootstrap sketch, via arduino-cli.
#
#   make            build -> build/iot-base.ino.bin
#   make flash      build + upload over serial
#   make factory    build + upload with full flash erase (wipes LittleFS config)
#   make monitor    serial monitor at 115200
#   make deps       install ESP8266 core + ArduinoJson
#   make ports      list serial ports
#
# Override anything on the command line, e.g. `make flash PORT=/dev/cu.usbserial-110`.

SKETCH      := iot-base
BUILD_DIR   := build
BIN         := $(BUILD_DIR)/$(SKETCH).ino.bin

# Prefer arduino-cli on PATH (brew install arduino-cli), fall back to the copy
# bundled inside Arduino IDE 2. Both use ~/Library/Arduino15 and
# ~/Documents/Arduino, so cores/libraries are shared with the IDE.
IDE_CLI     := /Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli
ARDUINO_CLI ?= $(shell command -v arduino-cli 2>/dev/null || { [ -x "$(IDE_CLI)" ] && printf '%s' "$(IDE_CLI)" | sed 's/ /\\ /g'; })

ESP8266_URL := https://arduino.esp8266.com/stable/package_esp8266com_index.json

# Generic ESP8266, 1MB flash with 64KB LittleFS -> ~470KB max OTA image.
BOARD_OPTS  ?= eesz=1M64,FlashMode=dout,ResetMethod=nodemcu,baud=115200
FQBN        ?= esp8266:esp8266:generic:$(BOARD_OPTS)

PORT        ?= $(firstword $(wildcard /dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART*))
MONITOR_BAUD ?= 115200

SOURCES     := $(SKETCH).ino $(wildcard *.h *.cpp)

.PHONY: all build flash factory monitor deps ports clean check-cli check-port

all: build

build: $(BIN)

$(BIN): $(SOURCES) Makefile | check-cli
	$(ARDUINO_CLI) compile --fqbn $(FQBN) --output-dir $(BUILD_DIR) --warnings default .
	@ls -l $(BIN)

flash: $(BIN) check-port
	$(ARDUINO_CLI) upload --fqbn $(FQBN) --port $(PORT) --input-dir $(BUILD_DIR) .

# wipe=all makes esptool erase the whole chip first, so the device boots
# with no /config.json and comes up in setup-portal mode.
factory: check-cli check-port
	$(ARDUINO_CLI) compile --fqbn $(FQBN),wipe=all --output-dir $(BUILD_DIR) --warnings default .
	$(ARDUINO_CLI) upload --fqbn $(FQBN),wipe=all --port $(PORT) --input-dir $(BUILD_DIR) .

# DTR/RTS off so the monitor doesn't hold the ESP-01 in reset/bootloader.
monitor: check-port
	$(ARDUINO_CLI) monitor --port $(PORT) --config baudrate=$(MONITOR_BAUD),dtr=off,rts=off

deps: check-cli
	$(ARDUINO_CLI) core update-index --additional-urls $(ESP8266_URL)
	$(ARDUINO_CLI) core install esp8266:esp8266 --additional-urls $(ESP8266_URL)
	$(ARDUINO_CLI) lib install ArduinoJson@7.4.2

ports: check-cli
	$(ARDUINO_CLI) board list

clean:
	rm -rf $(BUILD_DIR)

check-cli:
	@[ -n "$(ARDUINO_CLI)" ] || { echo "arduino-cli not found: brew install arduino-cli (or install Arduino IDE 2)"; exit 1; }

check-port: check-cli
	@[ -n "$(PORT)" ] || { echo "No USB serial port found. Plug in the adapter or pass PORT=/dev/cu.xxx"; exit 1; }

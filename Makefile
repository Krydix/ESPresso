ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_DIR ?= $(ROOT_DIR)/build
IDF_PATH ?= $(HOME)/esp/esp-idf
IDF_PYTHON_ENV_PATH ?= $(firstword $(wildcard $(HOME)/.espressif/python_env/idf5.4*_env))
PORT ?=
WEB_INSTALLER_DIR ?= $(BUILD_DIR)/web-installer

.DEFAULT_GOAL := help

define run_idf
	@bash -lc 'set -eo pipefail; \
		test -f "$(IDF_PATH)/export.sh" || { echo "ESP-IDF not found at $(IDF_PATH)"; exit 1; }; \
		if [ -n "$(IDF_PYTHON_ENV_PATH)" ]; then export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV_PATH)"; fi; \
		. "$(IDF_PATH)/export.sh" >/dev/null 2>&1; \
		cd "$(ROOT_DIR)"; \
		idf.py -B "$(BUILD_DIR)" $(1)'
endef

.PHONY: help build reconfigure clean fullclean flash monitor flash-monitor size test test-cups web-installer

help:
	@printf '%s\n' \
		'ESPresso targets:' \
		'  make build                 Build ESP32-S3 firmware' \
		'  make flash PORT=/dev/...   Flash a connected board' \
		'  make monitor PORT=/dev/... Open the serial monitor' \
		'  make flash-monitor PORT=... Build, flash, and monitor' \
		'  make test                  Run host-side IPP codec tests' \
		'  make test-cups             Validate normalized IPP with CUPS ipptool' \
		'  make web-installer         Stage the GitHub Pages flasher' \
		'  make size                  Show firmware size information'

build:
	$(call run_idf,build)

reconfigure:
	$(call run_idf,reconfigure)

clean:
	$(call run_idf,clean)

fullclean:
	$(call run_idf,fullclean)

flash:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	$(call run_idf,-p "$(PORT)" flash)

monitor:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	$(call run_idf,-p "$(PORT)" monitor)

flash-monitor:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	$(call run_idf,-p "$(PORT)" flash monitor)

size:
	$(call run_idf,size)

test:
	@mkdir -p "$(BUILD_DIR)/host-tests"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/ipp_codec.c" \
		"$(ROOT_DIR)/tests/test_ipp_codec.c" \
		-o "$(BUILD_DIR)/host-tests/test_ipp_codec"
	@"$(BUILD_DIR)/host-tests/test_ipp_codec"

test-cups:
	@command -v ipptool >/dev/null || { echo "ipptool is required"; exit 1; }
	@sh scripts/test_cups_oracle.sh

web-installer:
	@python3 scripts/stage_web_installer.py \
		--build-dir "$(BUILD_DIR)" \
		--output-dir "$(WEB_INSTALLER_DIR)"

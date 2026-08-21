ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
TARGET ?= esp32s3
SUPPORTED_TARGETS := esp32 esp32s2 esp32s3
BUILD_DIR ?= $(ROOT_DIR)/build/$(TARGET)
SDKCONFIG ?= $(BUILD_DIR)/sdkconfig
IDF_PATH ?= $(HOME)/esp/esp-idf
IDF_PYTHON_ENV_PATH ?= $(firstword $(wildcard $(HOME)/.espressif/python_env/idf5.4*_env))
PORT ?=
WEB_INSTALLER_DIR ?= $(ROOT_DIR)/build/web-installer

ifeq ($(filter $(TARGET),$(SUPPORTED_TARGETS)),)
$(error unsupported TARGET '$(TARGET)'; choose one of: $(SUPPORTED_TARGETS))
endif

.DEFAULT_GOAL := help

define run_idf
	@bash -lc 'set -eo pipefail; \
		test -f "$(IDF_PATH)/export.sh" || { echo "ESP-IDF not found at $(IDF_PATH)"; exit 1; }; \
		if [ -n "$(IDF_PYTHON_ENV_PATH)" ]; then export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV_PATH)"; fi; \
		. "$(IDF_PATH)/export.sh" >/dev/null 2>&1; \
		cd "$(ROOT_DIR)"; \
		IDF_TARGET="$(TARGET)" idf.py -B "$(BUILD_DIR)" \
			-D SDKCONFIG="$(SDKCONFIG)" $(1)'
endef

.PHONY: help build reconfigure clean fullclean flash monitor flash-monitor size test test-cups test-compat test-sanitize test-fuzz-smoke test-roadmap test-conformance-report web-installer web-installer-all

help:
	@printf '%s\n' \
		'ESPresso targets:' \
		'  make build [TARGET=...]    Build ESP32-S3 (default), ESP32, or ESP32-S2 firmware' \
		'  make flash PORT=/dev/...   Flash a connected board' \
		'  make monitor PORT=/dev/... Open the serial monitor' \
		'  make flash-monitor PORT=... Build, flash, and monitor' \
		'  make test                  Run host-side IPP codec tests' \
		'  make test-cups             Validate normalized IPP with CUPS ipptool' \
		'  make test-compat           Run the CUPS differential compatibility lab' \
		'  make test-sanitize         Run codec tests with ASan and UBSan' \
		'  make test-fuzz-smoke       Run the coverage-guided IPP codec fuzz gate' \
		'  make test-roadmap          Validate and report the feature test matrix' \
		'  make test-conformance-report  Record expected-red full CUPS suites' \
		'  make web-installer         Stage a flasher for TARGET' \
		'  make web-installer-all     Stage the auto-detecting three-target flasher' \
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
		"$(ROOT_DIR)/main/improv_serial_codec.c" \
		"$(ROOT_DIR)/tests/test_improv_serial_codec.c" \
		-o "$(BUILD_DIR)/host-tests/test_improv_serial_codec"
	@"$(BUILD_DIR)/host-tests/test_improv_serial_codec"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/ipp_codec.c" \
		"$(ROOT_DIR)/tests/test_ipp_codec.c" \
		-o "$(BUILD_DIR)/host-tests/test_ipp_codec"
	@"$(BUILD_DIR)/host-tests/test_ipp_codec"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/ipp_codec.c" \
		"$(ROOT_DIR)/main/ipp_proxy_core.c" \
		"$(ROOT_DIR)/tests/test_ipp_proxy_core.c" \
		-o "$(BUILD_DIR)/host-tests/test_ipp_proxy_core"
	@"$(BUILD_DIR)/host-tests/test_ipp_proxy_core"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/printer_advertisement.c" \
		"$(ROOT_DIR)/tests/test_printer_advertisement.c" \
		-o "$(BUILD_DIR)/host-tests/test_printer_advertisement"
	@"$(BUILD_DIR)/host-tests/test_printer_advertisement"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/ipp_stream.c" \
		"$(ROOT_DIR)/tests/test_ipp_stream.c" \
		-o "$(BUILD_DIR)/host-tests/test_ipp_stream"
	@"$(BUILD_DIR)/host-tests/test_ipp_stream"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/ipp_http_request.c" \
		"$(ROOT_DIR)/tests/test_ipp_http_request.c" \
		-o "$(BUILD_DIR)/host-tests/test_ipp_http_request"
	@"$(BUILD_DIR)/host-tests/test_ipp_http_request"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/ipp_codec.c" \
		"$(ROOT_DIR)/tests/test_ipp_fuzz_smoke.c" \
		-o "$(BUILD_DIR)/host-tests/test_ipp_fuzz_smoke"
	@"$(BUILD_DIR)/host-tests/test_ipp_fuzz_smoke"

test-cups:
	@command -v ipptool >/dev/null || { echo "ipptool is required"; exit 1; }
	@sh scripts/test_cups_oracle.sh

test-compat:
	@command -v ipptool >/dev/null || { echo "ipptool is required"; exit 1; }
	@command -v cups-config >/dev/null || { echo "cups-config is required"; exit 1; }
	@mkdir -p "$(BUILD_DIR)/host-tests"
	@$(CC) -std=c11 -Wall -Wextra -Werror -fPIC -shared \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/ipp_codec.c" \
		"$(ROOT_DIR)/main/ipp_proxy_core.c" \
		"$(ROOT_DIR)/tests/compat/codec_bridge.c" \
		-o "$(BUILD_DIR)/host-tests/libespresso_compat.so"
	@python3 "$(ROOT_DIR)/tests/compat/compat_lab.py" \
		--root "$(ROOT_DIR)" \
		--library "$(BUILD_DIR)/host-tests/libespresso_compat.so" \
		$(sort $(wildcard $(ROOT_DIR)/tests/compat/fixtures/*.json))

test-sanitize:
	@mkdir -p "$(BUILD_DIR)/host-tests"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/improv_serial_codec.c" \
		"$(ROOT_DIR)/tests/test_improv_serial_codec.c" \
		-o "$(BUILD_DIR)/host-tests/test_improv_serial_codec_sanitize"
	@ASAN_OPTIONS=detect_leaks=0 "$(BUILD_DIR)/host-tests/test_improv_serial_codec_sanitize"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/ipp_codec.c" \
		"$(ROOT_DIR)/tests/test_ipp_codec.c" \
		-o "$(BUILD_DIR)/host-tests/test_ipp_codec_sanitize"
	@ASAN_OPTIONS=detect_leaks=0 "$(BUILD_DIR)/host-tests/test_ipp_codec_sanitize"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/ipp_codec.c" \
		"$(ROOT_DIR)/main/ipp_proxy_core.c" \
		"$(ROOT_DIR)/tests/test_ipp_proxy_core.c" \
		-o "$(BUILD_DIR)/host-tests/test_ipp_proxy_core_sanitize"
	@ASAN_OPTIONS=detect_leaks=0 "$(BUILD_DIR)/host-tests/test_ipp_proxy_core_sanitize"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/printer_advertisement.c" \
		"$(ROOT_DIR)/tests/test_printer_advertisement.c" \
		-o "$(BUILD_DIR)/host-tests/test_printer_advertisement_sanitize"
	@ASAN_OPTIONS=detect_leaks=0 "$(BUILD_DIR)/host-tests/test_printer_advertisement_sanitize"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/ipp_stream.c" \
		"$(ROOT_DIR)/tests/test_ipp_stream.c" \
		-o "$(BUILD_DIR)/host-tests/test_ipp_stream_sanitize"
	@ASAN_OPTIONS=detect_leaks=0 "$(BUILD_DIR)/host-tests/test_ipp_stream_sanitize"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/ipp_http_request.c" \
		"$(ROOT_DIR)/tests/test_ipp_http_request.c" \
		-o "$(BUILD_DIR)/host-tests/test_ipp_http_request_sanitize"
	@ASAN_OPTIONS=detect_leaks=0 "$(BUILD_DIR)/host-tests/test_ipp_http_request_sanitize"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/ipp_codec.c" \
		"$(ROOT_DIR)/tests/test_ipp_fuzz_smoke.c" \
		-o "$(BUILD_DIR)/host-tests/test_ipp_fuzz_smoke_sanitize"
	@ASAN_OPTIONS=detect_leaks=0 "$(BUILD_DIR)/host-tests/test_ipp_fuzz_smoke_sanitize"

test-fuzz-smoke:
	@command -v clang >/dev/null || { echo "clang is required"; exit 1; }
	@mkdir -p "$(BUILD_DIR)/host-tests/fuzz-artifacts"
	@clang -std=c11 -Wall -Wextra -Werror -fno-omit-frame-pointer \
		-fsanitize=address,undefined -fsanitize-coverage=trace-pc-guard \
		-I"$(ROOT_DIR)/main" \
		-c "$(ROOT_DIR)/main/ipp_codec.c" \
		-o "$(BUILD_DIR)/host-tests/ipp_codec_fuzz.o"
	@clang -std=c11 -Wall -Wextra -Werror -fno-omit-frame-pointer \
		-fsanitize=address,undefined -DESPRESSO_STANDALONE_FUZZ \
		-I"$(ROOT_DIR)/main" \
		-c "$(ROOT_DIR)/tests/fuzz_ipp_codec.c" \
		-o "$(BUILD_DIR)/host-tests/fuzz_ipp_codec.o"
	@clang -fsanitize=address,undefined \
		"$(BUILD_DIR)/host-tests/ipp_codec_fuzz.o" \
		"$(BUILD_DIR)/host-tests/fuzz_ipp_codec.o" \
		-o "$(BUILD_DIR)/host-tests/fuzz_ipp_codec"
	@ASAN_OPTIONS=detect_leaks=0 "$(BUILD_DIR)/host-tests/fuzz_ipp_codec"

test-roadmap:
	@python3 "$(ROOT_DIR)/scripts/check_feature_manifest.py" \
		"$(ROOT_DIR)/tests/feature-matrix.json"
	@mkdir -p "$(BUILD_DIR)/host-tests"
	@$(CC) -std=c11 -Wall -Wextra -Werror -fPIC -shared \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/ipp_codec.c" \
		"$(ROOT_DIR)/main/ipp_proxy_core.c" \
		"$(ROOT_DIR)/tests/compat/codec_bridge.c" \
		-o "$(BUILD_DIR)/host-tests/libespresso_compat.so"
	@python3 "$(ROOT_DIR)/tests/roadmap/roadmap_probes.py" \
		--library "$(BUILD_DIR)/host-tests/libespresso_compat.so" \
		--fixture "$(ROOT_DIR)/tests/roadmap/fixtures/format-conditional.json" \
		--output "$(BUILD_DIR)/host-tests/roadmap-probes.json"

test-conformance-report:
	@command -v ipptool >/dev/null || { echo "ipptool is required"; exit 1; }
	@command -v cups-config >/dev/null || { echo "cups-config is required"; exit 1; }
	@mkdir -p "$(BUILD_DIR)/host-tests"
	@$(CC) -std=c11 -Wall -Wextra -Werror -fPIC -shared \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/ipp_codec.c" \
		"$(ROOT_DIR)/main/ipp_proxy_core.c" \
		"$(ROOT_DIR)/tests/compat/codec_bridge.c" \
		-o "$(BUILD_DIR)/host-tests/libespresso_compat.so"
	@python3 "$(ROOT_DIR)/tests/compat/compat_lab.py" \
		--root "$(ROOT_DIR)" \
		--library "$(BUILD_DIR)/host-tests/libespresso_compat.so" \
		--conformance-report "$(BUILD_DIR)/host-tests/conformance-roadmap.json" \
		"$(ROOT_DIR)/tests/compat/fixtures/basic-airprint.json"

web-installer:
	@python3 scripts/stage_web_installer.py \
		--build "$(TARGET)=$(BUILD_DIR)" \
		--output-dir "$(WEB_INSTALLER_DIR)"

web-installer-all:
	@python3 scripts/stage_web_installer.py \
		--build "esp32=$(ROOT_DIR)/build/esp32" \
		--build "esp32s2=$(ROOT_DIR)/build/esp32s2" \
		--build "esp32s3=$(ROOT_DIR)/build/esp32s3" \
		--output-dir "$(WEB_INSTALLER_DIR)"

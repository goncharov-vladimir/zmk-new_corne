.PHONY: help init build build-left build-right build-dongle build-studio build-reset build-left-led-scan build-right-led-scan clean pristine

# ── Variables ────────────────────────────────────────────────────────────────
BUILD_DIR  := build
FIRMWARE   := firmware
CONFIG_DIR := config
APP_SRC    := zmk/app

# Extra ZMK modules (semicolon-separated).
# eyelash_corne_module: board + shield definitions for this keyboard.
# zmk-ws2812-driver: RGB widget driver (west-managed but needs explicit registration).
EXTRA_MODULES := $(PWD)/eyelash_corne_module;$(PWD)/modules/zmk-ws2812-driver

# Common CMake flags
CMAKE_EXTRA :=

# Boards
BOARD_LEFT  := eyelash_corne_left
BOARD_RIGHT := eyelash_corne_right
BOARD_DONGLE := nice_nano_v2

# Shields
SHIELD_LEFT   := nice_view
SHIELD_RIGHT  := nice_view
SHIELD_DONGLE := eyelash_corne_dongle dongle_display

# ── Toolchain ────────────────────────────────────────────────────────────────
export ZEPHYR_TOOLCHAIN_VARIANT := gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH := /Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi

# ── Help ─────────────────────────────────────────────────────────────────────
help:
	@echo "ZMK Firmware — Eyelash Corne (dongle setup)"
	@echo ""
	@echo "  make init           — Initialize west workspace (first time)"
	@echo "  make build          — Build all firmware variants"
	@echo "  make build-left     — Build left half (peripheral)"
	@echo "  make build-right    — Build right half (peripheral)"
	@echo "  make build-dongle   — Build dongle (central)"
	@echo "  make build-studio   — Build left half with ZMK Studio"
	@echo "  make build-left-debug  — Build left half standalone (no BLE, USB console)"
	@echo "  make build-right-debug — Build right half standalone (no BLE, USB console)"
	@echo "  make build-left-led-scan  — Build left raw LED address scanner"
	@echo "  make build-right-led-scan — Build right raw LED address scanner"
	@echo "  make build-reset    — Build settings reset firmware"
	@echo ""
	@echo "  make clean          — Remove build/ artifacts"
	@echo "  make pristine       — Full reset (removes zmk/, modules/, .west/ too)"
	@echo ""
	@echo "Output: $(FIRMWARE)/*.uf2"

# ── Init ─────────────────────────────────────────────────────────────────────
init:
	@echo "Initializing west workspace..."
	@if [ ! -f .west/config ]; then \
		west init -l $(CONFIG_DIR); \
		west update; \
		west zephyr-export; \
		python3 patches/apply.py; \
	else \
		echo "Workspace already initialized. Run 'make pristine' to reset."; \
	fi

# ── Build all ────────────────────────────────────────────────────────────────
build: build-right build-left build-dongle build-reset
	@echo "✓ All firmware built → $(FIRMWARE)/"

# ── Individual builds ────────────────────────────────────────────────────────
build-left:
	@echo "Building LEFT half (peripheral)..."
	@west build -p -s $(APP_SRC) -d $(BUILD_DIR)/left -b $(BOARD_LEFT) -- \
			-DSHIELD="$(SHIELD_LEFT)" \
			-DCONFIG_ZMK_SPLIT_ROLE_CENTRAL=n \
			-DZMK_CONFIG="$(PWD)/$(CONFIG_DIR)" \
			-DZMK_EXTRA_MODULES="$(EXTRA_MODULES)" \
			$(CMAKE_EXTRA)
	@mkdir -p $(FIRMWARE)
	@cp $(BUILD_DIR)/left/zephyr/zmk.uf2 $(FIRMWARE)/$(BOARD_LEFT).uf2
	@echo "✓ $(FIRMWARE)/$(BOARD_LEFT).uf2"

build-right:
	@echo "Building RIGHT half (peripheral)..."
	@west build -p -s $(APP_SRC) -d $(BUILD_DIR)/right -b $(BOARD_RIGHT) -- \
		-DSHIELD="$(SHIELD_RIGHT)" \
		-DZMK_CONFIG="$(PWD)/$(CONFIG_DIR)" \
		-DZMK_EXTRA_MODULES="$(EXTRA_MODULES)" \
		$(CMAKE_EXTRA)
	@mkdir -p $(FIRMWARE)
	@cp $(BUILD_DIR)/right/zephyr/zmk.uf2 $(FIRMWARE)/$(BOARD_RIGHT).uf2
	@echo "✓ $(FIRMWARE)/$(BOARD_RIGHT).uf2"

build-left-led-scan:
	@echo "Building LEFT raw LED address scanner..."
	@west build -p -s $(APP_SRC) -d $(BUILD_DIR)/left-led-scan -b $(BOARD_LEFT) -- \
			-DSHIELD="$(SHIELD_LEFT)" \
			-DCONFIG_ZMK_SPLIT_ROLE_CENTRAL=n \
			-DZMK_CONFIG="$(PWD)/$(CONFIG_DIR)" \
			-DZMK_EXTRA_MODULES="$(EXTRA_MODULES)" \
			-DEXTRA_CONF_FILE="$(PWD)/$(CONFIG_DIR)/led_auto_scan.conf" \
			$(CMAKE_EXTRA)
	@mkdir -p $(FIRMWARE)
	@cp $(BUILD_DIR)/left-led-scan/zephyr/zmk.uf2 $(FIRMWARE)/$(BOARD_LEFT)_led_scan.uf2
	@echo "✓ $(FIRMWARE)/$(BOARD_LEFT)_led_scan.uf2"

build-right-led-scan:
	@echo "Building RIGHT raw LED address scanner..."
	@west build -p -s $(APP_SRC) -d $(BUILD_DIR)/right-led-scan -b $(BOARD_RIGHT) -- \
			-DSHIELD="$(SHIELD_RIGHT)" \
			-DZMK_CONFIG="$(PWD)/$(CONFIG_DIR)" \
			-DZMK_EXTRA_MODULES="$(EXTRA_MODULES)" \
			-DEXTRA_CONF_FILE="$(PWD)/$(CONFIG_DIR)/led_auto_scan.conf" \
			$(CMAKE_EXTRA)
	@mkdir -p $(FIRMWARE)
	@cp $(BUILD_DIR)/right-led-scan/zephyr/zmk.uf2 $(FIRMWARE)/$(BOARD_RIGHT)_led_scan.uf2
	@echo "✓ $(FIRMWARE)/$(BOARD_RIGHT)_led_scan.uf2"

build-dongle:
	@echo "Building DONGLE (central)..."
	@west build -p -s $(APP_SRC) -d $(BUILD_DIR)/dongle -b $(BOARD_DONGLE) -- \
		-DSHIELD="$(SHIELD_DONGLE)" \
		-DZMK_CONFIG="$(PWD)/$(CONFIG_DIR)" \
		-DZMK_EXTRA_MODULES="$(EXTRA_MODULES)" \
		$(CMAKE_EXTRA)
	@mkdir -p $(FIRMWARE)
	@cp $(BUILD_DIR)/dongle/zephyr/zmk.uf2 $(FIRMWARE)/$(BOARD_DONGLE)_dongle.uf2
	@echo "✓ $(FIRMWARE)/$(BOARD_DONGLE)_dongle.uf2"

build-studio:
	@echo "Building LEFT half with ZMK Studio..."
	@west build -p -s $(APP_SRC) -d $(BUILD_DIR)/studio -b $(BOARD_LEFT) -- \
		-DSHIELD="$(SHIELD_LEFT)" \
		-DCONFIG_ZMK_STUDIO=y \
		-DCONFIG_ZMK_STUDIO_LOCKING=n \
		-DZMK_CONFIG="$(PWD)/$(CONFIG_DIR)" \
		-DZMK_EXTRA_MODULES="$(EXTRA_MODULES)" \
		$(CMAKE_EXTRA)
	@mkdir -p $(FIRMWARE)
	@cp $(BUILD_DIR)/studio/zephyr/zmk.uf2 $(FIRMWARE)/$(BOARD_LEFT)_studio.uf2
	@echo "✓ $(FIRMWARE)/$(BOARD_LEFT)_studio.uf2"

# NOTE: standalone (-DCONFIG_ZMK_SPLIT=n) was tried here to let one half
# run the keymap locally without the dongle, but it hits an unrelated
# Zephyr devicetree bug (DEVICE_DT_NAME for the stock &mo behavior node
# overflows Z_DEVICE_MAX_NAME_LEN only when ZMK_SPLIT=n). Keep these as
# split peripherals (CONFIG_ZMK_SPLIT_ROLE_CENTRAL=n) — pair with the
# dongle and read both USB logs side by side (dongle: layer_relay_cb /
# invoke_ws2812_layer_on_peripherals; half: "tick: layer N → ..."). If
# the dongle logs the new layer but the half's log doesn't follow within
# ~100ms, the fault is in the BLE relay, not in widget.c.
build-left-debug:
	@echo "Building LEFT half DEBUG (peripheral, USB console)..."
	@west build -p -s $(APP_SRC) -d $(BUILD_DIR)/left-debug -b $(BOARD_LEFT) -- \
			-DSHIELD="$(SHIELD_LEFT)" \
			-DCONFIG_ZMK_SPLIT_ROLE_CENTRAL=n \
			-DEXTRA_DTC_OVERLAY_FILE="$(PWD)/$(CONFIG_DIR)/eyelash_corne_left_debug.overlay" \
			-DZMK_CONFIG="$(PWD)/$(CONFIG_DIR)" \
			-DZMK_EXTRA_MODULES="$(EXTRA_MODULES)" \
			-DEXTRA_CONF_FILE="$(PWD)/$(CONFIG_DIR)/eyelash_corne_left_debug.conf" \
			$(CMAKE_EXTRA)
	@mkdir -p $(FIRMWARE)
	@cp $(BUILD_DIR)/left-debug/zephyr/zmk.uf2 $(FIRMWARE)/$(BOARD_LEFT)_debug.uf2
	@echo "✓ $(FIRMWARE)/$(BOARD_LEFT)_debug.uf2 (still pairs with dongle — USB console added for logging)"

build-right-debug:
	@echo "Building RIGHT half DEBUG (peripheral, USB console)..."
	@west build -p -s $(APP_SRC) -d $(BUILD_DIR)/right-debug -b $(BOARD_RIGHT) -- \
			-DSHIELD="$(SHIELD_RIGHT)" \
			-DEXTRA_DTC_OVERLAY_FILE="$(PWD)/$(CONFIG_DIR)/eyelash_corne_right_debug.overlay" \
			-DZMK_CONFIG="$(PWD)/$(CONFIG_DIR)" \
			-DZMK_EXTRA_MODULES="$(EXTRA_MODULES)" \
			-DEXTRA_CONF_FILE="$(PWD)/$(CONFIG_DIR)/eyelash_corne_right_debug.conf" \
			$(CMAKE_EXTRA)
	@mkdir -p $(FIRMWARE)
	@cp $(BUILD_DIR)/right-debug/zephyr/zmk.uf2 $(FIRMWARE)/$(BOARD_RIGHT)_debug.uf2
	@echo "✓ $(FIRMWARE)/$(BOARD_RIGHT)_debug.uf2 (still pairs with dongle — USB console added for logging)"

build-reset:
	@echo "Building settings reset..."
	@west build -p -s $(APP_SRC) -d $(BUILD_DIR)/reset -b $(BOARD_DONGLE) -- \
		-DSHIELD=settings_reset \
		-DZMK_CONFIG="$(PWD)/$(CONFIG_DIR)" \
		-DZMK_EXTRA_MODULES="$(EXTRA_MODULES)" \
		$(CMAKE_EXTRA)
	@mkdir -p $(FIRMWARE)
	@cp $(BUILD_DIR)/reset/zephyr/zmk.uf2 $(FIRMWARE)/settings_reset.uf2
	@echo "✓ $(FIRMWARE)/settings_reset.uf2"

# ── Clean ────────────────────────────────────────────────────────────────────
clean:
	@rm -rf $(BUILD_DIR)
	@echo "✓ Build artifacts removed"

pristine: clean
	@rm -rf .west zmk modules zmk-dongle-display .cache $(FIRMWARE)
	@echo "✓ Full reset. Run 'make init' to reinitialize."

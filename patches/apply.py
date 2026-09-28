#!/usr/bin/env python3
"""Apply local patches to west-managed dependencies after `west update'.

Patches are needed because:
1. Zephyr 3.5 kconfig.py treats upstream Kconfig warnings (LVGL, EC11)
   as fatal errors. We suppress them since they're in ZMK/Zephyr code.
2. ws2812-driver source files are maintained locally in patches/files/
   and copied over after `west update' overwrites them.

Run this after `make init' or whenever `west update' overwrites these files.
"""

import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCHES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "files")

# Simple find-replace patches
PATCHES = [
    {
        "file": os.path.join(ROOT, "zephyr", "scripts", "kconfig", "kconfig.py"),
        "desc": "Suppress upstream Kconfig warnings (LVGL, EC11, etc.)",
        "find": "            error_out = True\n",
        "replace": (
            "            # Don't error out on upstream Kconfig warnings (LVGL, EC11, etc.)\n"
            "            error_out = False  # patched\n"
        ),
        "already_patched_marker": "error_out = False  # patched",
    },
    {
        "file": os.path.join(ROOT, "zephyr", "drivers", "display", "ssd1306.c"),
        "desc": "Enable runtime 0/180 degree rotation for SSD1306/SH1106",
        "find": (
            "static int ssd1306_set_orientation(const struct device *dev,\n"
            "\t\t\t\t   const enum display_orientation\n"
            "\t\t\t\t   orientation)\n"
            "{\n"
            "\tLOG_ERR(\"Unsupported\");\n"
            "\treturn -ENOTSUP;\n"
            "}\n"
        ),
        "replace": (
            "static int ssd1306_set_orientation(const struct device *dev,\n"
            "\t\t\t\t   const enum display_orientation\n"
            "\t\t\t\t   orientation)\n"
            "{\n"
            "\tconst struct ssd1306_config *config = dev->config;\n"
            "\tbool rotated;\n\n"
            "\tif (orientation == DISPLAY_ORIENTATION_NORMAL) {\n"
            "\t\trotated = false;\n"
            "\t} else if (orientation == DISPLAY_ORIENTATION_ROTATED_180) {\n"
            "\t\trotated = true;\n"
            "\t} else {\n"
            "\t\tLOG_ERR(\"Unsupported orientation\");\n"
            "\t\treturn -ENOTSUP;\n"
            "\t}\n\n"
            "\tuint8_t cmd_buf[] = {\n"
            "\t\t(config->segment_remap != rotated ? SSD1306_SET_SEGMENT_MAP_REMAPED\n"
            "\t\t\t\t\t\t  : SSD1306_SET_SEGMENT_MAP_NORMAL),\n"
            "\t\t(config->com_invdir != rotated ? SSD1306_SET_COM_OUTPUT_SCAN_FLIPPED\n"
            "\t\t\t\t\t\t      : SSD1306_SET_COM_OUTPUT_SCAN_NORMAL),\n"
            "\t};\n\n"
            "\treturn ssd1306_write_bus(dev, cmd_buf, sizeof(cmd_buf), true);\n"
            "}\n"
        ),
        "already_patched_marker": "Unsupported orientation",
    },
    {
        "file": os.path.join(ROOT, "zmk", "app", "src", "split", "bluetooth", "central.c"),
        "desc": "Defer split GATT discovery until the peripheral reaches security L2",
        "find": (
            "    confirm_peripheral_slot_conn(conn);\n"
            "    split_central_process_connection(conn);\n"
            "    k_work_submit(&notify_status_work);\n"
        ),
        "replace": (
            "    confirm_peripheral_slot_conn(conn);\n"
            "    if (bt_conn_get_security(conn) < BT_SECURITY_L2) {\n"
            "        int err = bt_conn_set_security(conn, BT_SECURITY_L2);\n"
            "        if (err) {\n"
            "            LOG_ERR(\"Failed to start peripheral security (err %d)\", err);\n"
            "        }\n"
            "        start_scanning();\n"
            "    } else {\n"
            "        split_central_process_connection(conn);\n"
            "    }\n"
            "    k_work_submit(&notify_status_work);\n"
        ),
        "already_patched_marker": "Failed to start peripheral security",
    },
    {
        "file": os.path.join(ROOT, "zmk", "app", "src", "split", "bluetooth", "central.c"),
        "desc": "Start split discovery from the security-changed callback",
        "find": (
            "    struct peripheral_slot *slot = peripheral_slot_for_conn(conn);\n"
            "    if (!slot || !slot->selected_physical_layout_handle) {\n"
            "        return;\n"
            "    }\n\n"
            "    if (err > 0) {\n"
            "        LOG_DBG(\"Skipping updating the physical layout for peripheral with security error\");\n"
            "        return;\n"
            "    }\n\n"
            "    if (level < BT_SECURITY_L2) {\n"
            "        LOG_DBG(\"Skipping updating the physical layout for peripheral with insufficient security\");\n"
            "        return;\n"
            "    }\n\n"
            "    k_work_submit(&update_peripherals_selected_layouts_work);\n"
        ),
        "replace": (
            "    struct peripheral_slot *slot = peripheral_slot_for_conn(conn);\n"
            "    if (!slot) {\n"
            "        return;\n"
            "    }\n\n"
            "    if (err > 0) {\n"
            "        LOG_ERR(\"Peripheral security failed (err %d)\", err);\n"
            "        return;\n"
            "    }\n\n"
            "    if (level < BT_SECURITY_L2) {\n"
            "        LOG_DBG(\"Waiting for peripheral security level L2\");\n"
            "        return;\n"
            "    }\n\n"
            "    if (!slot->subscribe_params.value_handle) {\n"
            "        split_central_process_connection(conn);\n"
            "    } else if (slot->selected_physical_layout_handle) {\n"
            "        k_work_submit(&update_peripherals_selected_layouts_work);\n"
            "    }\n"
        ),
        "already_patched_marker": "Waiting for peripheral security level L2",
    },
    {
        "file": os.path.join(ROOT, "zmk", "app", "src", "split", "bluetooth", "central.c"),
        "desc": "Recover split peripherals from stale BLE bond keys",
        "find": (
            "    if (err > 0) {\n"
            "        LOG_ERR(\"Peripheral security failed (err %d)\", err);\n"
            "        return;\n"
            "    }\n"
        ),
        "replace": (
            "    if (err > 0) {\n"
            "        LOG_ERR(\"Peripheral security failed (err %d)\", err);\n"
            "        if (err == BT_SECURITY_ERR_AUTH_FAIL ||\n"
            "            err == BT_SECURITY_ERR_PIN_OR_KEY_MISSING) {\n"
            "            const bt_addr_le_t *peer = bt_conn_get_dst(conn);\n"
            "            LOG_WRN(\"Clearing stale peripheral bond and retrying pairing\");\n"
            "            int unpair_err = bt_unpair(BT_ID_DEFAULT, peer);\n"
            "            if (unpair_err) {\n"
            "                LOG_ERR(\"Failed to clear stale peripheral bond (err %d)\",\n"
            "                        unpair_err);\n"
            "            }\n"
            "        }\n"
            "        return;\n"
            "    }\n"
        ),
        "already_patched_marker": "Clearing stale peripheral bond and retrying pairing",
    },
    {
        "file": os.path.join(ROOT, "zmk", "app", "src", "split", "bluetooth", "central.c"),
        "desc": "Allow split peripherals without an encoder sensor characteristic",
        "find": (
            "#if ZMK_KEYMAP_HAS_SENSORS\n"
            "    subscribed = subscribed && slot->sensor_subscribe_params.value_handle;\n"
            "#endif /* ZMK_KEYMAP_HAS_SENSORS */\n\n"
        ),
        "replace": "",
        "already_patched_marker": (
            "slot->sensor_subscribe_params.value_handle = 0;\n"
            "#endif /* ZMK_KEYMAP_HAS_SENSORS */\n"
            "    slot->run_behavior_handle = 0;"
        ),
    },
    {
        "file": os.path.join(ROOT, "zmk", "app", "src", "split", "bluetooth", "central.c"),
        "desc": "Clear the split sensor handle when a peripheral disconnects",
        "find": (
            "    slot->subscribe_params.value_handle = 0;\n"
            "    slot->run_behavior_handle = 0;\n"
        ),
        "replace": (
            "    slot->subscribe_params.value_handle = 0;\n"
            "#if ZMK_KEYMAP_HAS_SENSORS\n"
            "    slot->sensor_subscribe_params.value_handle = 0;\n"
            "#endif /* ZMK_KEYMAP_HAS_SENSORS */\n"
            "    slot->run_behavior_handle = 0;\n"
        ),
        "already_patched_marker": (
            "slot->sensor_subscribe_params.value_handle = 0;\n"
            "#endif /* ZMK_KEYMAP_HAS_SENSORS */\n"
            "    slot->run_behavior_handle = 0;"
        ),
    },
    {
        "file": os.path.join(ROOT, "zmk", "app", "src", "split", "bluetooth", "central.c"),
        "desc": "Keep position and sensor CCC discovery state separate",
        "find": (
            "    struct bt_gatt_subscribe_params sensor_subscribe_params;\n"
            "    struct bt_gatt_discover_params sub_discover_params;\n"
        ),
        "replace": (
            "    struct bt_gatt_subscribe_params sensor_subscribe_params;\n"
            "    struct bt_gatt_discover_params sub_discover_params;\n"
            "    struct bt_gatt_discover_params sensor_sub_discover_params;\n"
        ),
        "already_patched_marker": "struct bt_gatt_discover_params sensor_sub_discover_params;",
    },
    {
        "file": os.path.join(ROOT, "zmk", "app", "src", "split", "bluetooth", "central.c"),
        "desc": "Use the dedicated sensor CCC discovery state",
        "find": (
            "            slot->sensor_subscribe_params.disc_params = "
            "&slot->sub_discover_params;\n"
        ),
        "replace": (
            "            slot->sensor_subscribe_params.disc_params = "
            "&slot->sensor_sub_discover_params;\n"
        ),
        "already_patched_marker": (
            "slot->sensor_subscribe_params.disc_params = "
            "&slot->sensor_sub_discover_params;"
        ),
    },
    {
        "file": os.path.join(
            ROOT,
            "zmk-dongle-display",
            "boards",
            "shields",
            "dongle_display",
            "widgets",
            "battery_status.c",
        ),
        "desc": "Add a charging flag to the dongle battery widget state",
        "find": "};\n\nstruct battery_object {\n",
        "replace": "};\n\n#define ZMK_BATTERY_CHARGING_FLAG BIT(7)\n\nstruct battery_object {\n",
        "already_patched_marker": "#define ZMK_BATTERY_CHARGING_FLAG BIT(7)",
    },
    {
        "file": os.path.join(
            ROOT,
            "zmk-dongle-display",
            "boards",
            "shields",
            "dongle_display",
            "widgets",
            "battery_status.c",
        ),
        "desc": "Show charging instead of a false 100 percent",
        "find": "    lv_label_set_text_fmt(label, \"%4u%%\", state.level);\n",
        "replace": (
            "    if (state.usb_present) {\n"
            "        if (state.level > 0) {\n"
            "            lv_label_set_text_fmt(label, \"%3u%%+\", state.level);\n"
            "        } else {\n"
            "            lv_label_set_text(label, \" CHG \" );\n"
            "        }\n"
            "    } else {\n"
            "        lv_label_set_text_fmt(label, \"%4u%%\", state.level);\n"
            "    }\n"
        ),
        "already_patched_marker": 'lv_label_set_text_fmt(label, "%3u%%+", state.level);',
    },
    {
        "file": os.path.join(
            ROOT,
            "zmk-dongle-display",
            "boards",
            "shields",
            "dongle_display",
            "widgets",
            "battery_status.c",
        ),
        "desc": "Decode peripheral charging state on the dongle",
        "find": (
            "    const struct zmk_peripheral_battery_state_changed *ev = as_zmk_peripheral_battery_state_changed(eh);\n"
            "    return (struct battery_state){\n"
            "        .source = ev->source + SOURCE_OFFSET,\n"
            "        .level = ev->state_of_charge,\n"
            "    };\n"
        ),
        "replace": (
            "    const struct zmk_peripheral_battery_state_changed *ev = as_zmk_peripheral_battery_state_changed(eh);\n"
            "    bool charging = (ev->state_of_charge & ZMK_BATTERY_CHARGING_FLAG) != 0;\n"
            "    return (struct battery_state){\n"
            "        .source = ev->source + SOURCE_OFFSET,\n"
            "        .level = ev->state_of_charge & 0x7F,\n"
            "        .usb_present = charging,\n"
            "    };\n"
        ),
        "already_patched_marker": ".level = ev->state_of_charge & 0x7F,",
    },
]

# File mappings: (source in patches/files/, destination in project)
# Source paths are relative to patches/files/
# Destination paths are relative to ROOT
FILE_COPIES = [
    # ws2812-driver module (west-managed, overwritten on `west update')
    ("zmk-ws2812-driver/src/widget.c", "modules/zmk-ws2812-driver/src/widget.c"),
    (
        "zmk-ws2812-driver/src/behaviors/behavior_ws2812_widget.c",
        "modules/zmk-ws2812-driver/src/behaviors/behavior_ws2812_widget.c",
    ),
    (
        "zmk-ws2812-driver/include/zmk_ws2812_widget/widget.h",
        "modules/zmk-ws2812-driver/include/zmk_ws2812_widget/widget.h",
    ),
    (
        "zmk-ws2812-driver/include/zmk/events/ws2812_hints_state_changed.h",
        "modules/zmk-ws2812-driver/include/zmk/events/ws2812_hints_state_changed.h",
    ),
    (
        "zmk-ws2812-driver/src/events/ws2812_hints_state_changed.c",
        "modules/zmk-ws2812-driver/src/events/ws2812_hints_state_changed.c",
    ),
    (
        "zmk-dongle-display/boards/shields/dongle_display/CMakeLists.txt",
        "zmk-dongle-display/boards/shields/dongle_display/CMakeLists.txt",
    ),
    (
        "zmk-dongle-display/boards/shields/dongle_display/custom_status_screen.c",
        "zmk-dongle-display/boards/shields/dongle_display/custom_status_screen.c",
    ),
    (
        "zmk-dongle-display/boards/shields/dongle_display/widgets/led_hints_status.c",
        "zmk-dongle-display/boards/shields/dongle_display/widgets/led_hints_status.c",
    ),
    (
        "zmk-dongle-display/boards/shields/dongle_display/widgets/led_hints_status.h",
        "zmk-dongle-display/boards/shields/dongle_display/widgets/led_hints_status.h",
    ),
    (
        "zmk-ws2812-driver/dts/behaviors/ws2812_widget.dtsi",
        "modules/zmk-ws2812-driver/dts/behaviors/ws2812_widget.dtsi",
    ),
    ("zmk-ws2812-driver/Kconfig", "modules/zmk-ws2812-driver/Kconfig"),
    (
        "zmk-ws2812-driver/CMakeLists.txt",
        "modules/zmk-ws2812-driver/CMakeLists.txt",
    ),
    # eyelash_corne_module (also managed locally but consistency)
    (
        "eyelash_corne_module/dts/behaviors/led_test.dtsi",
        "eyelash_corne_module/dts/behaviors/led_test.dtsi",
    ),
    (
        "eyelash_corne_module/dts/behaviors/led_set.dtsi",
        "eyelash_corne_module/dts/behaviors/led_set.dtsi",
    ),
    (
        "eyelash_corne_module/dts/bindings/behaviors/zmk,behavior-led-test.yaml",
        "eyelash_corne_module/dts/bindings/behaviors/zmk,behavior-led-test.yaml",
    ),
    (
        "eyelash_corne_module/dts/bindings/behaviors/zmk,behavior-led-set.yaml",
        "eyelash_corne_module/dts/bindings/behaviors/zmk,behavior-led-set.yaml",
    ),
    (
        "eyelash_corne_module/src/behaviors/behavior_led_test.c",
        "eyelash_corne_module/src/behaviors/behavior_led_test.c",
    ),
    (
        "eyelash_corne_module/src/behaviors/behavior_led_set.c",
        "eyelash_corne_module/src/behaviors/behavior_led_set.c",
    ),
    (
        "eyelash_corne_module/src/behaviors/behavior_layer_relay.c",
        "eyelash_corne_module/src/behaviors/behavior_layer_relay.c",
    ),
]

GIT_PATCHES = [
    (
        "zephyr",
        "zephyr/charging-state.patch",
        "Allow the private split charging flag through the internal BAS link",
    ),
    (
        "zmk",
        "zmk/remote-sensors.patch",
        "Compile keymap handling for remote split sensors without a fake local device",
    ),
    (
        "zmk",
        "zmk/peripheral-ws2812.patch",
        "Handle relayed WS2812 behaviors directly on split peripherals",
    ),
    (
        "zmk",
        "zmk/charging-state.patch",
        "Carry peripheral charging state to the dongle and clamp host BAS values",
    ),
]


def patch_file(patch):
    """Apply a simple find-replace patch to a file."""
    path = patch["file"]
    relpath = os.path.relpath(path, ROOT)

    if not os.path.exists(path):
        print(f"  SKIP (file not found): {relpath}")
        return False

    with open(path, "r", encoding="utf-8") as f:
        content = f.read()

    if patch["already_patched_marker"] in content:
        print(f"  OK (already patched): {relpath}")
        return True

    if patch["find"] not in content:
        print(f"  WARN (pattern not found): {relpath}")
        return False

    content = content.replace(patch["find"], patch["replace"], 1)

    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"  PATCHED: {relpath}")
    return True


def copy_if_different(src, dst):
    """Copy file only if content differs."""
    if os.path.exists(dst):
        with open(src, "rb") as f:
            src_content = f.read()
        with open(dst, "rb") as f:
            dst_content = f.read()
        if src_content == dst_content:
            print(f"  OK (already up to date): {os.path.relpath(dst, ROOT)}")
            return True

    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    print(f"  COPIED: {os.path.relpath(dst, ROOT)}")
    return True


def apply_git_patch(repo_rel, patch_rel, desc):
    repo = os.path.join(ROOT, repo_rel)
    patch = os.path.join(PATCHES_DIR, patch_rel)
    rel_patch = os.path.relpath(patch, ROOT)

    if not os.path.exists(repo) or not os.path.exists(patch):
        print(f"  SKIP (missing repo or patch): {rel_patch}")
        return False

    reverse_check = subprocess.run(
        ["git", "apply", "--reverse", "--check", patch], cwd=repo, capture_output=True
    )
    if reverse_check.returncode == 0:
        print(f"  OK (already patched): {desc}")
        return True

    check = subprocess.run(["git", "apply", "--check", patch], cwd=repo, capture_output=True)
    if check.returncode != 0:
        print(f"  WARN (patch does not apply): {rel_patch}")
        return False

    subprocess.run(["git", "apply", patch], cwd=repo, check=True)
    print(f"  PATCHED: {desc}")
    return True


def main():
    print("Applying local patches...")
    all_ok = True

    # Simple find-replace patches
    for p in PATCHES:
        if not patch_file(p):
            all_ok = False

    for repo_rel, patch_rel, desc in GIT_PATCHES:
        if not apply_git_patch(repo_rel, patch_rel, desc):
            all_ok = False

    # File copies from patches/files/
    print()
    print("Copying local source files...")
    for src_rel, dst_rel in FILE_COPIES:
        src = os.path.join(PATCHES_DIR, src_rel)
        dst = os.path.join(ROOT, dst_rel)

        if not os.path.exists(src):
            print(f"  SKIP (source not found): {src_rel}")
            all_ok = False
            continue

        copy_if_different(src, dst)

    print()
    if all_ok:
        print("✓ All patches applied successfully.")
    else:
        print("⚠ Some patches could not be applied. Build may fail.")
        print("  This can happen if upstream code changed since the patch was written.")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())

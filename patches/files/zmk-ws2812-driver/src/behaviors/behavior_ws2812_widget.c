#define DT_DRV_COMPAT zmk_behavior_ws2812_widget

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/events/ws2812_hints_state_changed.h>

#include <zmk_ws2812_widget/widget.h>

#if DT_HAS_CHOSEN(zephyr_display)
#include <zephyr/drivers/display.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zmk/display.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define WS2812_WIDGET_COMMAND_TOGGLE 254
#define WS2812_WIDGET_COMMAND_DISPLAY_ROTATE 253

static bool layer_hints_enabled = false;

bool zmk_ws2812_hints_enabled(void) { return layer_hints_enabled; }

#if DT_HAS_CHOSEN(zephyr_display) && (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
// Runtime 0/180 rotation of the dongle OLED. Only the central (dongle) handles
// it: the halves' nice!view doesn't support orientation changes.
#define DISPLAY_ROTATION_SUPPORTED 1

// Rotation is persisted in settings and re-applied on boot, so the dongle keeps
// the orientation it had before a reboot or re-plug.
#define DISPLAY_ROTATION_BOOT_POLL_MS 100
#define DISPLAY_ROTATION_BOOT_MAX_WAIT_MS 10000
// Debounce flash writes when the key is pressed several times in a row.
#define DISPLAY_ROTATION_SAVE_DELAY_MS 2000

static bool display_rotated;

static void rotate_display_work_cb(struct k_work *work) {
    const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    enum display_orientation orientation = display_rotated
                                               ? DISPLAY_ORIENTATION_ROTATED_180
                                               : DISPLAY_ORIENTATION_NORMAL;
    int err = display_set_orientation(display, orientation);

    if (err) {
        LOG_ERR("Failed to rotate display: %d", err);
    }
}

K_WORK_DEFINE(rotate_display_work, rotate_display_work_cb);

#if IS_ENABLED(CONFIG_SETTINGS)
static void save_display_rotation_work_cb(struct k_work *work) {
    int err = settings_save_one("ws2812_wdg/rotated", &display_rotated, sizeof(display_rotated));
    if (err < 0) {
        LOG_ERR("Failed to save display rotation (err %d)", err);
    }
}

static K_WORK_DELAYABLE_DEFINE(save_display_rotation_work, save_display_rotation_work_cb);

static int display_rotation_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                         void *cb_arg) {
    const char *next;
    if (settings_name_steq(name, "rotated", &next) && !next) {
        if (len != sizeof(display_rotated)) {
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, &display_rotated, sizeof(display_rotated));
        return MIN(rc, 0);
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(ws2812_wdg, "ws2812_wdg", NULL, display_rotation_settings_set, NULL,
                               NULL);
#endif // IS_ENABLED(CONFIG_SETTINGS)

// The display driver re-initializes the panel in its default orientation on
// boot, so the saved rotation has to be sent again once the display is up.
// Settings are loaded before the display is initialized, so by then
// display_rotated holds the saved value.
static void restore_display_rotation_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(restore_display_rotation_work, restore_display_rotation_work_cb);

static void restore_display_rotation_work_cb(struct k_work *work) {
    if (!zmk_display_is_initialized()) {
        if (k_uptime_get() < DISPLAY_ROTATION_BOOT_MAX_WAIT_MS) {
            k_work_reschedule(&restore_display_rotation_work, K_MSEC(DISPLAY_ROTATION_BOOT_POLL_MS));
        }
        return;
    }

    if (display_rotated) {
        k_work_submit_to_queue(zmk_display_work_q(), &rotate_display_work);
    }
}

static int restore_display_rotation_init(void) {
    k_work_schedule(&restore_display_rotation_work, K_MSEC(DISPLAY_ROTATION_BOOT_POLL_MS));
    return 0;
}

SYS_INIT(restore_display_rotation_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif

struct behavior_ws2812_wdg_config {
    bool indicate_battery;
    bool indicate_connectivity;
    bool indicate_layer;
};

static int __maybe_unused behavior_ws2812_wdg_init(const struct device *dev) { return 0; }

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    if (binding->param1 == WS2812_WIDGET_COMMAND_DISPLAY_ROTATE) {
#if defined(DISPLAY_ROTATION_SUPPORTED)
        display_rotated = !display_rotated;
        if (zmk_display_is_initialized()) {
            k_work_submit_to_queue(zmk_display_work_q(), &rotate_display_work);
        }
#if IS_ENABLED(CONFIG_SETTINGS)
        k_work_reschedule(&save_display_rotation_work, K_MSEC(DISPLAY_ROTATION_SAVE_DELAY_MS));
#endif
#endif
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (binding->param1 == WS2812_WIDGET_COMMAND_TOGGLE) {
        layer_hints_enabled = !layer_hints_enabled;
        LOG_INF("ws2812_wdg behavior: layer hints %s",
                layer_hints_enabled ? "enabled" : "disabled");
        raise_ws2812_hints_state_changed(layer_hints_enabled);
#if IS_ENABLED(CONFIG_WS2812_WIDGET)
        zmk_ws2812_widget_toggle();
#endif
        return ZMK_BEHAVIOR_OPAQUE;
    }

#if IS_ENABLED(CONFIG_WS2812_WIDGET)
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_ws2812_wdg_config *cfg = dev->config;

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY)
    if (cfg->indicate_battery) {
        ws2812_indicate_battery();
    }
#endif
#if (IS_ENABLED(CONFIG_ZMK_USB) || IS_ENABLED(CONFIG_ZMK_BLE)) && IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_CONNECTIVITY)
    if (cfg->indicate_connectivity) {
        ws2812_indicate_connectivity();
    }
#endif
#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
    if (cfg->indicate_layer) {
        if (binding->param1 < WS2812_WIDGET_COMMAND_TOGGLE) {
            // Specific layer number provided via behavior relay —
            // works on all devices (central and peripheral)
            LOG_INF("ws2812_wdg behavior: param1=%d (relay), calling ws2812_set_layer_color", binding->param1);
            ws2812_set_layer_color(binding->param1);
        } else {
            // Auto-detect current layer (param1=255 / 0xFF) —
            // uses zmk_keymap_highest_layer_active() on central,
            // uses stored current_layer on peripheral
            LOG_INF("ws2812_wdg behavior: param1=0xFF (auto-detect), calling ws2812_indicate_layer");
            ws2812_indicate_layer();
        }
    }
#endif
#endif // IS_ENABLED(CONFIG_WS2812_WIDGET)

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api __maybe_unused behavior_ws2812_wdg_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

#define WS2812_WDG_INST(n)                                                                          \
    static struct behavior_ws2812_wdg_config behavior_ws2812_wdg_config_##n = {                     \
        .indicate_battery = DT_INST_PROP(n, indicate_battery),                                      \
        .indicate_connectivity = DT_INST_PROP(n, indicate_connectivity),                            \
        .indicate_layer = DT_INST_PROP(n, indicate_layer),                                          \
    };                                                                                              \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_ws2812_wdg_init, NULL, NULL, &behavior_ws2812_wdg_config_##n, \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                       \
                            &behavior_ws2812_wdg_driver_api);

DT_INST_FOREACH_STATUS_OKAY(WS2812_WDG_INST)

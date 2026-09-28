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
#include <zmk/display.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define WS2812_WIDGET_COMMAND_TOGGLE 254
#define WS2812_WIDGET_COMMAND_DISPLAY_ROTATE 253

static bool layer_hints_enabled = false;

bool zmk_ws2812_hints_enabled(void) { return layer_hints_enabled; }

#if DT_HAS_CHOSEN(zephyr_display)
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
#if DT_HAS_CHOSEN(zephyr_display)
        display_rotated = !display_rotated;
        if (zmk_display_is_initialized()) {
            k_work_submit_to_queue(zmk_display_work_q(), &rotate_display_work);
        }
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

#define DT_DRV_COMPAT zmk_behavior_led_test

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>

#include <zmk_ws2812_widget/widget.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Color code mapping: 0=off, 1=red, 2=green, 3=blue, 4=white,
//                     5=yellow, 6=cyan, 7=magenta, 8=orange
static void color_code_to_rgb(uint8_t code, uint8_t *r, uint8_t *g, uint8_t *b) {
    switch (code) {
    case 1: *r = 255; *g = 0;   *b = 0;   break; // red
    case 2: *r = 0;   *g = 255; *b = 0;   break; // green
    case 3: *r = 0;   *g = 0;   *b = 255; break; // blue
    case 4: *r = 255; *g = 255; *b = 255; break; // white
    case 5: *r = 255; *g = 255; *b = 0;   break; // yellow
    case 6: *r = 0;   *g = 255; *b = 255; break; // cyan
    case 7: *r = 255; *g = 0;   *b = 255; break; // magenta
    case 8: *r = 255; *g = 165; *b = 0;   break; // orange
    default: *r = 0;  *g = 0;   *b = 0;   break; // off
    }
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
#if IS_ENABLED(CONFIG_WS2812_WIDGET)
    uint8_t led_index = binding->param1;
    uint8_t color_code = binding->param2;
    uint8_t r, g, b;
    color_code_to_rgb(color_code, &r, &g, &b);
    LOG_INF("LED test: index=%d color=%d → rgb(%d,%d,%d)", led_index, color_code, r, g, b);
    ws2812_test_led(led_index, r, g, b);
#else
    LOG_WRN("LED test behavior pressed but WS2812_WIDGET is disabled");
#endif
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api __maybe_unused behavior_led_test_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define LED_TEST_INST(n)                                                                            \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL,                                             \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                        \
                            &behavior_led_test_driver_api);

DT_INST_FOREACH_STATUS_OKAY(LED_TEST_INST)

#include <zephyr/kernel.h>

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/ws2812_hints_state_changed.h>

#include "led_hints_status.h"

struct led_hints_status_state {
    bool enabled;
};

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

static void set_led_hints_status(lv_obj_t *label, struct led_hints_status_state state) {
    lv_label_set_text(label, state.enabled ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
}

static void led_hints_status_update_cb(struct led_hints_status_state state) {
    struct zmk_widget_led_hints_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        set_led_hints_status(widget->obj, state);
    }
}

static struct led_hints_status_state led_hints_status_get_state(const zmk_event_t *eh) {
    const struct zmk_ws2812_hints_state_changed *ev =
        as_zmk_ws2812_hints_state_changed(eh);

    return (struct led_hints_status_state){
        .enabled = ev != NULL ? ev->enabled : zmk_ws2812_hints_enabled(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_led_hints_status, struct led_hints_status_state,
                            led_hints_status_update_cb, led_hints_status_get_state)

ZMK_SUBSCRIPTION(widget_led_hints_status, zmk_ws2812_hints_state_changed);

int zmk_widget_led_hints_status_init(struct zmk_widget_led_hints_status *widget,
                                     lv_obj_t *parent) {
    widget->obj = lv_label_create(parent);
    lv_obj_set_style_text_font(widget->obj, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(widget->obj, 0, LV_PART_MAIN);
    sys_slist_append(&widgets, &widget->node);
    widget_led_hints_status_init();
    return 0;
}

lv_obj_t *zmk_widget_led_hints_status_obj(struct zmk_widget_led_hints_status *widget) {
    return widget->obj;
}

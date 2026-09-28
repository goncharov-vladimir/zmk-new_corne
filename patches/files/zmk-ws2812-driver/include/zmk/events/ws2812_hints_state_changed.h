#pragma once

#include <stdbool.h>
#include <zmk/event_manager.h>

struct zmk_ws2812_hints_state_changed {
    bool enabled;
};

ZMK_EVENT_DECLARE(zmk_ws2812_hints_state_changed);

bool zmk_ws2812_hints_enabled(void);

static inline int raise_ws2812_hints_state_changed(bool enabled) {
    return raise_zmk_ws2812_hints_state_changed(
        (struct zmk_ws2812_hints_state_changed){.enabled = enabled});
}

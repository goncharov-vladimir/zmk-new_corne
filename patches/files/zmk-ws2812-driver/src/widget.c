/*
 * WS2812 LED widget for Eyelash Corne — per-key RGB with layer masks.
 *
 * Combines the animation engine from ZMK's rgb_underglow (solid, breathe,
 * spectrum, swirl) with per-LED masking so effects only run on the LEDs
 * that belong to the current layer's mask.
 *
 * Key design choices (following rgb_underglow.c):
 *  - Static pixels[] buffer written by effect functions, then flushed once
 *    with led_strip_update_rgb()
 *  - HSB colour space for smooth colour manipulation
 *  - k_timer + k_work for 50 ms animation tick
 *  - Layer mask is applied per-pixel: masked LEDs show the effect,
 *    unmasked LEDs are forced to black
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <math.h>
#include <stdlib.h>

#if defined(CONFIG_BOARD_EYELASH_CORNE_LEFT) || defined(CONFIG_BOARD_EYELASH_CORNE_RIGHT)
#include <hal/nrf_gpio.h>
#include <cmsis_core.h>
#endif

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/behavior.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/workqueue.h>
#include <zmk/rgb_underglow.h>
#include <drivers/ext_power.h>

#if __has_include(<zmk/split/central.h>)
#include <zmk/split/central.h>
#else
#include <zmk/split/bluetooth/central.h>
#endif

#include <zephyr/logging/log.h>
#include <zmk_ws2812_widget/widget.h>
#include <ws2812_led_layouts.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ── Devicetree ─────────────────────────────────────────────────────────── */

#define WS2812_STRIP_NODE DT_CHOSEN(zmk_ws2812_widget)

#if !DT_NODE_EXISTS(WS2812_STRIP_NODE)
#error "WS2812 widget chosen node zmk,ws2812-widget not found"
#endif

static const struct device *led_strip = DEVICE_DT_GET(WS2812_STRIP_NODE);
#define STRIP_NUM_PIXELS DT_PROP(WS2812_STRIP_NODE, chain_length)

/* ── HSB / colour helpers (from rgb_underglow.c) ──────────────────────── */

#define HUE_MAX 360
#define SAT_MAX 100
#define BRT_MAX 100

/* struct zmk_led_hsb is defined in <zmk/rgb_underglow.h> */

static struct zmk_led_hsb hsb_scale_min_max(struct zmk_led_hsb hsb) {
    hsb.b = CONFIG_WS2812_WIDGET_BRT_MIN +
             (CONFIG_WS2812_WIDGET_BRT_MAX - CONFIG_WS2812_WIDGET_BRT_MIN) * hsb.b / BRT_MAX;
    return hsb;
}

static struct zmk_led_hsb hsb_scale_zero_max(struct zmk_led_hsb hsb) {
    hsb.b = hsb.b * CONFIG_WS2812_WIDGET_BRT_MAX / BRT_MAX;
    return hsb;
}

static struct led_rgb hsb_to_rgb(struct zmk_led_hsb hsb) {
    float r = 0, g = 0, b = 0;
    uint8_t i = hsb.h / 60;
    float v = hsb.b / ((float)BRT_MAX);
    float s = hsb.s / ((float)SAT_MAX);
    float f = hsb.h / ((float)HUE_MAX) * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);

    switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    case 5: r = v; g = p; b = q; break;
    }
    return (struct led_rgb){r : r * 255, g : g * 255, b : b * 255};
}

static struct led_rgb hex_to_rgb(uint32_t hex_color) {
    return (struct led_rgb){
        .r = (hex_color >> 16) & 0xFF,
        .g = (hex_color >> 8) & 0xFF,
        .b = hex_color & 0xFF
    };
}

/* ── Animation state ───────────────────────────────────────────────────── */

enum ws2812_effect {
    WS2812_EFFECT_SOLID,
    WS2812_EFFECT_BREATHE,
    WS2812_EFFECT_SPECTRUM,
    WS2812_EFFECT_SWIRL,
    WS2812_EFFECT_NUMBER
};

struct ws2812_state {
    struct zmk_led_hsb color;       /* Current HSB colour (from layer) */
    uint8_t  animation_speed;       /* 1-5 */
    uint8_t  current_effect;         /* enum ws2812_effect */
    uint16_t animation_step;        /* Running counter for animations */
    bool     on;                    /* LED strip powered on */
};

static struct ws2812_state state;

static struct led_rgb pixels[STRIP_NUM_PIXELS];  /* Static frame buffer */
static uint32_t current_layer = 0;
static bool initialized = false;
static bool test_mode_active = false;  /* LED test: suppress animation tick */
static bool manual_mode_active = false; /* Manual pixel control: suppress animation tick */

static K_MUTEX_DEFINE(led_mutex);

/* ── External power ────────────────────────────────────────────────────── */

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
static const struct device *const ext_power_dev =
    DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif

/* ── Layer colour / mask lookup ───────────────────────────────────────── */

static struct led_rgb get_layer_color(uint8_t layer) {
    switch (layer) {
    case 0: return hex_to_rgb(CONFIG_WS2812_WIDGET_LAYER_0_COLOR);
    case 1: return hex_to_rgb(CONFIG_WS2812_WIDGET_LAYER_1_COLOR);
    case 2: return hex_to_rgb(CONFIG_WS2812_WIDGET_LAYER_2_COLOR);
    case 3: return hex_to_rgb(CONFIG_WS2812_WIDGET_LAYER_3_COLOR);
    case 4: return hex_to_rgb(CONFIG_WS2812_WIDGET_LAYER_4_COLOR);
    case 5: return hex_to_rgb(CONFIG_WS2812_WIDGET_LAYER_5_COLOR);
    case 6: return hex_to_rgb(CONFIG_WS2812_WIDGET_LAYER_6_COLOR);
    default: return hex_to_rgb(CONFIG_WS2812_WIDGET_COLOR_WHITE);
    }
}

static uint32_t get_layer_mask(uint8_t layer) {
    if (layer >= WS2812_LAYER_COUNT) {
        return 0;
    }

    uint32_t mask = 0;
    for (int group = 0; group < WS2812_GROUP_COUNT; group++) {
        if (ws2812_layer_group_colors[layer][group] != LED_OFF) {
            mask |= BIT(group);
        }
    }
    return mask;
}

static void render_layer_layout(uint8_t layer) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){0, 0, 0};
    }

    if (layer >= WS2812_LAYER_COUNT) {
        return;
    }

    for (int group = 0; group < WS2812_GROUP_COUNT && group < STRIP_NUM_PIXELS; group++) {
        uint32_t color = ws2812_layer_group_colors[layer][group];
        if (color != LED_OFF) {
            pixels[group] = hex_to_rgb(color);
        }
    }
}

/* ── Strip update helper (mutex-protected) ─────────────────────────────── */

#if defined(CONFIG_BOARD_EYELASH_CORNE_LEFT) || defined(CONFIG_BOARD_EYELASH_CORNE_RIGHT)
#define LED_DATA_PIN NRF_GPIO_PIN_MAP(1, 12)
#define LED_DATA_MASK BIT(12)
#define LED_BIT_CYCLES 80U
#define LED_T0H_CYCLES 22U
#define LED_T1H_CYCLES 45U

static inline void eyelash_wait_until(uint32_t start, uint32_t cycles) {
    while ((uint32_t)(DWT->CYCCNT - start) < cycles) {
        __NOP();
    }
}

static void eyelash_send_byte(uint8_t value) {
    for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
        uint32_t start = DWT->CYCCNT;
        NRF_P1->OUTSET = LED_DATA_MASK;
        eyelash_wait_until(start, (value & mask) ? LED_T1H_CYCLES : LED_T0H_CYCLES);
        NRF_P1->OUTCLR = LED_DATA_MASK;
        eyelash_wait_until(start, LED_BIT_CYCLES);
    }
}

static int eyelash_gpio_flush(void) {
    nrf_gpio_cfg_output(LED_DATA_PIN);
    nrf_gpio_pin_clear(LED_DATA_PIN);
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    unsigned int key = irq_lock();
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        eyelash_send_byte(pixels[i].g);
        eyelash_send_byte(pixels[i].r);
        eyelash_send_byte(pixels[i].b);
    }
    NRF_P1->OUTCLR = LED_DATA_MASK;
    irq_unlock(key);
    k_busy_wait(120);
    return 0;
}
#endif

static int led_strip_flush(void) {
    k_mutex_lock(&led_mutex, K_FOREVER);
#if defined(CONFIG_BOARD_EYELASH_CORNE_LEFT) || defined(CONFIG_BOARD_EYELASH_CORNE_RIGHT)
    int rc = eyelash_gpio_flush();
#else
    int rc = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
#endif
    k_mutex_unlock(&led_mutex);
    if (rc != 0) {
        LOG_ERR("led_strip_update_rgb failed: %d", rc);
    }
    return rc;
}

/* ── Effect rendering (same pattern as rgb_underglow.c) ──────────────── */

static void ws2812_effect_solid(void) {
    struct led_rgb rgb = hsb_to_rgb(hsb_scale_min_max(state.color));
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = rgb;
    }
}

static void ws2812_effect_breathe(void) {
    struct zmk_led_hsb hsb = state.color;
    hsb.b = abs(state.animation_step - 1200) / 12;
    struct led_rgb rgb = hsb_to_rgb(hsb_scale_zero_max(hsb));
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = rgb;
    }
    state.animation_step += state.animation_speed * 10;
    if (state.animation_step > 2400) {
        state.animation_step = 0;
    }
}

static void ws2812_effect_spectrum(void) {
    struct zmk_led_hsb hsb = state.color;
    hsb.h = state.animation_step;
    struct led_rgb rgb = hsb_to_rgb(hsb_scale_min_max(hsb));
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = rgb;
    }
    state.animation_step += state.animation_speed;
    state.animation_step = state.animation_step % HUE_MAX;
}

static void ws2812_effect_swirl(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = (HUE_MAX / STRIP_NUM_PIXELS * i + state.animation_step) % HUE_MAX;
        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }
    state.animation_step += state.animation_speed * 2;
    state.animation_step = state.animation_step % HUE_MAX;
}

/* ── Animation tick: render current effect, apply mask, flush ────────── */

static uint8_t last_logged_layer = 0xFF; /* force first-tick log */

static void ws2812_tick(struct k_work *work) {
    ARG_UNUSED(work);

    if (!state.on) {
        return;
    }

    /* LED test mode: animation tick suppressed so test LED stays visible */
    if (test_mode_active) {
        return;
    }

    /* Render the six physical group colors from config/ws2812_led_layouts.h. */
    render_layer_layout(current_layer);
    uint32_t mask = get_layer_mask(current_layer);

    /* Log mask application once per layer change (not every 50ms tick) */
    if (current_layer != last_logged_layer) {
        int lit_count = 0;
        for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
            if ((mask >> i) & 1) lit_count++;
        }
        LOG_INF("tick: layer %d -> mask=0x%08X (%d/%d LEDs lit)", current_layer, mask,
                lit_count, STRIP_NUM_PIXELS);
        last_logged_layer = current_layer;
    }

    /* Линия данных электрически маргинальна: каждая передача тёмного кадра —
     * шанс глитча (вспышка случайного диода). Если кадр полностью чёрный,
     * шлём его 3 раза (надёжно гасим) и перестаём дёргать линию до смены
     * состояния. Нет передач — нет вспышек. */
    bool frame_dark = true;
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        if (pixels[i].r || pixels[i].g || pixels[i].b) {
            frame_dark = false;
            break;
        }
    }
    static uint8_t dark_streak;
    if (frame_dark) {
        if (dark_streak >= 3) {
            return; /* лента уже погашена — не трогаем линию */
        }
        dark_streak++;
    } else {
        dark_streak = 0;
    }

    led_strip_flush();
}

K_WORK_DEFINE(tick_work, ws2812_tick);

static void ws2812_tick_handler(struct k_timer *timer) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &tick_work);
}

K_TIMER_DEFINE(underglow_tick, ws2812_tick_handler, NULL);

/* ── Public API: effect selection (matching rgb_underglow API) ────────── */

int zmk_ws2812_widget_select_effect(int effect) {
    if (effect < 0 || effect >= WS2812_EFFECT_NUMBER) {
        return -EINVAL;
    }
    state.current_effect = effect;
    state.animation_step = 0;
    return 0;
}

int zmk_ws2812_widget_cycle_effect(int direction) {
    return zmk_ws2812_widget_select_effect(
        (state.current_effect + WS2812_EFFECT_NUMBER + direction) % WS2812_EFFECT_NUMBER);
}

int zmk_ws2812_widget_set_hsb(struct zmk_led_hsb color) {
    if (color.h > HUE_MAX || color.s > SAT_MAX || color.b > BRT_MAX) {
        return -ENOTSUP;
    }
    state.color = color;
    return 0;
}

struct zmk_led_hsb zmk_ws2812_widget_calc_hue(int direction) {
    struct zmk_led_hsb color = state.color;
    color.h += HUE_MAX + (direction * 5);
    color.h %= HUE_MAX;
    return color;
}

struct zmk_led_hsb zmk_ws2812_widget_calc_sat(int direction) {
    struct zmk_led_hsb color = state.color;
    int s = color.s + (direction * 10);
    color.s = (s < 0) ? 0 : ((s > SAT_MAX) ? SAT_MAX : s);
    return color;
}

struct zmk_led_hsb zmk_ws2812_widget_calc_brt(int direction) {
    struct zmk_led_hsb color = state.color;
    int b = color.b + (direction * 10);
    color.b = (b < 0) ? 0 : ((b > BRT_MAX) ? BRT_MAX : b);
    return color;
}

int zmk_ws2812_widget_change_hue(int direction) {
    state.color = zmk_ws2812_widget_calc_hue(direction);
    return 0;
}

int zmk_ws2812_widget_change_sat(int direction) {
    state.color = zmk_ws2812_widget_calc_sat(direction);
    return 0;
}

int zmk_ws2812_widget_change_brt(int direction) {
    state.color = zmk_ws2812_widget_calc_brt(direction);
    return 0;
}

int zmk_ws2812_widget_change_spd(int direction) {
    if (state.animation_speed == 1 && direction < 0) {
        return 0;
    }
    state.animation_speed += direction;
    if (state.animation_speed > 5) {
        state.animation_speed = 5;
    }
    return 0;
}

int zmk_ws2812_widget_on(void) {
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
    if (ext_power_dev != NULL) {
        ext_power_enable(ext_power_dev);
    }
#endif
    state.on = true;
    state.animation_step = 0;
    k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));
    return 0;
}

int zmk_ws2812_widget_off(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){0, 0, 0};
    }
    led_strip_flush();
    k_timer_stop(&underglow_tick);
    state.on = false;
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
    if (ext_power_dev != NULL) {
        ext_power_disable(ext_power_dev);
    }
#endif
    return 0;
}

int zmk_ws2812_widget_toggle(void) {
    return state.on ? zmk_ws2812_widget_off() : zmk_ws2812_widget_on();
}

/* ── Set layer colour + mask immediately (no animation) ──────────────── */

static void apply_layer_color_immediate(uint8_t layer) {
    struct led_rgb rgb = get_layer_color(layer);
    /* Convert RGB → HSB for animation state */
    float r = rgb.r / 255.0f, g = rgb.g / 255.0f, b = rgb.b / 255.0f;
    float max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float delta = max - min;
    state.color.h = (delta == 0) ? 0 :
        (max == r) ? (uint16_t)(60 * ((g - b) / delta)) % 360 :
        (max == g) ? (uint16_t)(60 * ((b - r) / delta + 2)) % 360 :
                     (uint16_t)(60 * ((r - g) / delta + 4)) % 360;
    state.color.s = (max == 0) ? 0 : (uint8_t)(delta / max * 100);
    state.color.b = (uint8_t)(max * 100);
    current_layer = layer;
    uint32_t mask = get_layer_mask(layer);
    LOG_INF("apply_layer_color_immediate: layer=%d rgb=(%d,%d,%d) hsb=(%d,%d,%d) mask=0x%08X",
            layer, rgb.r, rgb.g, rgb.b, state.color.h, state.color.s, state.color.b, mask);
}

/* ── Layer indication ──────────────────────────────────────────────────── */

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)

void ws2812_set_layer_color(uint8_t layer) {
    test_mode_active = false;  /* Re-enable animation when layer changes */
    manual_mode_active = false;
    current_layer = layer;
    struct led_rgb color = get_layer_color(layer);
    uint32_t mask = get_layer_mask(layer);
    LOG_INF("ws2812_set_layer_color: layer=%d rgb=(%d,%d,%d) mask=0x%08X test_mode=OFF manual_mode=OFF",
            layer, color.r, color.g, color.b, mask);
    apply_layer_color_immediate(layer);
}

#if DT_NODE_HAS_STATUS(DT_NODELABEL(ws2812_layer), okay)
#define WS2812_LAYER_BEHAVIOR_DEV DEVICE_DT_NAME(DT_NODELABEL(ws2812_layer))
#endif

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static void invoke_ws2812_layer_behavior(uint8_t layer) {
#if DT_NODE_HAS_STATUS(DT_NODELABEL(ws2812_layer), okay)
    struct zmk_behavior_binding binding = {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
        .local_id = 0,
#endif
        .behavior_dev = WS2812_LAYER_BEHAVIOR_DEV,
        .param1 = layer,
        .param2 = 0,
    };
    struct zmk_behavior_binding_event event = {
        .position = 0,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
    LOG_INF("invoke_ws2812_layer_behavior: relaying layer %d to peripherals", layer);
    int ret = zmk_behavior_invoke_binding(&binding, event, true);
    if (ret) {
        LOG_WRN("Failed to invoke &ws2812_layer %d: %d", layer, ret);
    } else {
        LOG_INF("invoke_ws2812_layer_behavior: layer %d relayed OK", layer);
    }
#else
    LOG_WRN("ws2812_layer behavior node not found in devicetree");
#endif
}
#endif

void ws2812_indicate_layer(void) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    uint8_t layer = zmk_keymap_highest_layer_active();
    uint32_t mask = get_layer_mask(layer);
    struct led_rgb color = get_layer_color(layer);
    LOG_INF("ws2812_indicate_layer [CENTRAL]: layer=%d rgb=(%d,%d,%d) mask=0x%08X", layer, color.r, color.g, color.b, mask);
    apply_layer_color_immediate(layer);
    invoke_ws2812_layer_behavior(layer);
#else
    LOG_INF("ws2812_indicate_layer [PERIPHERAL]: layer=%d mask=0x%08X", current_layer, get_layer_mask(current_layer));
    apply_layer_color_immediate(current_layer);
#endif
}

static struct k_work_delayable layer_indicate_work;

static int led_layer_listener_cb(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *activity_ev = as_zmk_activity_state_changed(eh);
    if (activity_ev != NULL) {
        switch (activity_ev->state) {
        case ZMK_ACTIVITY_SLEEP:
            if (!initialized) break;
            LOG_INF("LED layer listener: activity=SLEEP, turning off LEDs");
            zmk_ws2812_widget_off();
            current_layer = 0;
            return 0;
        case ZMK_ACTIVITY_ACTIVE:
            LOG_INF("LED layer listener: activity=ACTIVE, rescheduling layer indicate");
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
            if (ext_power_dev != NULL) {
                ext_power_enable(ext_power_dev);
            }
#endif
            if (initialized) {
                k_work_reschedule(&layer_indicate_work, K_MSEC(CONFIG_WS2812_WIDGET_LAYER_DEBOUNCE_MS));
            }
            break;
        default:
            break;
        }
        return 0;
    }

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (initialized) {
        uint8_t layer = zmk_keymap_highest_layer_active();
        LOG_INF("LED layer event: new highest_layer=%d, scheduling indicate (debounce=%dms)",
                layer, CONFIG_WS2812_WIDGET_LAYER_DEBOUNCE_MS);
        k_work_reschedule(&layer_indicate_work, K_MSEC(CONFIG_WS2812_WIDGET_LAYER_DEBOUNCE_MS));
    }
#endif
    return 0;
}

static void indicate_layer_cb(struct k_work *work) {
    ws2812_indicate_layer();
}

ZMK_LISTENER(led_layer_listener, led_layer_listener_cb);
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_SUBSCRIPTION(led_layer_listener, zmk_layer_state_changed);
#endif
ZMK_SUBSCRIPTION(led_layer_listener, zmk_activity_state_changed);

#endif /* CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE */

/* ── Battery indication ──────────────────────────────────────────────── */

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY)

static struct led_rgb get_battery_color(uint8_t battery_level) {
    if (battery_level == 0) return hex_to_rgb(CONFIG_WS2812_WIDGET_COLOR_OFF);
    if (battery_level >= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_HIGH) return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_HIGH);
    if (battery_level >= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_LOW) return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_MEDIUM);
    if (battery_level <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL) return hex_to_rgb(CONFIG_WS2812_WIDGET_COLOR_RED);
    return hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_LOW);
}

void ws2812_indicate_battery(void) {
    uint8_t battery_level = zmk_battery_state_of_charge();
    int retry = 0;
    while (battery_level == 0 && retry++ < 10) {
        k_sleep(K_MSEC(100));
        battery_level = zmk_battery_state_of_charge();
    }
    /* Flash battery colour 3 times on masked LEDs */
    struct led_rgb color = get_battery_color(battery_level);
    uint32_t mask = get_layer_mask(current_layer);
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
            pixels[i] = ((mask >> i) & 1) ? color : (struct led_rgb){0, 0, 0};
        }
        led_strip_flush();
        k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_BATTERY_BLINK_MS));
        for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
            pixels[i] = (struct led_rgb){0, 0, 0};
        }
        led_strip_flush();
        k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_INTERVAL_MS));
    }
}

static int led_battery_listener_cb(const zmk_event_t *eh) {
    if (!initialized) return 0;
    uint8_t battery_level = as_zmk_battery_state_changed(eh)->state_of_charge;
    if (battery_level > 0 && battery_level <= CONFIG_WS2812_WIDGET_BATTERY_LEVEL_CRITICAL) {
        /* Flash critical battery on masked LEDs */
        struct led_rgb color = hex_to_rgb(CONFIG_WS2812_WIDGET_BATTERY_COLOR_CRITICAL);
        uint32_t mask = get_layer_mask(current_layer);
        for (int r = 0; r < 5; r++) {
            for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
                pixels[i] = ((mask >> i) & 1) ? color : (struct led_rgb){0, 0, 0};
            }
            led_strip_flush();
            k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_BATTERY_BLINK_MS));
        }
    }
    return 0;
}

ZMK_LISTENER(led_battery_listener, led_battery_listener_cb);
ZMK_SUBSCRIPTION(led_battery_listener, zmk_battery_state_changed);

#endif /* Battery */

/* ── Connectivity indication ──────────────────────────────────────────── */

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_CONNECTIVITY)

static void indicate_connectivity_internal(void) {
    struct led_rgb color;
    uint8_t repeat_count = 1;

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    switch (zmk_endpoints_selected().transport) {
    case ZMK_TRANSPORT_USB:
// #if IS_ENABLED(CONFIG_WS2812_WIDGET_CONN_SHOW_USB)
//         color = hex_to_rgb(CONFIG_WS2812_WIDGET_CONN_COLOR_USB);
//         break;
// #endif
    default:
#if IS_ENABLED(CONFIG_ZMK_BLE)
        {
            uint8_t profile_index = zmk_ble_active_profile_index();
            if (zmk_ble_active_profile_is_connected()) {
                color = hex_to_rgb(CONFIG_WS2812_WIDGET_CONN_COLOR_CONNECTED);
                repeat_count = profile_index + 1;
            } else if (zmk_ble_active_profile_is_open()) {
                color = hex_to_rgb(CONFIG_WS2812_WIDGET_CONN_COLOR_ADVERTISING);
                repeat_count = profile_index + 1;
            } else {
                color = hex_to_rgb(CONFIG_WS2812_WIDGET_CONN_COLOR_DISCONNECTED);
                repeat_count = profile_index + 1;
            }
        }
#endif
        break;
    }
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
    if (zmk_split_bt_peripheral_is_connected()) {
        color = hex_to_rgb(CONFIG_WS2812_WIDGET_CONN_COLOR_CONNECTED);
    } else {
        color = hex_to_rgb(CONFIG_WS2812_WIDGET_CONN_COLOR_DISCONNECTED);
    }
#endif

    uint32_t mask = get_layer_mask(current_layer);
    for (int r = 0; r < repeat_count; r++) {
        for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
            pixels[i] = ((mask >> i) & 1) ? color : (struct led_rgb){0, 0, 0};
        }
        led_strip_flush();
        k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_CONN_BLINK_MS));
        for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
            pixels[i] = (struct led_rgb){0, 0, 0};
        }
        led_strip_flush();
        k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_INTERVAL_MS));
    }
}

static int led_output_listener_cb(const zmk_event_t *eh) {
    if (initialized) {
        indicate_connectivity_internal();
    }
    return 0;
}

ZMK_LISTENER(led_output_listener, led_output_listener_cb);
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#if IS_ENABLED(CONFIG_WS2812_WIDGET_CONN_SHOW_USB)
ZMK_SUBSCRIPTION(led_output_listener, zmk_endpoint_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(led_output_listener, zmk_ble_active_profile_changed);
#endif
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
ZMK_SUBSCRIPTION(led_output_listener, zmk_split_peripheral_status_changed);
#endif

static struct k_work_delayable indicate_connectivity_work;
static void indicate_connectivity_cb(struct k_work *work) {
    indicate_connectivity_internal();
}
void ws2812_indicate_connectivity(void) {
    k_work_reschedule(&indicate_connectivity_work, K_MSEC(16));
}

#endif /* Connectivity */

/* ── Manual pixel control ─────────────────────────────────────────────── */

void ws2812_set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (index >= STRIP_NUM_PIXELS) {
        LOG_WRN("ws2812_set_pixel: index %d out of range (max %d)", index, STRIP_NUM_PIXELS);
        return;
    }
    manual_mode_active = true;   /* Suppress animation tick */
    test_mode_active = true;     /* Also suppress test tick path if used */
    pixels[index] = (struct led_rgb){r, g, b};
    LOG_INF("ws2812_set_pixel: LED %d → rgb(%d,%d,%d)", index, r, g, b);
    led_strip_flush();
}

void ws2812_clear_all(void) {
    manual_mode_active = false;
    test_mode_active = false;
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){0, 0, 0};
    }
    LOG_INF("ws2812_clear_all: all LEDs cleared, animation resumed");
    led_strip_flush();
}

/* ── LED test mode ─────────────────────────────────────────────────────── */

#if IS_ENABLED(CONFIG_WS2812_WIDGET_LED_TEST)

static uint8_t test_color_idx = 0;
static const struct led_rgb test_colors[] = {
    {255, 0, 0},     // 0: red
    {0, 255, 0},     // 1: green
    {0, 0, 255},     // 2: blue
    {255, 255, 255}, // 3: white
    {255, 255, 0},   // 4: yellow
    {0, 255, 255},   // 5: cyan
};
#define NUM_TEST_COLORS (sizeof(test_colors) / sizeof(test_colors[0]))

void ws2812_test_led(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    test_mode_active = true;  /* Suppress animation tick while testing */
    if (index >= STRIP_NUM_PIXELS) {
        for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
            pixels[i] = (struct led_rgb){0, 0, 0};
        }
        led_strip_flush();
        return;
    }
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){0, 0, 0};
    }
    pixels[index] = (struct led_rgb){r, g, b};
    led_strip_flush();
}

static int led_test_position_cb(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos_ev = as_zmk_position_state_changed(eh);
    if (pos_ev == NULL || !pos_ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    /* Position-to-LED mapping handled by behavior_led_test */
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_test_pos, led_test_position_cb);
ZMK_SUBSCRIPTION(led_test_pos, zmk_position_state_changed);

/* ── LED scan cursor ────────────────────────────────────────────────────
 * Interactive index/color cursor for &led_idx / &led_col: rotate through
 * every LED and every color code without rebuilding firmware per guess.
 * Reuses the same color table as behavior_led_test.c / behavior_led_set.c. */

static uint8_t scan_index = 0;
static uint8_t scan_color_code = 4; /* start on white */

static void scan_apply(void) {
    uint8_t r, g, b;
    switch (scan_color_code) {
    case 1: r = 255; g = 0;   b = 0;   break; // red
    case 2: r = 0;   g = 255; b = 0;   break; // green
    case 3: r = 0;   g = 0;   b = 255; break; // blue
    case 4: r = 255; g = 255; b = 255; break; // white
    case 5: r = 255; g = 255; b = 0;   break; // yellow
    case 6: r = 0;   g = 255; b = 255; break; // cyan
    case 7: r = 255; g = 0;   b = 255; break; // magenta
    case 8: r = 255; g = 165; b = 0;   break; // orange
    default: r = 0;  g = 0;   b = 0;   break; // off
    }
    LOG_INF("LED scan: index=%d color=%d rgb(%d,%d,%d)", scan_index, scan_color_code, r, g, b);
    ws2812_test_led(scan_index, r, g, b);
}

void ws2812_scan_step_index(int8_t direction) {
    scan_index = (scan_index + direction + STRIP_NUM_PIXELS) % STRIP_NUM_PIXELS;
    scan_apply();
}

void ws2812_scan_step_color(int8_t direction) {
    scan_color_code = (scan_color_code + direction + 9) % 9; /* 0..8 */
    scan_apply();
}

#else
/* Stub: LED test disabled — provide empty implementation for link compatibility */
void ws2812_test_led(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    ARG_UNUSED(index);
    ARG_UNUSED(r);
    ARG_UNUSED(g);
    ARG_UNUSED(b);
}

void ws2812_scan_step_index(int8_t direction) { ARG_UNUSED(direction); }
void ws2812_scan_step_color(int8_t direction) { ARG_UNUSED(direction); }
#endif /* LED_TEST */

/* ── Position logger (dongle debug) ────────────────────────────────────── */

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static int position_logger_cb(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos_ev = as_zmk_position_state_changed(eh);
    if (pos_ev == NULL) return ZMK_EV_EVENT_BUBBLE;
    LOG_INF("KEY: pos %lu %s", pos_ev->position, pos_ev->state ? "pressed" : "released");
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(pos_logger, position_logger_cb);
ZMK_SUBSCRIPTION(pos_logger, zmk_position_state_changed);
#endif

/* ── Initialization ─────────────────────────────────────────────────────── */

#if IS_ENABLED(CONFIG_WS2812_WIDGET_AUTO_SCAN)
static void auto_scan_flush_repeated(void) {
    for (int attempt = 0; attempt < 4; attempt++) {
        led_strip_flush();
        k_sleep(K_MSEC(2));
    }
}

static void auto_scan_fill(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){r, g, b};
    }
    auto_scan_flush_repeated();
}

static void auto_scan_highlight(uint8_t index) {
    /* Only the selected raw address is enabled. */
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){0, 0, 0};
    }
    pixels[index] = (struct led_rgb){255, 255, 255};
    auto_scan_flush_repeated();
}

static void run_auto_scan(void) {
    while (true) {
        /* Driver/color calibration: red, green, blue, then white. */
        static const struct led_rgb calibration_colors[] = {
            {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}
        };
        for (int color = 0; color < ARRAY_SIZE(calibration_colors); color++) {
            auto_scan_fill(calibration_colors[color].r, calibration_colors[color].g,
                           calibration_colors[color].b);
            k_sleep(K_SECONDS(2));
            auto_scan_fill(0, 0, 0);
            k_sleep(K_MSEC(400));
        }

        /* Dim all-LED marker: the address scan starts after this pause. */
        auto_scan_fill(16, 16, 16);
        k_sleep(K_SECONDS(2));
        auto_scan_fill(0, 0, 0);
        k_sleep(K_SECONDS(1));

        for (uint8_t index = 0; index < STRIP_NUM_PIXELS; index++) {
            auto_scan_highlight(index);
            LOG_INF("AUTO LED SCAN: raw index %d", index);
            k_sleep(K_MSEC(1000));
            auto_scan_fill(0, 0, 0);
            k_sleep(K_MSEC(350));
        }

        /* End marker, then restart the complete sequence. */
        auto_scan_fill(255, 0, 255);
        k_sleep(K_SECONDS(3));
        auto_scan_fill(0, 0, 0);
        k_sleep(K_SECONDS(2));
    }
}
#endif

extern void led_init_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0); ARG_UNUSED(d1); ARG_UNUSED(d2);

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
    int ext_rc = 0;
    if (device_is_ready(ext_power_dev)) {
        ext_rc = ext_power_enable(ext_power_dev);
        if (ext_rc != 0) {
            LOG_ERR("Failed to enable EXT_POWER: %d", ext_rc);
        } else {
            LOG_INF("EXT_POWER enabled for WS2812 (initial)");
        }
    } else {
        LOG_WRN("EXT_POWER device not ready");
    }
    LOG_INF("Waiting 2s for settings to load before re-enabling EXT_POWER...");
    k_sleep(K_MSEC(2000));
    ext_rc = ext_power_enable(ext_power_dev);
    if (ext_rc != 0) {
        LOG_ERR("Failed to re-enable EXT_POWER after settings: %d", ext_rc);
    } else {
        LOG_INF("EXT_POWER re-enabled for WS2812 (after settings)");
    }
    k_sleep(K_MSEC(50));
#endif

    if (!device_is_ready(led_strip)) {
        LOG_ERR("WS2812 LED strip device not ready");
        return;
    }
    LOG_INF("WS2812 LED strip initialized with %d pixels", STRIP_NUM_PIXELS);

    /* Загрузочный диагностический прогон удалён: он занимал ~52 с при
     * каждом старте и оставлял test_mode_active=true (анимация зависала
     * до первой смены слоя). Для ручных тестов есть &led_test и &led_set. */

    /* Set initial state from Kconfig */
    state = (struct ws2812_state){
        .color = {
            .h = 0,
            .s = 100,
            .b = 100,
        },
        .animation_speed = IS_ENABLED(CONFIG_WS2812_WIDGET_ANIMATION_SPEED) ? CONFIG_WS2812_WIDGET_ANIMATION_SPEED : 3,
        .current_effect = IS_ENABLED(CONFIG_WS2812_WIDGET_EFFECT) ? CONFIG_WS2812_WIDGET_EFFECT : 0,
        .animation_step = 0,
        .on = false,
    };

#if IS_ENABLED(CONFIG_WS2812_WIDGET_AUTO_SCAN)
    LOG_INF("Starting automatic raw LED address scan");
    run_auto_scan();
    return;
#endif

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
    k_work_init_delayable(&layer_indicate_work, indicate_layer_cb);
#endif

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_CONNECTIVITY)
    k_work_init_delayable(&indicate_connectivity_work, indicate_connectivity_cb);
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_BATTERY)
    ws2812_indicate_battery();
    k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_BATTERY_BLINK_MS + CONFIG_WS2812_WIDGET_INTERVAL_MS));
#endif

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_CONNECTIVITY)
    ws2812_indicate_connectivity();
#endif

#if IS_ENABLED(CONFIG_WS2812_WIDGET_SHOW_LAYER_CHANGE)
    k_sleep(K_MSEC(CONFIG_WS2812_WIDGET_INTERVAL_MS));
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    current_layer = zmk_keymap_highest_layer_active();
    LOG_INF("Initial layer detection (central): layer=%d", current_layer);
#else
    LOG_INF("Initial layer detection (peripheral): using default layer=%d", current_layer);
#endif
    apply_layer_color_immediate(current_layer);
    LOG_INF("Initial layer %d applied, mask=0x%08X", current_layer, get_layer_mask(current_layer));
#endif

    initialized = true;
    zmk_ws2812_widget_off();
    LOG_INF("WS2812 widget ready with layer hints disabled (effect=%d, speed=%d)",
            state.current_effect, state.animation_speed);
}

K_THREAD_DEFINE(led_init_tid, 2048, led_init_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 100);

#pragma once

/*
 * Layer hints for the six independently addressable vertical LED groups.
 *
 * The array order is the raw WS2812 address order (0..5). On the left half
 * this runs from the inner T/G/B column to the outer Tab/Caps/Ctrl column;
 * on the right half it runs from the inner Y/H/N column to the outer column.
 *
 * Change any entry below to a named color or a custom 0xRRGGBB value, then
 * rebuild both halves. Because all keys in a vertical group share one address,
 * individual rows within a group cannot have different colors.
 */

#define LED_OFF     0x000000
#define LED_RED     0xFF0000
#define LED_GREEN   0x00FF00
#define LED_BLUE    0x0000FF
#define LED_ORANGE  0xFF8C00
#define LED_CYAN    0x00BFFF
#define LED_MAGENTA 0xFF00FF
#define LED_WHITE   0xFFFFFF

#define WS2812_LAYER_COUNT 4
#define WS2812_GROUP_COUNT 6

#if defined(CONFIG_BOARD_EYELASH_CORNE_LEFT)
static const uint32_t
    ws2812_layer_group_colors[WS2812_LAYER_COUNT][WS2812_GROUP_COUNT] = {
        /* Base: no hints. */
        [0] = {LED_OFF, LED_OFF, LED_OFF, LED_OFF, LED_OFF, LED_OFF},

        /* Numbers: arrow columns A/S/D/F. */
        [1] = {LED_OFF, LED_BLUE, LED_BLUE, LED_BLUE, LED_BLUE, LED_OFF},

        /* Symbols: columns Q/W/E/R/T. */
        [2] = {LED_ORANGE, LED_ORANGE, LED_ORANGE, LED_ORANGE, LED_ORANGE, LED_OFF},

        /* Fn: F1-F5 and Bluetooth profile columns. */
        [3] = {LED_CYAN, LED_CYAN, LED_CYAN, LED_CYAN, LED_CYAN, LED_OFF},
    };
#elif defined(CONFIG_BOARD_EYELASH_CORNE_RIGHT)
static const uint32_t
    ws2812_layer_group_colors[WS2812_LAYER_COUNT][WS2812_GROUP_COUNT] = {
        /* Base: no hints. */
        [0] = {LED_OFF, LED_OFF, LED_OFF, LED_OFF, LED_OFF, LED_OFF},

        /* Numbers: numeric keypad columns; outer navigation column stays dark. */
        [1] = {LED_GREEN, LED_GREEN, LED_GREEN, LED_GREEN, LED_GREEN, LED_OFF},

        /* Symbols: every right-hand column contains symbols. */
        [2] = {LED_ORANGE, LED_ORANGE, LED_ORANGE, LED_ORANGE, LED_ORANGE, LED_ORANGE},

        /* Fn: F6-F11 plus system-key columns. */
        [3] = {LED_CYAN, LED_CYAN, LED_CYAN, LED_CYAN, LED_CYAN, LED_CYAN},
    };
#else
#error "ws2812_led_layouts.h included for an unsupported board"
#endif

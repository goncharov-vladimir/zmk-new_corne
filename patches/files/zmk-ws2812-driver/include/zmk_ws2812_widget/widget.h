#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <zmk/rgb_underglow.h>

/**
 * @brief Indicate current battery status with WS2812 LED colors/patterns
 */
void ws2812_indicate_battery(void);

/**
 * @brief Indicate current connectivity status with WS2812 LED colors/patterns
 */
void ws2812_indicate_connectivity(void);

/**
 * @brief Indicate current layer with WS2812 LED colors/patterns
 */
void ws2812_indicate_layer(void);

/**
 * @brief Set WS2812 LED color for a specific layer number.
 *
 * Works on both central and peripheral devices. On the peripheral, this
 * is called via the behavior relay when the central sends &ws2812_layer <N>.
 */
void ws2812_set_layer_color(uint8_t layer);

/**
 * @brief Light up a single LED for testing (clears all others first).
 */
void ws2812_test_led(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

/** Move the interactive test cursor to another LED index. */
void ws2812_scan_step_index(int8_t direction);

/** Change the color used by the interactive LED test cursor. */
void ws2812_scan_step_color(int8_t direction);

/**
 * @brief Set a single LED to an RGB colour without clearing other LEDs.
 *
 * Activates manual mode (stops animation tick). Call ws2812_clear_all()
 * or switch layer/effect to resume normal operation.
 */
void ws2812_set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Turn off all LEDs and exit manual mode, resuming normal animation.
 */
void ws2812_clear_all(void);

/* ── Animation control API (matching rgb_underglow API) ─────────────────── */

/**
 * @brief Select an animation effect by index.
 *  0 = solid, 1 = breathe, 2 = spectrum, 3 = swirl
 */
int zmk_ws2812_widget_select_effect(int effect);

/**
 * @brief Cycle to next/previous effect.
 */
int zmk_ws2812_widget_cycle_effect(int direction);

/**
 * @brief Set colour in HSB space.
 */
int zmk_ws2812_widget_set_hsb(struct zmk_led_hsb color);

/**
 * @brief Calculate next hue value.
 */
struct zmk_led_hsb zmk_ws2812_widget_calc_hue(int direction);

/**
 * @brief Calculate next saturation value.
 */
struct zmk_led_hsb zmk_ws2812_widget_calc_sat(int direction);

/**
 * @brief Calculate next brightness value.
 */
struct zmk_led_hsb zmk_ws2812_widget_calc_brt(int direction);

/**
 * @brief Change hue by step.
 */
int zmk_ws2812_widget_change_hue(int direction);

/**
 * @brief Change saturation by step.
 */
int zmk_ws2812_widget_change_sat(int direction);

/**
 * @brief Change brightness by step.
 */
int zmk_ws2812_widget_change_brt(int direction);

/**
 * @brief Change animation speed by step.
 */
int zmk_ws2812_widget_change_spd(int direction);

/**
 * @brief Turn LEDs on and start animation timer.
 */
int zmk_ws2812_widget_on(void);

/**
 * @brief Turn LEDs off and stop animation timer.
 */
int zmk_ws2812_widget_off(void);

/**
 * @brief Toggle LEDs on/off.
 */
int zmk_ws2812_widget_toggle(void);

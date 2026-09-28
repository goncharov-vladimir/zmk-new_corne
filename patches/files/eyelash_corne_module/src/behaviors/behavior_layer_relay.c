/*
 * Layer change → WS2812 behavior relay for the BLE central (dongle).
 *
 * When WS2812_WIDGET=n on the central, widget.c's led_layer_listener_cb
 * doesn't compile, so layer changes are never relayed to peripherals.
 * This module fills the gap: it subscribes to zmk_layer_state_changed
 * and invokes &ws2812_layer <N> with BEHAVIOR_LOCALITY_GLOBAL, which
 * ZMK's split system delivers to all connected peripherals.
 *
 * On peripherals (WS2812_WIDGET=y) this is a no-op — widget.c already
 * handles layer indication locally AND receives the relayed behavior.
 */

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/split/central.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#if !IS_ENABLED(CONFIG_WS2812_WIDGET)

/* ── Minimal layer relay: dongle-only, no WS2812 widget needed ───────── */

#define WS2812_LAYER_BEHAVIOR_DEV                                                                \
	COND_CODE_1(DT_NODE_HAS_STATUS(DT_NODELABEL(ws2812_layer), okay),                       \
		    (DEVICE_DT_NAME(DT_NODELABEL(ws2812_layer))), (NULL))

static void invoke_ws2812_layer_on_peripherals(uint8_t layer) {
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
	LOG_INF("layer_relay: invoking &ws2812_layer %d (global relay)", layer);
	int ret = zmk_behavior_invoke_binding(&binding, event, true);
	if (ret) {
		LOG_WRN("layer_relay: failed to invoke &ws2812_layer %d: %d", layer, ret);
	} else {
		LOG_INF("layer_relay: &ws2812_layer %d relayed OK", layer);
	}
#else
	LOG_WRN("layer_relay: ws2812_layer node not found in devicetree");
#endif
}

#include <zmk/event_manager.h>

static struct k_work_delayable layer_relay_work;

static void layer_relay_cb(struct k_work *work) {
	ARG_UNUSED(work);
	uint8_t layer = zmk_keymap_highest_layer_active();
	LOG_INF("layer_relay_cb: highest_layer=%d", layer);
	invoke_ws2812_layer_on_peripherals(layer);
}

static int layer_relay_listener_cb(const zmk_event_t *eh) {
	struct zmk_activity_state_changed *act_ev = as_zmk_activity_state_changed(eh);
	if (act_ev != NULL) {
		/* Activity state changes are not our concern here,
		 * but the event subsystem delivers them to all listeners.
		 * Just ignore and return. */
		return 0;
	}

	/* Only on central: debounce and relay layer change */
	k_work_reschedule(&layer_relay_work, K_MSEC(50));
	return 0;
}

ZMK_LISTENER(layer_relay, layer_relay_listener_cb);
ZMK_SUBSCRIPTION(layer_relay, zmk_layer_state_changed);

static int layer_relay_init(void) {
	k_work_init_delayable(&layer_relay_work, layer_relay_cb);
	LOG_INF("layer_relay: initialized (dongle-only, WS2812_WIDGET=n)");
	return 0;
}
SYS_INIT(layer_relay_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* !CONFIG_WS2812_WIDGET */
#endif /* CONFIG_ZMK_SPLIT && CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
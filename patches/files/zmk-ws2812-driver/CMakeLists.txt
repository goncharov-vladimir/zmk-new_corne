if(CONFIG_ZMK_WS2812_LAYER_BEHAVIOR)

# The full widget only belongs on halves that have a physical LED strip.
target_sources_ifdef(CONFIG_WS2812_WIDGET app PRIVATE src/widget.c)

# The behavior is also built on the dongle to relay commands to peripherals.
target_sources(app PRIVATE src/behaviors/behavior_ws2812_widget.c)
target_sources(app PRIVATE src/events/ws2812_hints_state_changed.c)

zephyr_include_directories(include)
target_include_directories(app PRIVATE ${ZMK_CONFIG})

endif()

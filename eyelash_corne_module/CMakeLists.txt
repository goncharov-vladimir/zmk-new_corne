# eyelash_corne module — board & shield definitions
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)

# This module provides board/shield definitions and layer relay behavior.
# The ws2812-driver module provides the include path for <zmk_ws2812_widget/widget.h>.
# Behaviors компилируются и на донгле (WIDGET=n): global-relay требует драйвер
# на central. Вызовы функций виджета внутри guarded #if CONFIG_WS2812_WIDGET.
if(CONFIG_ZMK_WS2812_LAYER_BEHAVIOR)
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/behaviors/behavior_layer_relay.c)
endif()

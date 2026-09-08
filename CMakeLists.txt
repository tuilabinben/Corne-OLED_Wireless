# Copyright (c) 2024 The ZMK Contributors
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)

if(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM)
    target_sources(app PRIVATE src/status_screen.c)
endif()

# µnleashed gateway sat: the sources.
# Copyright 2026 - Robert Mech. SPDX-License-Identifier: GPL-3.0-or-later
#
# A new .cpp needs a clean build, because this GLOBs (as the core's own
# component does).
#
# webassets/ is generated: tools/fetch_web.py fetches xterm.js and
# tools/mkweb.py gzips every page into it, writes web_assets.h and writes
# web_assets.cmake naming the files to embed. Generating the CMake list as
# well as the header is what stops the two drifting the day a page is added.
#
# Building with idf.py rather than PlatformIO: run
#   python tools/mkweb.py
# first, or this include fails and says so.
FILE(GLOB app_sources ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/webassets/web_assets.cmake)
    message(FATAL_ERROR
        "src/webassets is not generated. Run: python tools/mkweb.py")
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/webassets/web_assets.cmake)

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "." "${CMAKE_CURRENT_SOURCE_DIR}/webassets"
                       EMBED_FILES ${GW_WEB_EMBED}
                       REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer
                                esp_http_server esp_driver_uart esp_driver_gpio
                                esp_hw_support lwip mbedtls esp_app_format)

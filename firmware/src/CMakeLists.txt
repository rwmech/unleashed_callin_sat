# µnleashed gateway sat: the sources.
# Copyright 2026 - Robert Mech. SPDX-License-Identifier: GPL-3.0-or-later
#
# A new .cpp needs a clean build, because this GLOBs (as the core's own
# component does).
#
# webassets/ is generated: tools/fetch_web.py fetches xterm.js and
# tools/mkweb.py gzips every page into webassets/web_assets.cpp, with
# web_assets.h naming what each one is served as. One generated .cpp rather
# than CMake's EMBED_FILES, which PlatformIO's SCons cannot build; mkweb.py
# says so at more length.
#
# COMPONENT_DIR and not CMAKE_CURRENT_SOURCE_DIR, every time. The IDF
# processes a component's CMakeLists.txt TWICE: once as a cmake script to
# collect its requirements, where CMAKE_CURRENT_SOURCE_DIR is the IDF's own
# script directory and every path built from it is wrong, and once properly.
# COMPONENT_DIR is set in both (component_get_requirements.cmake:105).
#
# Building with idf.py rather than PlatformIO: run
#   python tools/mkweb.py
# first, or this include fails and says so.
FILE(GLOB app_sources ${COMPONENT_DIR}/*.cpp)

if(NOT EXISTS ${COMPONENT_DIR}/webassets/web_assets.cpp)
    message(FATAL_ERROR
        "src/webassets is not generated. Run: python tools/mkweb.py")
endif()

idf_component_register(SRCS ${app_sources} "webassets/web_assets.cpp"
                       INCLUDE_DIRS "." "webassets"
                       REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer
                                esp_http_server esp_driver_uart esp_driver_gpio
                                esp_hw_support lwip mbedtls esp_app_format)

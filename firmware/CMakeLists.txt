# µnleashed gateway sat: the firmware.
# Copyright 2026 - Robert Mech. SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16.0)
# The version in the image, set here rather than from git describe, which
# fails on a repository with no commits and says nothing useful in a clone.
# Keep it the same as GW_VERSION in src/version.h; version.h is what the
# portal and the console show.
set(PROJECT_VER "0.1.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gateway)

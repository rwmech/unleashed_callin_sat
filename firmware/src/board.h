// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/board.h
// Module:       The board profile: which ESP32 this image is for, and which
//               of its pins are already somebody's
//
// Purpose:      The ONE build-time choice in this firmware. The three roles
//               (access point, terminal server, repeater) are runtime
//               settings and never build flags; what a build does decide is
//               which part it runs on, because pins are physical.
//
//               A profile carries its own pin table and nothing falls
//               through from another profile. That rule is the core's, paid
//               for by the AI-Thinker ESP32-CAM, where GPIO0 is the camera's
//               clock and every inherited default was wrong.
//
//               pinProblem() is the one place a pin is judged, so the
//               settings page, the serial line's own start-up and any later
//               role all refuse the same pin for the same stated reason.
//               A refusal names the pin and says who holds it; that is the
//               difference between a sysop fixing it in ten seconds and a
//               sysop reflashing to find out.
//
// Targets:      GW_BOARD_WROOM32 (the floor) and GW_BOARD_WROVER (the hedge)
// See also:     settings.cpp (the pin rows), uart.cpp (the terminal server)
//
// Copyright 2026 - Robert Mech
// License:      GNU General Public License v3 or later
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation; either version 3 of the License, or (at your
// option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program. If not, see <https://www.gnu.org/licenses/>.
// ===========================================================================
#pragma once
#include <cstdint>

#if !defined(GW_BOARD_WROOM32) && !defined(GW_BOARD_WROVER)
#error "No board profile. Build an env from platformio.ini (gateway, gateway_wrover)."
#endif

// ---------------------------------------------------------------------------
//  The profiles
// ---------------------------------------------------------------------------
#ifdef GW_BOARD_WROOM32
// A bare ESP32-WROOM-32E dev kit: 4 MB flash, no PSRAM, two cores. The
// floor, and what every figure in this repository is measured against.
#define GW_BOARD_TAG      "WROOM"
#define GW_BOARD_VERSION  "1.0.0"
#define GW_HAS_PSRAM      0
#define GW_LED_PIN        2          // the blue LED on most dev kits
#endif

#ifdef GW_BOARD_WROVER
// An ESP32-WROVER: the same part with 4 MB of PSRAM, the hedge if phase 2
// reads the internal heap with the access point up and finds it tight.
// GPIO 16 and 17 are the PSRAM's on this module and are refused by name.
#define GW_BOARD_TAG      "WROVER"
#define GW_BOARD_VERSION  "1.0.0"
#define GW_HAS_PSRAM      1
#define GW_LED_PIN        2
#endif

// The console is UART0 on both profiles, so GPIO 1 and 3 carry it and the
// terminal server may not have them. Same shape as the core's
// BBS_CONSOLE_UART0: a board whose console is the native USB port would set
// this 0 and free the pair.
#define GW_CONSOLE_UART0  1

// The terminal server's port. Never UART0, which is the console: flashing
// and the monitor have to keep working on a box in a field.
#define GW_UART_PORT      2

// Highest GPIO that exists, and the highest that can be an output. On the
// classic ESP32, 34 to 39 are input-only, so a TX pin stops at 33.
#define GW_GPIO_MAX       39
#define GW_GPIO_OUT_MAX   33

namespace board {

// Why this pin cannot be used, or nullptr if it can. `output` is true for a
// pin this firmware will drive (a UART's TX, an LED), false for one it only
// reads (a UART's RX, a button).
//
// Every caller asks this; nothing judges a pin for itself.
inline const char* pinProblem(int pin, bool output) {
    if (pin < 0) return nullptr;                  // -1 is off, the lights' convention
    if (pin > GW_GPIO_MAX) return "no such pin on this board";
    if (pin >= 6 && pin <= 11) return "the flash chip's";
    if (output && pin > GW_GPIO_OUT_MAX) return "input only on this board";
#if GW_CONSOLE_UART0
    if (pin == 1 || pin == 3) return "the console port, which must keep working";
#endif
#if defined(GW_BOARD_WROVER)
    if (pin == 16 || pin == 17) return "the PSRAM's on this module";
#endif
    if (pin == GW_LED_PIN) return "this board's LED";
    return nullptr;
}

// A strapping pin works, and is worth a word on the form rather than a
// refusal: somebody who knows what they are doing may want GPIO 5.
inline bool pinIsStrap(int pin) {
    return pin == 0 || pin == 2 || pin == 5 || pin == 12 || pin == 15;
}

}  // namespace board

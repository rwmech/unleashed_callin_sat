// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/ap.h
// Module:       The radio: the access point role, and the station beside it
//
// Purpose:      One radio, so one channel, for everything. That one fact is
//               the whole of the channel design and it decides every legal
//               arrangement:
//
//               | This gateway is | The channel is set by |
//               |---|---|
//               | an access point only | this gateway, freely. The field case |
//               | a station only       | the router it joined |
//               | both                 | the STATION, so the router, and the
//                                        access point is dragged onto it and
//                                        tells its phones with a Channel
//                                        Switch Announcement |
//
//               The rule that falls out, and it holds everywhere: THE
//               BOARD'S CHANNEL IS THE AUTHORITY, and an access point must
//               be set to it. In phase 1 the board is reached over IP, so
//               either the board joined this gateway's access point (and
//               the channel is settled by construction, which is why that
//               is the recommended shape with no router) or both are on one
//               router (and the router settles it). Phase 4 pairs over
//               ESP-NOW and then a channel that differs is refused by name.
//
//               The access point is OPEN, with no password, deliberately:
//               a caller at a fairground has nothing to type and a portal
//               behind a password is not a front door. What that costs is
//               said plainly on the portal rather than hidden.
//
// See also:     dns.h (every name answered with this box), web.h (the
//               portal), settings.h (ap_*, net_*)
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
#include <cstddef>
#include <cstdint>

namespace ap {

// Brings the radio up as the settings ask: an access point, a station, or
// both. Returns false if there was nothing to bring up, which is not an
// error: a gateway with no roles on and no network to join is a box waiting
// to be set up, and it says so rather than looking broken.
bool begin();

// Once a second, from main. The redial's clock lives on one task and the
// Wi-Fi event handler only ever stamps it, so there is nothing to race.
void tick();

// What the access point is, for the portal and the console.
bool        apUp();
const char* ssid();
const char* addr();          // dotted, the address a phone reaches us on
uint8_t     channel();       // what the radio actually settled on
uint8_t     phones();        // stations associated right now

// The network this gateway joined, if any.
bool        staUp();
const char* staAddr();       // dotted, or "" when not joined
const char* staSsid();

}  // namespace ap

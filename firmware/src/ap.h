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
//               THE ACCESS POINT IS PASSWORDED BY DEFAULT (Rob,
//               2026-10-05), and that settles the whole security story:
//               WPA2-PSK, where the pinned IDF overwrites the pairwise
//               cipher with CCMP for us, so every hop from the phone to
//               the board is encrypted and the portal being plain HTTP
//               makes no difference to anybody.
//
//               The default password is PUBLISHED, not secret: the same
//               word as the board's own published sysop default, meant to
//               be printed on the sign beside the network's name. What it
//               buys is CCMP on the air rather than privacy from callers.
//
//               Blank is still allowed and is the deliberate exception: an
//               open network, where anyone in range reads everything, said
//               plainly on the portal and in the board's own connection
//               line rather than hidden. The certificate argument belongs
//               ONLY there, because it is the only place anybody would
//               reach for HTTPS to fix things. Running the two cases
//               together is what produced an overclaim once already.
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
// Has a password, so WPA2 and the link is encrypted. This is what the
// portal's copy and the board's connection line both key off, so there is
// one answer to "is this hop readable" rather than two that could differ.
bool        secure();
const char* ssid();
const char* addr();          // dotted, the address a phone reaches us on
uint8_t     channel();       // what the radio actually settled on
uint8_t     phones();        // stations associated right now

// The network this gateway joined, if any.
bool        staUp();
const char* staAddr();       // dotted, or "" when not joined
const char* staSsid();

}  // namespace ap

// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/uart.h
// Module:       The terminal server role
//
// Purpose:      A serial port carrying one caller, and the cheapest
//               possible proof that the roles really are switches over one
//               core rather than three programs in a trench coat: this is
//               a second SOURCE feeding the same uplink the access point
//               feeds, and it is about two hundred lines.
//
//               What plugs in: a VT220, a PC's DB9 through a null modem
//               and an RS-232 level shifter, a Raspberry Pi running
//               Direwolf and linbpq for packet radio (1.3.0's ham dock is
//               this row with a Pi where the terminal was), and later a
//               modem answering a telephone line.
//
//               It works WHILE THE ACCESS POINT IS UP, which is Rob's own
//               example of why the roles compose: a phone and a terminal
//               at once, two callers, two board lines, one gateway.
//
//               Never UART0. That is the console, and flashing and the
//               monitor have to keep working on a box in a field; the pins
//               are refused by name, per board profile, so a sysop is told
//               on the form rather than after a reload.
//
//               THE WIRED LINE IS THE ONE PRIVATE PATH, from phase 3 on.
//               A wire the sysop ran, then AES-128-CCM over the link. In
//               phase 1 the second hop is plain telnet over IP, so the
//               line it shows says so; saying "then encrypted" here would
//               be wrong in the one place this project could honestly say
//               otherwise once CALLIN exists.
//
// See also:     line.h (the abstraction this is a source for), board.h
//               (which pins are refused), settings.h (the ser_* rows)
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

namespace uartsrv {

// Brings the port up and starts its reader. Returns false, with a reason on
// the console, when the settings do not allow it; it is never fatal, so a
// gateway whose serial pins are wrong still serves its portal.
bool begin();

void stop();
bool up();

// Whether a caller is on the wire right now, for the portal and the
// console.
bool busy();

}  // namespace uartsrv

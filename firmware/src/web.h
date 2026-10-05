// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/web.h
// Module:       The portal, the browser terminal, and this gateway's own
//               setup page
//
// Purpose:      The access point role's visible half, and the second half
//               of the portal popping up. dns.cpp makes every name resolve
//               to this box; this answers each operating system's probe in
//               the way that makes it open its sign-in sheet.
//
//               THE PORTAL IS A LANDING PAGE, NOT THE TERMINAL, and that
//               is a design decision rather than a layout one. Apple's
//               Captive Network Assistant is a sheet and not full Safari,
//               and a page wanting a WebSocket is exactly the sort of thing
//               that may not run in it; Android's sign-in flow is a Custom
//               Tab with the same question. So the sheet gets a small page
//               with a link, and the terminal opens in the caller's real
//               browser. It costs nothing and it is also why the landing
//               page is a few KB while the terminal is 70.
//
//               Three screens:
//                 /       the portal: what this is, the honest line about
//                         an open network, the boards, and the plain
//                         address for a phone whose portal did not pop
//                 /t      the terminal, which is a static asset
//                 /setup  this gateway's own settings, behind a password
//
//               Plain HTTP, and TLS is refused on CERTIFICATE grounds
//               rather than on memory: a captive portal cannot be
//               intercepted over HTTPS at all, and a box in a field can
//               only serve TLS under a name nobody has signed, so every
//               phone would meet a full-page security warning as the front
//               door. A warning is a worse experience AND a worse security
//               message than plain HTTP with an honest sentence, which is
//               the same wall RFC 8908 hits and concedes.
//
// See also:     dns.h, line.h (this file is one of the two sources),
//               settings.h
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

namespace web {

bool begin();
void stop();
bool up();

// What the whole web payload costs the app image, for the console. Knowing
// it is what stops anybody hunting for the flash a gateway "lost".
uint32_t payloadBytes();

}  // namespace web

// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/dns.h
// Module:       The DNS redirector: every name is this box
//
// Purpose:      The first half of the portal popping up. A phone that joins
//               an access point asks its own operating system's probe
//               address (Apple's captive.apple.com, Android's
//               connectivitycheck.gstatic.com, Microsoft's
//               www.msftconnecttest.com), and if that question is answered
//               with THIS box's address then the probe's HTTP request
//               arrives here and web.cpp can answer it in the way that
//               makes the OS open its sign-in sheet. Without this, the name
//               does not resolve, the probe fails, and the phone decides
//               the network has no internet without ever offering a portal.
//
//               Written here rather than vendored from Espressif's captive
//               portal example, for two reasons worth naming: the example
//               is Apache-2.0 code that would need its own notice in a
//               GPLv3 repository for about 200 lines of well-understood
//               wire format, and a responder answering a public open
//               network needs a bounded parser rather than an example's.
//
//               TTL is ZERO on purpose. A phone that caches an answer for
//               this box's address and then leaves would carry the hijack
//               with it: every site it visits for the next TTL would
//               resolve to an address that is no longer anything. Zero
//               costs one more query per name while the caller is here,
//               which on a local link is nothing.
//
//               Only A questions are answered with an address. AAAA gets a
//               well-formed answer with no records, which tells the phone
//               "no IPv6 here" rather than leaving it waiting.
//
// See also:     ap.h (which tells every phone to ask us), web.h (the other
//               half), RFC 1035 §4
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

namespace dns {

// Starts the responder on port 53, answering every A question with
// `dotted`. One socket and one small task.
bool begin(const char* dotted);

void stop();

// How many questions have been answered, for the status page: a portal
// that is not popping is usually a phone that never asked, and knowing
// which half failed is the whole of debugging it with no laptop.
uint32_t answered();

}  // namespace dns

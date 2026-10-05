// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/telnet.h
// Module:       The telnet client on the IP uplink
//
// Purpose:      The gateway is a caller to the board, so it has to speak
//               what a telnet client speaks or the caller sees the
//               negotiation as line noise. Three jobs, and each one is
//               there for a reason this project already paid for:
//
//               1. Take the IAC commands out of the board's stream. Without
//                  it a browser terminal draws the negotiation.
//               2. Double 0xFF on the way out, because a telnet stream
//                  cannot carry a bare one.
//               3. Speak FIRST. The board's connect-time detector gives a
//                  client that opens with IAC character mode and an
//                  immediate cursor-position probe, which is the good path:
//                  xterm.js answers the probe, so the board detects a real
//                  ANSI terminal rather than falling back to asking.
//
//               And one win that is nearly free: NAWS. The board honours
//               it, so telling it the caller's real width means a phone
//               held upright gets the board's 40-column layout and a phone
//               on its side gets 80, with nobody choosing anything.
//
//               Binary mode is requested in both directions, which is what
//               stops the board's NVT rule rewriting a bare CR. The core's
//               own lesson is in the asymmetry: ASKING is not AGREEING, so
//               nothing here changes behaviour until the far end has said
//               WILL or DO itself.
//
// See also:     line.cpp (who calls this), RFC 854, RFC 858, RFC 856,
//               RFC 1073 (NAWS: SB is sent only after DO)
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

namespace telnet {

// The commands and options this layer knows. Anything else is refused,
// which is the whole policy: a half-implemented option is worse than a
// refused one, because the far end then waits for something.
enum : uint8_t {
    SE = 240, SB = 250, WILL = 251, WONT = 252, DO = 253, DONT = 254, IAC = 255,
    OPT_BINARY = 0, OPT_ECHO = 1, OPT_SGA = 3, OPT_NAWS = 31,
};

struct State {
    uint8_t  st       = 0;      // 0 data, 1 after IAC, 2 after DO/DONT/WILL/WONT, 3 in SB, 4 IAC in SB
    uint8_t  cmd      = 0;      // which of DO/DONT/WILL/WONT we are completing
    bool     nawsOk   = false;  // the board said DO NAWS, so SB NAWS is welcome
    bool     binIn    = false;  // the board said WILL BINARY: its CR is its own
    bool     binOut   = false;  // the board said DO BINARY: our CR is ours
    uint16_t sbSeen   = 0;      // bytes swallowed in this subnegotiation, bounded
};

// Enough for the longest burst this layer can answer in one pass: a
// handful of three-byte replies and one NAWS subnegotiation.
constexpr size_t kReplyMax = 64;

// Takes the IAC traffic out of `buf` in place and returns how many data
// bytes are left. Anything that must be said back is appended to `reply`
// (up to kReplyMax; a reply that will not fit is dropped rather than
// truncated, because half a command is worse than none and the far end
// will ask again).
size_t filter(State& s, uint8_t* buf, size_t n,
              uint8_t* reply, size_t replyCap, size_t& replyLen);

// Doubles 0xFF on the way to the board. Returns bytes written, or 0 if
// `cap` could not hold the worst case, which the caller sizes for.
size_t escape(const uint8_t* in, size_t n, uint8_t* out, size_t cap);

// What the gateway says before the board has said anything: the IAC-first
// opening that gets character mode and the immediate probe.
size_t hello(uint8_t* out, size_t cap);

// The caller's window, once the board has said DO NAWS. Writes nothing if
// it has not (RFC 1073), which is why this takes the State.
size_t naws(const State& s, uint8_t* out, size_t cap, uint16_t cols, uint16_t rows);

}  // namespace telnet

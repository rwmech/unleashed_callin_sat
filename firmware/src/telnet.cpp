// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/telnet.cpp
// Module:       The telnet client on the IP uplink
//
// Purpose:      telnet.h says what and why. This is the state machine.
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
#include "telnet.h"

namespace telnet {
namespace {

// A subnegotiation from the board is swallowed, but not without a bound: a
// stream that never sends SE would otherwise leave this layer eating data
// for ever and the caller would see a dead terminal rather than a fault.
// 64 is far beyond anything the board sends; past it, assume the stream is
// confused and go back to reading data.
constexpr uint16_t kSbMax = 64;

inline void say(uint8_t* reply, size_t cap, size_t& len, uint8_t cmd, uint8_t opt) {
#if GW_TEST_BREAK != TBREAK_REPLY_ROOM
    if (len + 3 > cap) return;   // dropped whole; see telnet.h
#else
    if (len >= cap) return;      // the broken form: truncates
#endif
    reply[len++] = IAC;
    reply[len++] = cmd;
    reply[len++] = opt;
}

}  // namespace

size_t filter(State& s, uint8_t* buf, size_t n,
              uint8_t* reply, size_t replyCap, size_t& replyLen) {
    replyLen = 0;
    size_t out = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = buf[i];
        switch (s.st) {
            case 0:
                if (c == IAC) { s.st = 1; }
                else          { buf[out++] = c; }
                break;

            case 1:
                switch (c) {
                    case IAC:                        // a doubled 0xFF is one data byte
                        buf[out++] = IAC; s.st = 0; break;
                    case DO: case DONT: case WILL: case WONT:
                        s.cmd = c; s.st = 2; break;
                    case SB:
                        s.sbSeen = 0; s.st = 3; break;
                    default:
                        // Every other command (NOP, GA, AYT, the rest) has
                        // no argument and needs no answer from a client that
                        // is not pretending to be a terminal driver.
                        s.st = 0; break;
                }
                break;

            case 2: {
                const uint8_t opt = c;
                s.st = 0;
                if (s.cmd == DO) {
                    // What we agree to do, and nothing else.
                    if (opt == OPT_SGA)         { say(reply, replyCap, replyLen, WILL, opt); }
                    else if (opt == OPT_NAWS)   { say(reply, replyCap, replyLen, WILL, opt);
                                                  s.nawsOk = true; }
                    else if (opt == OPT_BINARY) { say(reply, replyCap, replyLen, WILL, opt);
                                                  s.binOut = true; }
                    else                        { say(reply, replyCap, replyLen, WONT, opt); }
                } else if (s.cmd == DONT) {
                    if (opt == OPT_NAWS)        { s.nawsOk = false; }
                    if (opt == OPT_BINARY)      { s.binOut = false; }
                    // WONT is the only honest answer to DONT, and saying it
                    // unconditionally cannot loop: DONT is never a reply to
                    // WONT.
                    say(reply, replyCap, replyLen, WONT, opt);
                } else if (s.cmd == WILL) {
                    // What we are happy for the board to do. It echoes, so
                    // ECHO is wanted; binary from its side stops it padding
                    // a bare CR.
                    if (opt == OPT_ECHO || opt == OPT_SGA) {
                        say(reply, replyCap, replyLen, DO, opt);
                    } else if (opt == OPT_BINARY) {
                        say(reply, replyCap, replyLen, DO, opt);
                        s.binIn = true;
                    } else {
                        say(reply, replyCap, replyLen, DONT, opt);
                    }
                } else {  // WONT
                    if (opt == OPT_BINARY) s.binIn = false;
                    say(reply, replyCap, replyLen, DONT, opt);
                }
                break;
            }

            case 3:
                if (c == IAC) { s.st = 4; }
#if GW_TEST_BREAK != TBREAK_SB_BOUND
                else if (++s.sbSeen > kSbMax) { s.st = 0; }
#else
                else { ++s.sbSeen; }
#endif
                break;

            case 4:
                // IAC SE ends it. IAC IAC is a data byte inside it, still
                // swallowed, and still counted so a stream of them cannot
                // hold this layer here. Anything else is a confused stream
                // and the safe reading is that the subnegotiation ended.
                if (c == IAC && s.sbSeen < kSbMax) {
                    ++s.sbSeen;
                    s.st = 3;
                } else {
                    s.st = 0;
                }
                break;

            default:
                s.st = 0;
                break;
        }
    }
    return out;
}

size_t escape(const uint8_t* in, size_t n, uint8_t* out, size_t cap) {
    // All or nothing: half an escaped burst would put a bare 0xFF on the
    // wire at the split and the board would read the next byte as a
    // command. The caller sizes `cap` at 2n for exactly this reason.
#if GW_TEST_BREAK != TBREAK_ESCAPE_CAP
    if (cap < n * 2) return 0;
#endif
    size_t w = 0;
    for (size_t i = 0; i < n; ++i) {
        out[w++] = in[i];
        if (in[i] == IAC) out[w++] = IAC;
    }
    return w;
}

size_t hello(uint8_t* out, size_t cap) {
    static const uint8_t k[] = {
        IAC, WILL, OPT_SGA,      // we will not wait to be asked for a turn
        IAC, DO,   OPT_SGA,      // and the board need not either
        IAC, WILL, OPT_NAWS,     // we can say how wide the caller is
        IAC, WILL, OPT_BINARY,   // our bytes are bytes
        IAC, DO,   OPT_BINARY,   // and so are the board's, please
    };
    if (cap < sizeof(k)) return 0;
    for (size_t i = 0; i < sizeof(k); ++i) out[i] = k[i];
    return sizeof(k);
}

size_t naws(const State& s, uint8_t* out, size_t cap, uint16_t cols, uint16_t rows) {
    if (!s.nawsOk) return 0;
    if (cap < 13) return 0;      // the worst case: both figures doubled
    size_t w = 0;
    out[w++] = IAC; out[w++] = SB; out[w++] = OPT_NAWS;
    const uint8_t v[4] = { static_cast<uint8_t>(cols >> 8), static_cast<uint8_t>(cols & 0xFF),
                           static_cast<uint8_t>(rows >> 8), static_cast<uint8_t>(rows & 0xFF) };
    for (uint8_t b : v) {
        out[w++] = b;
        // Inside a subnegotiation a 0xFF is still doubled (RFC 855), which
        // bites at exactly 255 columns and would otherwise end the SB early.
#if GW_TEST_BREAK != TBREAK_NAWS_IAC
        if (b == IAC) out[w++] = IAC;
#endif
    }
    out[w++] = IAC; out[w++] = SE;
    return w;
}

}  // namespace telnet

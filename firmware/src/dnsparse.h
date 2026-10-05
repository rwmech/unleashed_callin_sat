// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/dnsparse.h
// Module:       The DNS wire format, with no platform in it
//
// Purpose:      The pure half of the DNS redirector: reading a question off
//              the wire and building the reply. No sockets, no tasks, no
//              ESP-IDF, so it can be tested on a host against RFC 1035
//              rather than against a board.
//
//              It is a header of its own because of WHO SENDS IT. The
//              responder listens on an access point anybody in radio range
//              can join, so every byte here comes from a stranger, and the
//              deliverable is a parser that CANNOT be made to loop or to
//              read out of bounds - not coverage. host/test_dns.cpp drives
//              it with the malformations real resolvers have been sent, and
//              proves each guard by removing it (GW_TEST_BREAK) and showing
//              which checks then fail.
//
//              Same shape the core uses for its own pure halves
//              (forums_ptr.h, panel_photo_fit.h, satsched.h): the logic
//              worth testing is lifted into a header with no platform, and
//              the .cpp keeps the sockets.
//
// See also:     dns.cpp (the socket and the task), RFC 1035 §4
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
#include <cstring>

namespace dnsparse {

// A DNS message over UDP is 512 bytes without EDNS0 (RFC 1035 §4.2.1,
// "Messages carried by UDP are restricted to 512 bytes"). A question
// longer than that is not one this box needs to answer.
constexpr size_t kMsgMax = 512;

constexpr uint16_t kTypeA    = 1;
constexpr uint16_t kTypeAAAA = 28;
constexpr uint16_t kClassIn  = 1;

// RFC 1035 §2.3.4: a label is at most 63 octets and a name at most 255.
constexpr size_t kLabelMax = 63;
constexpr size_t kNameMax  = 255;

// A switch that removes the guards one at a time, for the host tests only.
// Each value disables exactly one check, so a test can prove it is THAT
// check which catches a given packet rather than some other one further
// down. Never defined in a firmware build.
//
// MACROS AND NOT AN ENUM, and the reason is worth keeping: the
// preprocessor cannot see an enumerator. Written as an enum, every
// `#if GW_TEST_BREAK != BREAK_x` compared 0 against an unknown identifier,
// which `#if` reads as 0 - so the condition was false and EVERY GUARD WAS
// COMPILED OUT, of the firmware as well as the tests. It was caught within
// a minute by -Werror on an unused parameter, because removing the last
// check left `cap` unread; without -Wall -Wextra -Werror this would have
// shipped as a DNS parser with no bounds checks at all, on an open
// network. That is the whole argument for building the tests that way.
#ifndef GW_TEST_BREAK
#define GW_TEST_BREAK 0
#endif
#define BREAK_NONE         0
#define BREAK_POINTER      1   /* allow a compression pointer in a question */
#define BREAK_NAME_LEN     2   /* allow a name past 255 octets */
#define BREAK_QUESTION_END 3   /* do not require QTYPE and QCLASS present */
#define BREAK_HEADER_LEN   4   /* do not require a 12-octet header */
#define BREAK_ANSWER_ROOM  5   /* do not check there is room for the answer */
#define BREAK_QDCOUNT      6   /* accept a question count other than one */

inline uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline void wr16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

// Walks one question's name and returns the offset just past its QTYPE and
// QCLASS, or 0 if the question is malformed. `nameLen` gets the name's
// length in octets as counted by RFC 1035 §2.3.4.
//
// Every refusal below is a packet a real resolver has been sent:
//
//  - a length octet of 64 or more. The two high bits set is a COMPRESSION
//    POINTER (§4.1.4), which is illegal in a question, and following one is
//    how a parser is made to loop for ever on a packet that points at
//    itself or on two that point at each other. 0x40 and 0x80 are reserved
//    and refused with it;
//  - a name that never terminates before the end of the packet;
//  - a name whose octets add up past 255, the protocol's own limit;
//  - a question whose QTYPE and QCLASS are not both inside the packet.
//
// `at` strictly increases on every iteration, which is what makes the loop
// terminate whatever the input: there is no path that revisits an offset.
inline size_t walkName(const uint8_t* msg, size_t n, size_t at, size_t& nameLen) {
    nameLen = 0;
    while (at < n) {
        const uint8_t len = msg[at];
        if (len == 0) {
            ++at;
            // QTYPE and QCLASS, four octets, must also be inside the packet.
#if GW_TEST_BREAK == BREAK_QUESTION_END
            return at + 4;
#else
            return (at + 4 <= n) ? at + 4 : 0;
#endif
        }
#if GW_TEST_BREAK != BREAK_POINTER
        if (len > kLabelMax) return 0;          // a pointer, or a reserved form
#endif
#if GW_TEST_BREAK != BREAK_NAME_LEN
        if (nameLen + len + 1 > kNameMax) return 0;
#endif
        at      += static_cast<size_t>(len) + 1;
        nameLen += static_cast<size_t>(len) + 1;
    }
    return 0;                                   // ran off the end
}

// What a question turned out to be.
struct Question {
    bool     ok     = false;   // worth answering at all
    bool     isA    = false;   // an A question, so it gets an address
    size_t   end    = 0;       // offset just past the question
    uint16_t qtype  = 0;
    uint16_t qclass = 0;
    uint16_t flags  = 0;       // the question's own header flags
};

// Reads the header and the single question. Refuses, in this order: a
// message shorter than a header; one that is already an answer (QR set),
// which is not ours to answer; one whose opcode is not a standard query;
// one that does not carry exactly one question; a malformed name; and a
// class other than IN.
inline Question read(const uint8_t* msg, size_t n) {
    Question q;
#if GW_TEST_BREAK != BREAK_HEADER_LEN
    if (n < 12) return q;
#endif
    q.flags = rd16(msg + 2);
    if (q.flags & 0x8000) return q;                     // already an answer
    if (((q.flags >> 11) & 0x0F) != 0) return q;         // not OPCODE 0

#if GW_TEST_BREAK != BREAK_QDCOUNT
    if (rd16(msg + 4) != 1) return q;                   // exactly one question
#endif

    size_t nameLen = 0;
    const size_t end = walkName(msg, n, 12, nameLen);
    if (!end) return q;

    q.qtype  = rd16(msg + end - 4);
    q.qclass = rd16(msg + end - 2);
    if (q.qclass != kClassIn) return q;

    q.end  = end;
    q.isA  = (q.qtype == kTypeA);
    q.ok   = true;
    return q;
}

// Builds the reply into `out` and returns its length, or 0 if there is no
// room. `addr` is the address in NETWORK order, as it goes on the wire.
//
// The reply is the question with its header rewritten, which is what keeps
// the name's octets exactly as they were asked and lets the answer point
// at offset 12 rather than repeating them (§4.1.4).
//
// An A question gets one answer record. Anything else gets NOERROR with no
// records, which tells a resolver "there is no AAAA here" and stops it
// waiting; NXDOMAIN would say the NAME does not exist, and some resolvers
// then give up on the A as well.
inline size_t reply(const uint8_t* msg, size_t n, const Question& q,
                    uint32_t addr, uint8_t* out, size_t cap) {
    if (!q.ok) return 0;
#if GW_TEST_BREAK != BREAK_ANSWER_ROOM
    if (q.end + 16 > cap) return 0;
#endif
    if (q.end > n) return 0;
    memcpy(out, msg, q.end);
    size_t w = q.end;

    // QR=1, AA=1, RD copied from the question, RA=0, RCODE=0 (§4.1.1).
    wr16(out + 2, static_cast<uint16_t>(0x8400 | (q.flags & 0x0100)));
    wr16(out + 4, 1);                                   // QDCOUNT
    wr16(out + 6, q.isA ? 1 : 0);                       // ANCOUNT
    wr16(out + 8, 0);                                   // NSCOUNT
    wr16(out + 10, 0);                                  // ARCOUNT

    if (q.isA) {
        out[w++] = 0xC0;                                // a pointer to offset 12
        out[w++] = 0x0C;
        wr16(out + w, kTypeA);   w += 2;
        wr16(out + w, kClassIn); w += 2;
        // TTL 0. A resolver that cached this box's address and then left
        // would carry the hijack with it: every name it looked up for the
        // next TTL would resolve to an address that is no longer anything.
        out[w++] = 0; out[w++] = 0; out[w++] = 0; out[w++] = 0;
        wr16(out + w, 4); w += 2;                       // RDLENGTH
        memcpy(out + w, &addr, 4); w += 4;              // RDATA, network order
    }
    return w;
}

}  // namespace dnsparse

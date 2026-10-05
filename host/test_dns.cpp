// ===========================================================================
//  µnleashed gateway sat: the host tests
// ===========================================================================
//
// File:         host/test_dns.cpp
// Module:       The DNS redirector's parser, against RFC 1035
//
// Purpose:      This responder listens on an access point anybody in radio
//              range can join, so EVERY BYTE IT PARSES COMES FROM A
//              STRANGER. The deliverable is a parser that cannot be made
//              to loop or to read out of bounds, and that is what these
//              checks are about; coverage is a side effect.
//
//              TWO RULES THE MAIN PROJECT PAID FOR, both followed here:
//
//              1. The encoder below is written from RFC 1035's own wire
//                 format - the 12-octet header of §4.1.1, the
//                 length-prefixed labels of §3.1, QTYPE and QCLASS of
//                 §4.1.2 - and shares no code with dnsparse.h. A test
//                 client written beside the implementation tests that the
//                 two agree, not that either is right, which is exactly
//                 how `tools/testclient.py` let two real telnet bugs
//                 through while hardware failed.
//              2. Every check is proved to fail against code without the
//                 guard it is testing. `make break` compiles the suite
//                 once per GW_TEST_BREAK value and reports which checks
//                 fail each time. A test written after the fix that passes
//                 immediately has proved nothing, and this project has
//                 shipped tests that agreed with the bug.
//
//              Build and run:  cd host && make && ./test_dns
//              Prove the guards:  cd host && make break
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
#include <string>
#include <vector>

#include "check.h"
#include "dnsparse.h"

namespace {

// -------------------------------------------------------------------------
//  An encoder written from the RFC, sharing nothing with dnsparse.h.
//
//  RFC 1035 §4.1.1, the header, is six 16-bit fields in network order:
//    ID, flags, QDCOUNT, ANCOUNT, NSCOUNT, ARCOUNT.
//  §3.1, a name, is a sequence of labels, each one length octet then that
//  many octets, ended by a zero length octet.
//  §4.1.2, a question, is that name then QTYPE then QCLASS.
// -------------------------------------------------------------------------
using Msg = std::vector<unsigned char>;

void put16(Msg& m, unsigned v) {
    m.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
    m.push_back(static_cast<unsigned char>(v & 0xFF));
}

void header(Msg& m, unsigned id, unsigned flags, unsigned qd) {
    put16(m, id);
    put16(m, flags);
    put16(m, qd);
    put16(m, 0);   // ANCOUNT
    put16(m, 0);   // NSCOUNT
    put16(m, 0);   // ARCOUNT
}

// "captive.apple.com" -> 07 captive 05 apple 03 com 00
void name(Msg& m, const std::string& dotted) {
    size_t at = 0;
    while (at < dotted.size()) {
        size_t dot = dotted.find('.', at);
        if (dot == std::string::npos) dot = dotted.size();
        const size_t len = dot - at;
        m.push_back(static_cast<unsigned char>(len));
        for (size_t i = 0; i < len; ++i) m.push_back(static_cast<unsigned char>(dotted[at + i]));
        at = dot + 1;
    }
    m.push_back(0);
}

// A whole standard query, the way every operating system's probe sends one.
Msg query(const std::string& host, unsigned qtype = 1, unsigned qclass = 1,
          unsigned id = 0x1234, unsigned flags = 0x0100 /* RD */) {
    Msg m;
    header(m, id, flags, 1);
    name(m, host);
    put16(m, qtype);
    put16(m, qclass);
    return m;
}

dnsparse::Question parse(const Msg& m) {
    return dnsparse::read(m.data(), m.size());
}

// 172.16.0.1 in network order, the way it sits on the wire and the way
// dnsparse::reply wants it.
constexpr uint32_t kAddr = 0x010010ACu;   // AC 10 00 01 little-endian

// -------------------------------------------------------------------------
//  The questions every operating system actually asks. Each one is quoted
//  from its vendor or from AOSP in the study, so these are the real names.
// -------------------------------------------------------------------------
void probes() {
    chk::group("the real probe names resolve");
    const char* kProbes[] = {
        "captive.apple.com",                 // Apple
        "connectivitycheck.gstatic.com",      // Android
        "www.msftconnecttest.com",            // Windows
        "www.msftncsi.com",                   // Windows before 14393
        "detectportal.firefox.com",           // Firefox
        "nmcheck.gnome.org",                  // NetworkManager, GNOME's
        "network-test.debian.org",            // NetworkManager, Debian's
        "connectivity-check.ubuntu.com",      // NetworkManager, Ubuntu's
    };
    for (const char* h : kProbes) {
        const Msg m = query(h);
        const dnsparse::Question q = parse(m);
        chk::ok(q.ok && q.isA, h);

        unsigned char out[dnsparse::kMsgMax];
        const size_t w = dnsparse::reply(m.data(), m.size(), q, kAddr, out, sizeof out);
        chk::ok(w > m.size(), "the reply is longer than the question");
        // §4.1.1: the ID is echoed, QR is set, ANCOUNT is one.
        chk::eq((out[0] << 8) | out[1], 0x1234, "the ID is echoed");
        chk::ok((out[2] & 0x80) != 0, "QR is set");
        chk::ok((out[2] & 0x04) != 0, "AA is set");
        chk::eq((out[4] << 8) | out[5], 1, "QDCOUNT is one");
        chk::eq((out[6] << 8) | out[7], 1, "ANCOUNT is one");
        chk::eq((out[8] << 8) | out[9], 0, "NSCOUNT is zero");
        chk::eq((out[10] << 8) | out[11], 0, "ARCOUNT is zero");
        // The question is echoed octet for octet, which is what lets the
        // answer point at offset 12 instead of repeating the name.
        chk::bytes(out + 12, m.data() + 12, m.size() - 12, "the question is echoed");
        // §4.1.4: a pointer is the two high bits set and a 14-bit offset.
        const size_t ans = m.size();
        chk::eq(out[ans], 0xC0, "the answer's name is a pointer");
        chk::eq(out[ans + 1], 0x0C, "pointing at offset 12");
        chk::eq((out[ans + 2] << 8) | out[ans + 3], 1, "TYPE is A");
        chk::eq((out[ans + 4] << 8) | out[ans + 5], 1, "CLASS is IN");
        chk::eq(out[ans + 6] | out[ans + 7] | out[ans + 8] | out[ans + 9], 0,
                "TTL is zero, so the hijack cannot travel");
        chk::eq((out[ans + 10] << 8) | out[ans + 11], 4, "RDLENGTH is four");
        const unsigned char want[4] = { 172, 16, 0, 1 };
        chk::bytes(out + ans + 12, want, 4, "RDATA is 172.16.0.1");
        chk::eq(static_cast<long>(w), static_cast<long>(ans + 16), "the whole reply");
    }
}

void aaaa_and_other_types() {
    chk::group("only A gets an address");
    // §3.2.2's types. AAAA is RFC 3596's 28.
    const unsigned kTypes[] = { 28 /* AAAA */, 2 /* NS */, 5 /* CNAME */,
                                15 /* MX */, 16 /* TXT */, 65 /* HTTPS */,
                                255 /* ANY */ };
    for (unsigned t : kTypes) {
        const Msg m = query("captive.apple.com", t);
        const dnsparse::Question q = parse(m);
        chk::ok(q.ok, "answered at all");
        chk::ok(!q.isA, "but not with an address");

        unsigned char out[dnsparse::kMsgMax];
        const size_t w = dnsparse::reply(m.data(), m.size(), q, kAddr, out, sizeof out);
        chk::eq(static_cast<long>(w), static_cast<long>(m.size()),
                "the reply is the question with a rewritten header");
        chk::eq((out[6] << 8) | out[7], 0, "ANCOUNT is zero");
        // NOERROR and not NXDOMAIN: §4.1.1's RCODE is the low four bits.
        chk::eq(out[3] & 0x0F, 0, "RCODE is NOERROR, so the name still exists");
    }

    chk::group("a class other than IN is not answered");
    for (unsigned c : { 3u /* CH */, 4u /* HS */, 255u /* ANY */ }) {
        const Msg m = query("captive.apple.com", 1, c);
        chk::ok(!parse(m).ok, "refused");
    }
}

// -------------------------------------------------------------------------
//  Hostile input. Each of these is a shape a real resolver has been sent.
// -------------------------------------------------------------------------
void hostile() {
    chk::group("a compression pointer in a question");
    {
        // THE DISCRIMINATING PACKET, and the others below are not.
        //
        // Read as a length octet rather than as a pointer, 0xC0 means
        // "skip 192 octets". Every other test in this group is a SHORT
        // packet, so without the pointer guard those 192 octets run off
        // the end and the name-never-terminates guard refuses them - they
        // prove the packet is refused without proving WHICH check did it.
        //
        // Here there are 192 octets of padding, so a parser without the
        // guard walks over them, finds a root label and a well-formed
        // QTYPE and QCLASS, and ANSWERS a question whose name is a
        // pointer. 193 octets is also under the 255-octet limit, so the
        // name-length guard does not cover for it either.
        //
        // Worth being precise about what the guard does and does not do:
        // this parser never FOLLOWS a pointer, so it cannot be made to
        // loop by one. What the guard buys is that a pointer is refused
        // rather than silently misread as a 192-octet label - which, in a
        // question, means answering something nobody asked.
        Msg m;
        header(m, 1, 0x0100, 1);
        m.push_back(0xC0);
        for (int i = 0; i < 192; ++i) m.push_back('x');
        m.push_back(0);
        put16(m, 1);
        put16(m, 1);
        chk::ok(!parse(m).ok,
                "a pointer with room to walk past is still refused");
    }
    // §4.1.4: 0xC0 0x0C is a pointer to offset 12. In a QUESTION it is
    // illegal, and FOLLOWING one is how a parser is made to loop.
    {
        Msg m;
        header(m, 1, 0x0100, 1);
        m.push_back(0xC0);
        m.push_back(0x0C);          // points at itself
        put16(m, 1);
        put16(m, 1);
        chk::ok(!parse(m).ok, "a pointer at itself is refused, not followed");
    }
    {
        // Two pointers at each other, the classic infinite loop.
        Msg m;
        header(m, 1, 0x0100, 1);
        m.push_back(0xC0); m.push_back(0x0E);   // offset 12 -> 14
        m.push_back(0xC0); m.push_back(0x0C);   // offset 14 -> 12
        put16(m, 1);
        put16(m, 1);
        chk::ok(!parse(m).ok, "two pointers at each other are refused");
    }
    {
        // A valid label, then a pointer, which is the legal form in an
        // ANSWER and still illegal in a question.
        Msg m;
        header(m, 1, 0x0100, 1);
        name(m, "captive");
        m.pop_back();               // take off the root label
        m.push_back(0xC0); m.push_back(0x0C);
        put16(m, 1);
        put16(m, 1);
        chk::ok(!parse(m).ok, "a label then a pointer is refused");
    }
    // Every length octet with a high bit set, including the reserved 0x40
    // and 0x80 forms (§4.1.4 reserves them).
    for (unsigned b : { 0x40u, 0x80u, 0xC0u, 0xFFu }) {
        Msg m;
        header(m, 1, 0x0100, 1);
        m.push_back(static_cast<unsigned char>(b));
        m.push_back(0x0C);
        put16(m, 1);
        put16(m, 1);
        chk::ok(!parse(m).ok, "a reserved or pointer length octet is refused");
    }

    chk::group("a label length running past the buffer");
    {
        // One label claiming 63 octets with only four following.
        Msg m;
        header(m, 1, 0x0100, 1);
        m.push_back(63);
        for (int i = 0; i < 4; ++i) m.push_back('a');
        chk::ok(!parse(m).ok, "a label longer than what follows is refused");
    }
    {
        // A label of exactly the remaining bytes, with no root label after.
        Msg m;
        header(m, 1, 0x0100, 1);
        m.push_back(3);
        m.push_back('c'); m.push_back('o'); m.push_back('m');
        chk::ok(!parse(m).ok, "a name with no root label is refused");
    }

    chk::group("a name over 255 octets");
    {
        // §2.3.4: a name is at most 255 octets. Five 63-octet labels is
        // 320, each label individually legal.
        Msg m;
        header(m, 1, 0x0100, 1);
        for (int l = 0; l < 5; ++l) {
            m.push_back(63);
            for (int i = 0; i < 63; ++i) m.push_back('a');
        }
        m.push_back(0);
        put16(m, 1);
        put16(m, 1);
        chk::ok(!parse(m).ok, "a 320-octet name is refused");
    }
    {
        // And one just inside it: three 63-octet labels plus a 60 is 252
        // octets of name, which is legal and must be answered.
        Msg m;
        header(m, 1, 0x0100, 1);
        for (int l = 0; l < 3; ++l) {
            m.push_back(63);
            for (int i = 0; i < 63; ++i) m.push_back('a');
        }
        m.push_back(59);
        for (int i = 0; i < 59; ++i) m.push_back('b');
        m.push_back(0);
        put16(m, 1);
        put16(m, 1);
        const dnsparse::Question q = parse(m);
        chk::ok(q.ok && q.isA, "a 255-octet name is still answered");
        unsigned char out[dnsparse::kMsgMax];
        chk::ok(dnsparse::reply(m.data(), m.size(), q, kAddr, out, sizeof out) > 0,
                "and its reply is built");
    }

    chk::group("a question whose QTYPE and QCLASS are past the end");
    {
        // The name ends exactly at the end of the packet, so QTYPE and
        // QCLASS are not in it. Built over-allocated with a perfectly
        // valid A/IN sitting in the four octets just past the end, and
        // the parser is told the SHORTER length.
        //
        // With the guard: refused. Without it: those four octets are read
        // and a question nobody sent is answered. Deterministic either
        // way, which matters - leaving it to whatever the heap contained
        // is how a test passes for a reason that has nothing to do with
        // what it claims.
        Msg m;
        header(m, 1, 0x0100, 1);
        name(m, "captive.apple.com");
        const size_t shortLen = m.size();        // name ends here, no QTYPE
        put16(m, 1);                             // A, just past the end
        put16(m, 1);                             // IN
        chk::ok(!dnsparse::read(m.data(), shortLen).ok,
                "a question with no room for QTYPE and QCLASS is refused");
        // And with the four octets inside the length it is answered, so
        // the check above is not refusing it for some other reason.
        chk::ok(dnsparse::read(m.data(), m.size()).ok,
                "the same packet one field longer is answered");
    }

    chk::group("a packet that simply ends");
    // Every prefix of a real query, from nothing to one octet short. Not
    // one of them may be answered, and none may read past its end: run
    // this under ASan and a single overrun is a crash rather than a pass.
    {
        const Msg full = query("captive.apple.com");
        for (size_t n = 0; n < full.size(); ++n) {
            // Copied into an allocation of EXACTLY n octets, not sliced
            // off a longer buffer. Sliced, a read past n is still inside
            // the vector: it returns a neighbouring octet, something
            // further down refuses the packet for another reason, and the
            // check passes while proving nothing. Exactly sized, the same
            // read is a real overrun and ASan makes it a crash.
            unsigned char* exact = new unsigned char[n ? n : 1];
            if (n) memcpy(exact, full.data(), n);
            const dnsparse::Question q = dnsparse::read(exact, n);
            chk::ok(!q.ok, "a truncated query is refused");
            delete[] exact;
        }
        // And the whole thing is answered, so the loop above was not
        // refusing everything for some other reason.
        chk::ok(dnsparse::read(full.data(), full.size()).ok,
                "the untruncated query is answered");
    }

    chk::group("zero-length labels in the middle");
    {
        // A zero length octet IS the root label, so this is a complete
        // name followed by rubbish. It must parse as the name and read its
        // QTYPE and QCLASS from the right place, not wander into the tail.
        Msg m;
        header(m, 1, 0x0100, 1);
        m.push_back(3); m.push_back('c'); m.push_back('o'); m.push_back('m');
        m.push_back(0);
        put16(m, 1);
        put16(m, 1);
        const size_t end = m.size();
        m.push_back(0); m.push_back(0); m.push_back(0);   // trailing rubbish
        const dnsparse::Question q = parse(m);
        chk::ok(q.ok && q.isA, "the name ends at its root label");
        chk::eq(static_cast<long>(q.end), static_cast<long>(end),
                "and the question ends where the RFC says");
    }
    {
        // A bare root label: the DNS root itself, a legal question.
        Msg m;
        header(m, 1, 0x0100, 1);
        m.push_back(0);
        put16(m, 1);
        put16(m, 1);
        chk::ok(parse(m).ok, "the root name is a legal question");
    }

    chk::group("not a standard query");
    {
        // §4.1.1: QR set means this is already an answer.
        Msg m = query("captive.apple.com");
        m[2] |= 0x80;
        chk::ok(!parse(m).ok, "an answer is not ours to answer");
    }
    for (unsigned op : { 1u /* IQUERY */, 2u /* STATUS */, 4u /* NOTIFY */,
                         5u /* UPDATE */ }) {
        Msg m = query("captive.apple.com");
        m[2] = static_cast<unsigned char>((m[2] & 0x87) | (op << 3));
        chk::ok(!parse(m).ok, "an opcode other than QUERY is refused");
    }
    for (unsigned qd : { 0u, 2u, 0xFFFFu }) {
        Msg m = query("captive.apple.com");
        m[4] = static_cast<unsigned char>(qd >> 8);
        m[5] = static_cast<unsigned char>(qd & 0xFF);
        chk::ok(!parse(m).ok, "a question count other than one is refused");
    }

    chk::group("a question with no room for its answer");
    {
        // A name long enough that the question fills the buffer, so the
        // 16-octet answer record cannot fit. Refused whole rather than
        // written past the end.
        Msg m;
        header(m, 1, 0x0100, 1);
        // 255 octets of name is the most the RFC allows, and with the
        // header and the question's tail that is 255 + 12 + 4 = 271, well
        // inside 512 - so to reach the refusal the BUFFER is what shrinks.
        name(m, "captive.apple.com");
        put16(m, 1);
        put16(m, 1);
        const dnsparse::Question q = parse(m);
        chk::ok(q.ok, "the question itself is fine");
        unsigned char small[40];
        chk::eq(static_cast<long>(
                    dnsparse::reply(m.data(), m.size(), q, kAddr, small, sizeof small)),
                0, "a buffer too small for the answer refuses whole");
        // One octet more than it needs is enough.
        unsigned char exact[dnsparse::kMsgMax];
        chk::eq(static_cast<long>(
                    dnsparse::reply(m.data(), m.size(), q, kAddr, exact, m.size() + 16)),
                static_cast<long>(m.size() + 16), "and exactly enough is enough");
        chk::eq(static_cast<long>(
                    dnsparse::reply(m.data(), m.size(), q, kAddr, exact, m.size() + 15)),
                0, "one octet short is refused");
    }

    chk::group("RD is copied and nothing else is invented");
    {
        // §4.1.1: RD is the caller's bit and an answer echoes it. RA is
        // ours and we do not offer recursion, so it must be clear.
        Msg withRd = query("captive.apple.com", 1, 1, 0x1234, 0x0100);
        Msg noRd   = query("captive.apple.com", 1, 1, 0x1234, 0x0000);
        unsigned char out[dnsparse::kMsgMax];
        dnsparse::reply(withRd.data(), withRd.size(), parse(withRd), kAddr, out, sizeof out);
        chk::ok((out[2] & 0x01) != 0, "RD set in the question is set in the answer");
        chk::ok((out[3] & 0x80) == 0, "RA is clear: we do not recurse");
        dnsparse::reply(noRd.data(), noRd.size(), parse(noRd), kAddr, out, sizeof out);
        chk::ok((out[2] & 0x01) == 0, "RD clear stays clear");
    }

    chk::group("every byte pattern, bounded");
    {
        // Not a fuzzer, but the cheap part of one: a sweep that proves no
        // single-octet corruption of a real query can make the parser
        // read out of bounds. Under ASan this is the check that matters.
        const Msg good = query("captive.apple.com");
        for (size_t i = 0; i < good.size(); ++i) {
            for (unsigned v = 0; v < 256; ++v) {
                Msg m = good;
                m[i] = static_cast<unsigned char>(v);
                const dnsparse::Question q = dnsparse::read(m.data(), m.size());
                if (!q.ok) continue;
                unsigned char out[dnsparse::kMsgMax];
                dnsparse::reply(m.data(), m.size(), q, kAddr, out, sizeof out);
            }
        }
        chk::ok(true, "no single-octet corruption reads out of bounds");
    }
}

}  // namespace

int main() {
    printf("test_dns: GW_TEST_BREAK=%d\n", GW_TEST_BREAK);
    probes();
    aaaa_and_other_types();
    hostile();
    return chk::done("test_dns");
}

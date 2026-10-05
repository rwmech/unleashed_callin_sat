// ===========================================================================
//  µnleashed gateway sat: the host tests
// ===========================================================================
//
// File:         host/test_telnet.cpp
// Module:       The telnet client on the IP uplink, against RFC 854
//
// Purpose:      The main project lost TWO ROUNDS to a telnet test client
//              written beside the server: it sent raw bytes and doubled
//              0xFF the way the board did, so every test passed while the
//              hardware failed - twice, once on binary negotiation and
//              once on CR padding. Its recorded conclusion is the rule
//              followed here:
//
//                  when a protocol is involved, test against somebody
//                  else's implementation. A client written next to the
//                  server tests that the two agree, not that either is
//                  right.
//
//              There is no lrzsz for telnet, so the next best thing is
//              used: every byte sequence below is written from the RFCs'
//              own tables and prose, quoted in the comments, and the
//              expected replies are spelt out as literals rather than
//              produced by calling telnet.cpp's own hello() or say().
//
//                RFC 854  the command codes, IAC IAC as a data 255, and
//                         "the other side may not have to respond"
//                RFC 855  the SB ... IAC SE subnegotiation frame
//                RFC 856  TRANSMIT-BINARY, option 0
//                RFC 857  ECHO, option 1
//                RFC 858  SUPPRESS-GO-AHEAD, option 3
//                RFC 1073 NAWS, option 31, and "the client sends SB only
//                         after the server has sent DO"
//
//              Build and run:  cd host && make && ./test_telnet
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
#include "telnet.h"

namespace {

// -------------------------------------------------------------------------
//  The codes, typed from RFC 854's own table rather than taken from
//  telnet.h. If the two ever disagree, that is the bug this duplication
//  exists to find.
//
//    RFC 854, "Command Name and Code":
//      SE 240  NOP 241  Data Mark 242  Break 243  Interrupt Process 244
//      Abort output 245  Are You There 246  Erase character 247
//      Erase Line 248  Go ahead 249  SB 250  WILL 251  WON'T 252
//      DO 253  DON'T 254  IAC 255
// -------------------------------------------------------------------------
constexpr unsigned char R_SE   = 240;
constexpr unsigned char R_NOP  = 241;
constexpr unsigned char R_DM   = 242;
constexpr unsigned char R_BRK  = 243;
constexpr unsigned char R_IP   = 244;
constexpr unsigned char R_AO   = 245;
constexpr unsigned char R_AYT  = 246;
constexpr unsigned char R_EC   = 247;
constexpr unsigned char R_EL   = 248;
constexpr unsigned char R_GA   = 249;
constexpr unsigned char R_SB   = 250;
constexpr unsigned char R_WILL = 251;
constexpr unsigned char R_WONT = 252;
constexpr unsigned char R_DO   = 253;
constexpr unsigned char R_DONT = 254;
constexpr unsigned char R_IAC  = 255;

// The option numbers, from their own RFCs.
constexpr unsigned char O_BINARY = 0;    // RFC 856
constexpr unsigned char O_ECHO   = 1;    // RFC 857
constexpr unsigned char O_SGA    = 3;    // RFC 858
constexpr unsigned char O_STATUS = 5;    // RFC 859
constexpr unsigned char O_TTYPE  = 24;   // RFC 1091
constexpr unsigned char O_NAWS   = 31;   // RFC 1073
constexpr unsigned char O_LINEMODE = 34; // RFC 1184

// telnet.h's own constants must equal the RFCs'. Checked, because the two
// copies are the point: a renumbered constant would otherwise be invisible.
void codes_agree() {
    chk::group("telnet.h agrees with RFC 854's table");
    chk::eq(telnet::IAC, R_IAC, "IAC is 255");
    chk::eq(telnet::SE, R_SE, "SE is 240");
    chk::eq(telnet::SB, R_SB, "SB is 250");
    chk::eq(telnet::WILL, R_WILL, "WILL is 251");
    chk::eq(telnet::WONT, R_WONT, "WONT is 252");
    chk::eq(telnet::DO, R_DO, "DO is 253");
    chk::eq(telnet::DONT, R_DONT, "DONT is 254");
    chk::eq(telnet::OPT_BINARY, O_BINARY, "TRANSMIT-BINARY is 0");
    chk::eq(telnet::OPT_ECHO, O_ECHO, "ECHO is 1");
    chk::eq(telnet::OPT_SGA, O_SGA, "SUPPRESS-GO-AHEAD is 3");
    chk::eq(telnet::OPT_NAWS, O_NAWS, "NAWS is 31");
}

// Runs a stream through the filter and reports the data and the reply.
struct Ran {
    std::vector<unsigned char> data;
    std::vector<unsigned char> reply;
};

Ran run(telnet::State& s, const std::vector<unsigned char>& in) {
    std::vector<unsigned char> buf = in;
    if (buf.empty()) buf.push_back(0);        // never a null pointer
    unsigned char rep[telnet::kReplyMax];
    size_t repLen = 0;
    const size_t got = telnet::filter(s, buf.data(), in.size(),
                                      rep, sizeof rep, repLen);
    Ran r;
    r.data.assign(buf.begin(), buf.begin() + static_cast<long>(got));
    r.reply.assign(rep, rep + repLen);
    return r;
}

// Fed one octet at a time, which is what a real socket does. The filter is
// a state machine across calls and this is the only way to find out.
Ran dribble(telnet::State& s, const std::vector<unsigned char>& in) {
    Ran all;
    for (unsigned char c : in) {
        const Ran r = run(s, { c });
        all.data.insert(all.data.end(), r.data.begin(), r.data.end());
        all.reply.insert(all.reply.end(), r.reply.begin(), r.reply.end());
    }
    return all;
}

void data_passes_through() {
    chk::group("data is data");
    telnet::State s;
    const std::vector<unsigned char> text = { 'H', 'e', 'l', 'l', 'o', '\r', '\n' };
    const Ran r = run(s, text);
    chk::eq(static_cast<long>(r.data.size()), 7, "seven octets of text survive");
    chk::bytes(r.data.data(), text.data(), 7, "unchanged");
    chk::eq(static_cast<long>(r.reply.size()), 0, "and nothing is said back");

    // Every octet from 0 to 254 is data. 255 is the one that is not.
    chk::group("every octet but 255 is data");
    for (unsigned v = 0; v < 255; ++v) {
        telnet::State s2;
        const Ran r2 = run(s2, { static_cast<unsigned char>(v) });
        chk::eq(static_cast<long>(r2.data.size()), 1, "one octet in, one out");
    }
    {
        telnet::State s2;
        const Ran r2 = run(s2, { R_IAC });
        chk::eq(static_cast<long>(r2.data.size()), 0, "a lone IAC is not data");
    }

    // RFC 854: "if a 255 is to be sent as data, it must be doubled".
    chk::group("IAC IAC is one data 255");
    {
        telnet::State s2;
        const Ran r2 = run(s2, { 'a', R_IAC, R_IAC, 'b' });
        const unsigned char want[3] = { 'a', 255, 'b' };
        chk::eq(static_cast<long>(r2.data.size()), 3, "three octets of data");
        chk::bytes(r2.data.data(), want, 3, "with a single 255 between them");
    }
    {
        // Split across two reads, which is where a state machine fails.
        telnet::State s2;
        const Ran a = run(s2, { 'a', R_IAC });
        const Ran b = run(s2, { R_IAC, 'b' });
        chk::eq(static_cast<long>(a.data.size()), 1, "the first half gives one octet");
        chk::eq(static_cast<long>(b.data.size()), 2, "the second gives the 255 and the b");
        chk::eq(b.data[0], 255, "and it is a 255");
    }
}

void commands_without_options() {
    // RFC 854 lists these as having no option and needing no answer from a
    // client that is not pretending to be a terminal driver. Each must be
    // swallowed whole and must not eat the data after it.
    chk::group("argument-less commands are swallowed");
    const unsigned char kNoArg[] = { R_NOP, R_DM, R_BRK, R_IP, R_AO, R_AYT,
                                     R_EC, R_EL, R_GA };
    for (unsigned char c : kNoArg) {
        telnet::State s;
        const Ran r = run(s, { 'a', R_IAC, c, 'b' });
        const unsigned char want[2] = { 'a', 'b' };
        chk::eq(static_cast<long>(r.data.size()), 2, "the command vanishes");
        chk::bytes(r.data.data(), want, 2, "and the data around it survives");
        chk::eq(static_cast<long>(r.reply.size()), 0, "with no answer");
    }
}

void negotiation() {
    // RFC 854: a party receiving DO for an option it will not perform
    // answers WON'T; receiving WILL for one it does not want, DON'T. The
    // expected replies below are written out as literals on purpose.
    chk::group("DO is answered WILL only for what we do");
    struct Case { unsigned char cmd, opt, wantCmd; const char* why; };
    const Case kDo[] = {
        { R_DO, O_SGA,    R_WILL, "DO SGA -> WILL SGA" },
        { R_DO, O_NAWS,   R_WILL, "DO NAWS -> WILL NAWS" },
        { R_DO, O_BINARY, R_WILL, "DO BINARY -> WILL BINARY" },
        { R_DO, O_ECHO,   R_WONT, "DO ECHO -> WONT: the board echoes" },
        { R_DO, O_TTYPE,  R_WONT, "DO TTYPE -> WONT: not implemented" },
        { R_DO, O_STATUS, R_WONT, "DO STATUS -> WONT" },
        { R_DO, O_LINEMODE, R_WONT, "DO LINEMODE -> WONT" },
        { R_WILL, O_ECHO,   R_DO,   "WILL ECHO -> DO: the board echoes" },
        { R_WILL, O_SGA,    R_DO,   "WILL SGA -> DO" },
        { R_WILL, O_BINARY, R_DO,   "WILL BINARY -> DO" },
        { R_WILL, O_TTYPE,  R_DONT, "WILL TTYPE -> DONT" },
        { R_WILL, O_NAWS,   R_DONT, "WILL NAWS -> DONT: that is ours to send" },
        { R_WILL, O_LINEMODE, R_DONT, "WILL LINEMODE -> DONT" },
    };
    for (const Case& c : kDo) {
        telnet::State s;
        const Ran r = run(s, { R_IAC, c.cmd, c.opt });
        chk::eq(static_cast<long>(r.data.size()), 0, "no data comes out");
        chk::eq(static_cast<long>(r.reply.size()), 3, "a three-octet answer");
        if (r.reply.size() == 3) {
            const unsigned char want[3] = { R_IAC, c.wantCmd, c.opt };
            chk::bytes(r.reply.data(), want, 3, c.why);
        }
    }

    // And no option number is left without an answer, because a far end
    // waiting for one waits for ever. Every option 0..255 that is not one
    // we perform must come back WON'T (for DO) or DON'T (for WILL).
    chk::group("every option number gets an answer");
    for (unsigned o = 0; o < 256; ++o) {
        telnet::State s;
        const Ran r = run(s, { R_IAC, R_DO, static_cast<unsigned char>(o) });
        chk::eq(static_cast<long>(r.reply.size()), 3, "DO is always answered");
        telnet::State s2;
        const Ran r2 = run(s2, { R_IAC, R_WILL, static_cast<unsigned char>(o) });
        chk::eq(static_cast<long>(r2.reply.size()), 3, "WILL is always answered");
    }

    chk::group("a negotiation split across reads");
    {
        // IAC, then the command, then the option, in three separate reads.
        telnet::State s;
        const Ran r = dribble(s, { 'a', R_IAC, R_DO, O_SGA, 'b' });
        const unsigned char want[2] = { 'a', 'b' };
        chk::bytes(r.data.data(), want, 2, "the data either side survives");
        chk::eq(static_cast<long>(r.reply.size()), 3, "and the answer still comes");
        const unsigned char wantRep[3] = { R_IAC, R_WILL, O_SGA };
        chk::bytes(r.reply.data(), wantRep, 3, "WILL SGA");
    }
}

void subnegotiation() {
    // RFC 855: a subnegotiation is IAC SB <option> <parameters> IAC SE.
    chk::group("a subnegotiation is swallowed whole");
    {
        telnet::State s;
        const Ran r = run(s, { 'a', R_IAC, R_SB, O_TTYPE, 0, 'V', 'T', '1', '0', '0',
                               R_IAC, R_SE, 'b' });
        const unsigned char want[2] = { 'a', 'b' };
        chk::eq(static_cast<long>(r.data.size()), 2, "none of it is data");
        chk::bytes(r.data.data(), want, 2, "and the data either side survives");
    }
    {
        // RFC 855: inside a subnegotiation a 255 is still doubled, so
        // IAC IAC must NOT end it.
        telnet::State s;
        const Ran r = run(s, { R_IAC, R_SB, O_NAWS, 0, R_IAC, R_IAC, 0, 24,
                               R_IAC, R_SE, 'x' });
        chk::eq(static_cast<long>(r.data.size()), 1, "only the x is data");
        chk::eq(r.data.empty() ? 0 : r.data[0], 'x', "and it is the x");
    }
    {
        // Dribbled one octet at a time.
        telnet::State s;
        const Ran r = dribble(s, { R_IAC, R_SB, O_TTYPE, 1, R_IAC, R_SE, 'y' });
        chk::eq(static_cast<long>(r.data.size()), 1, "still only the y");
    }

    chk::group("a subnegotiation that never ends does not eat the stream");
    {
        // The bound matters: without it a far end that never sends IAC SE
        // leaves this layer swallowing data for ever and the caller sees a
        // dead terminal rather than a fault. Past the bound it goes back
        // to reading data.
        telnet::State s;
        std::vector<unsigned char> in = { R_IAC, R_SB, O_TTYPE };
        for (int i = 0; i < 200; ++i) in.push_back('z');
        in.push_back('!');
        const Ran r = run(s, in);
        chk::ok(!r.data.empty(), "the stream comes back");
    }
}

void opening() {
    // The gateway speaks FIRST, which is what earns character mode and the
    // board's immediate cursor-position probe rather than its key prompt.
    // The expected bytes are written out from the RFCs, not taken from
    // hello().
    chk::group("the opening, octet by octet");
    unsigned char out[64];
    const size_t n = telnet::hello(out, sizeof out);
    const unsigned char want[] = {
        R_IAC, R_WILL, O_SGA,
        R_IAC, R_DO,   O_SGA,
        R_IAC, R_WILL, O_NAWS,
        R_IAC, R_WILL, O_BINARY,
        R_IAC, R_DO,   O_BINARY,
    };
    chk::eq(static_cast<long>(n), static_cast<long>(sizeof want),
            "five three-octet commands");
    if (n == sizeof want) chk::bytes(out, want, n, "and they are these");
    chk::ok(n > 0 && out[0] == R_IAC, "and IAC is the very first octet on the wire");

    chk::group("the opening is refused rather than truncated");
    for (size_t cap = 0; cap < sizeof want; ++cap) {
        unsigned char small[64];
        chk::eq(static_cast<long>(telnet::hello(small, cap)), 0,
                "a buffer too small gives nothing");
    }
}

void naws() {
    // RFC 1073: "the client sends the subnegotiation only after the server
    // has indicated DO NAWS". Sending SB before being asked is impolite
    // and some servers log it.
    chk::group("NAWS is not sent before DO NAWS");
    {
        telnet::State s;
        unsigned char out[32];
        chk::eq(static_cast<long>(telnet::naws(s, out, sizeof out, 80, 24)), 0,
                "nothing before the board asks");
    }
    chk::group("NAWS after DO NAWS");
    {
        telnet::State s;
        const Ran r = run(s, { R_IAC, R_DO, O_NAWS });
        chk::eq(static_cast<long>(r.reply.size()), 3, "WILL NAWS goes back");
        unsigned char out[32];
        const size_t n = telnet::naws(s, out, sizeof out, 80, 24);
        // RFC 1073: IAC SB NAWS WIDTH[1] WIDTH[0] HEIGHT[1] HEIGHT[0] IAC SE,
        // each figure 16 bits, most significant octet first.
        const unsigned char want[] = { R_IAC, R_SB, O_NAWS, 0, 80, 0, 24, R_IAC, R_SE };
        chk::eq(static_cast<long>(n), static_cast<long>(sizeof want), "nine octets");
        if (n == sizeof want) chk::bytes(out, want, n, "and they are the RFC's");
    }
    {
        // 40 columns, which is what a phone held upright should give the
        // board and the whole reason NAWS is here.
        telnet::State s;
        run(s, { R_IAC, R_DO, O_NAWS });
        unsigned char out[32];
        const size_t n = telnet::naws(s, out, sizeof out, 40, 25);
        const unsigned char want[] = { R_IAC, R_SB, O_NAWS, 0, 40, 0, 25, R_IAC, R_SE };
        chk::eq(static_cast<long>(n), static_cast<long>(sizeof want), "nine octets");
        if (n == sizeof want) chk::bytes(out, want, n, "a 40-column window");
    }
    {
        // RFC 1073 and RFC 855 together: a 255 in a parameter is doubled,
        // or the SB ends early. 255 columns is the case, and it is why the
        // line clamps at 240 - but the encoder must still be right.
        telnet::State s;
        run(s, { R_IAC, R_DO, O_NAWS });
        unsigned char out[32];
        const size_t n = telnet::naws(s, out, sizeof out, 255, 24);
        const unsigned char want[] = { R_IAC, R_SB, O_NAWS,
                                       0, 255, 255, 0, 24, R_IAC, R_SE };
        chk::eq(static_cast<long>(n), static_cast<long>(sizeof want),
                "ten octets, because the 255 is doubled");
        if (n == sizeof want) chk::bytes(out, want, n, "doubled inside the SB");
    }
    chk::group("DONT NAWS takes the permission away");
    {
        telnet::State s;
        run(s, { R_IAC, R_DO, O_NAWS });
        run(s, { R_IAC, R_DONT, O_NAWS });
        unsigned char out[32];
        chk::eq(static_cast<long>(telnet::naws(s, out, sizeof out, 80, 24)), 0,
                "and nothing is sent after it");
    }
}

void binary() {
    // The main project's own recorded lesson, in its words: ASKING IS NOT
    // AGREEING. Its first fix set the binary flag on the REQUEST, so a
    // terminal that declined kept padding CR for ever while the board had
    // already stopped stripping.
    chk::group("asking is not agreeing");
    {
        telnet::State s;
        unsigned char out[64];
        telnet::hello(out, sizeof out);        // we ask
        chk::ok(!s.binIn, "asking does not set the inbound flag");
        chk::ok(!s.binOut, "nor the outbound one");
    }
    {
        telnet::State s;
        run(s, { R_IAC, R_WILL, O_BINARY });   // the board agrees inbound
        chk::ok(s.binIn, "WILL BINARY from the board sets it");
        chk::ok(!s.binOut, "and only the inbound direction");
    }
    {
        telnet::State s;
        run(s, { R_IAC, R_DO, O_BINARY });     // the board agrees outbound
        chk::ok(s.binOut, "DO BINARY sets the outbound direction");
        chk::ok(!s.binIn, "and only that one");
    }
    {
        telnet::State s;
        run(s, { R_IAC, R_WILL, O_BINARY });
        run(s, { R_IAC, R_WONT, O_BINARY });
        chk::ok(!s.binIn, "WONT BINARY takes it back");
    }
    {
        telnet::State s;
        run(s, { R_IAC, R_DO, O_BINARY });
        run(s, { R_IAC, R_DONT, O_BINARY });
        chk::ok(!s.binOut, "DONT BINARY takes it back");
    }
}

void escaping() {
    // RFC 854 again: a 255 on the wire must be doubled.
    chk::group("escaping on the way out");
    {
        const unsigned char in[] = { 'a', 255, 'b' };
        unsigned char out[16];
        const size_t n = telnet::escape(in, 3, out, sizeof out);
        const unsigned char want[4] = { 'a', 255, 255, 'b' };
        chk::eq(static_cast<long>(n), 4, "one octet became two");
        chk::bytes(out, want, 4, "and the 255 is doubled");
    }
    {
        // All or nothing. Half an escaped burst puts a bare 255 on the
        // wire at the split and the board reads the next octet as a
        // command.
        const unsigned char in[] = { 255, 255, 255 };
        unsigned char out[8];
        chk::eq(static_cast<long>(telnet::escape(in, 3, out, 5)), 0,
                "a buffer that cannot hold the worst case refuses");
        chk::eq(static_cast<long>(telnet::escape(in, 3, out, 6)), 6,
                "and exactly the worst case is enough");
    }
    {
        // Nothing else is touched: a bare CR stays a bare CR, which is
        // what binary mode is for and what the board's in-place effects
        // depend on.
        const unsigned char in[] = { 'x', '\r', 0, '\n', 0x1B, '[', 'A' };
        unsigned char out[32];
        const size_t n = telnet::escape(in, sizeof in, out, sizeof out);
        chk::eq(static_cast<long>(n), static_cast<long>(sizeof in), "nothing added");
        chk::bytes(out, in, sizeof in, "and nothing changed");
    }
}

void a_real_opening_exchange() {
    // What the µnleashed board actually sends a client that speaks IAC
    // first, per its own documented detector: character mode negotiated
    // and then an immediate cursor-position probe. The probe is ordinary
    // data and must come through untouched, because xterm.js answering it
    // is what makes the board detect a real ANSI terminal.
    chk::group("a whole opening exchange, the board's side");
    telnet::State s;
    std::vector<unsigned char> in = {
        R_IAC, R_DO,   O_SGA,
        R_IAC, R_WILL, O_SGA,
        R_IAC, R_WILL, O_ECHO,
        R_IAC, R_DO,   O_NAWS,
        R_IAC, R_WILL, O_BINARY,
        R_IAC, R_DO,   O_BINARY,
    };
    // Then the probe: ESC [ 6 n, the cursor position report request.
    const unsigned char probe[] = { 0x1B, '[', '6', 'n' };
    for (unsigned char c : probe) in.push_back(c);

    const Ran r = dribble(s, in);
    chk::eq(static_cast<long>(r.data.size()), 4, "the probe is the only data");
    if (r.data.size() == 4) chk::bytes(r.data.data(), probe, 4, "and it is untouched");
    chk::eq(static_cast<long>(r.reply.size()), 18, "six commands answered");
    chk::ok(s.nawsOk, "NAWS is permitted");
    chk::ok(s.binIn, "binary inbound agreed");
    chk::ok(s.binOut, "binary outbound agreed");

    // And the answer to the probe goes back escaped and otherwise
    // untouched: ESC [ 24 ; 80 R.
    const unsigned char answer[] = { 0x1B, '[', '2', '4', ';', '8', '0', 'R' };
    unsigned char out[32];
    const size_t n = telnet::escape(answer, sizeof answer, out, sizeof out);
    chk::eq(static_cast<long>(n), static_cast<long>(sizeof answer), "nothing added");
    chk::bytes(out, answer, sizeof answer, "the report reaches the board as typed");
}

void reply_overflow() {
    // A burst of option requests bigger than the reply buffer. Each answer
    // is three octets and the buffer is kReplyMax, so past it answers are
    // dropped WHOLE rather than truncated: half a command is worse than
    // none, and the far end asks again.
    chk::group("a reply burst past the buffer drops whole commands");
    telnet::State s;
    std::vector<unsigned char> in;
    for (unsigned o = 0; o < 60; ++o) {
        in.push_back(R_IAC);
        in.push_back(R_DO);
        in.push_back(static_cast<unsigned char>(o + 40));   // none we perform
    }
    const Ran r = run(s, in);
    chk::ok(r.reply.size() <= telnet::kReplyMax, "the buffer is not overrun");
    chk::eq(static_cast<long>(r.reply.size() % 3), 0,
            "and what is there is whole commands");
    for (size_t i = 0; i + 2 < r.reply.size(); i += 3) {
        chk::eq(r.reply[i], R_IAC, "each starts with IAC");
        chk::eq(r.reply[i + 1], R_WONT, "and is a WONT");
    }
}

}  // namespace

int main() {
    printf("test_telnet: GW_TEST_BREAK=%d\n", GW_TEST_BREAK);
    codes_agree();
    data_passes_through();
    commands_without_options();
    negotiation();
    subnegotiation();
    opening();
    naws();
    binary();
    escaping();
    a_real_opening_exchange();
    reply_overflow();
    return chk::done("test_telnet");
}

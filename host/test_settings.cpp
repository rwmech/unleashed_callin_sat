// ===========================================================================
//  µnleashed gateway sat: the host tests
// ===========================================================================
//
// File:         host/test_settings.cpp
// Module:       The one validator
//
// Purpose:      set() is the only way any value changes, whoever is
//              asking, so it is the one place a bad value is judged. Most
//              of what it judges is TEXT A STRANGER TYPED into a form on
//              an open network, and that text then reaches an HTTP header,
//              an HTML page and a console line - so a control byte or a
//              newline getting through is somebody else's bug.
//
//              Three classes of check:
//
//              1. The refusals ARE REFUSALS, and the reason is a sentence
//                 rather than a code. A refusal that reads "GPIO 1 is the
//                 console port, which must keep working" is the difference
//                 between a sysop fixing it in ten seconds and reflashing
//                 to find out.
//              2. A refused value CHANGES NOTHING. That is the half a
//                 validator usually gets wrong: it reports the problem and
//                 has already written the value.
//              3. And the values that must be ACCEPTED are, because a
//                 validator that refuses everything passes every refusal
//                 test. Each group has its own this-is-fine case.
//
//              Build and run:  cd host && make && ./test_settings
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
#include <cstring>
#include <string>

#include "check.h"
#include "settings_rules.h"

namespace {

// The state the validator works on, fresh for every case so nothing
// carries over.
struct Box {
    settings::Cfg cfg;
    char          name[24] = {0};
    bool          restart  = false;

    const char* set(const char* k, const char* v) {
        return settings::set(cfg, name, sizeof name, restart, k, v);
    }
};

// Refused, and the reason is a real sentence.
void refused(Box& b, const char* k, const char* v, const char* what) {
    const char* why = b.set(k, v);
    chk::ok(why != nullptr, what);
    if (!why) return;
    // Not a code and not a shrug: long enough to say something, lower
    // case because it is appended after the field's name, and no control
    // bytes because it is going into an HTML page.
    chk::ok(strlen(why) >= 8, "the reason is a sentence");
    chk::ok(strlen(why) <= 90, "and fits a 40-column form's note");
    bool clean = true;
    for (const char* p = why; *p; ++p) {
        if (static_cast<unsigned char>(*p) < 0x20) clean = false;
    }
    chk::ok(clean, "with no control bytes in it");
}

void taken(Box& b, const char* k, const char* v, const char* what) {
    chk::ok(b.set(k, v) == nullptr, what);
}

void unknown_keys() {
    chk::group("a key nobody knows is refused");
    Box b;
    for (const char* k : { "", "x", "ap_", "AP", "ap_sid", "b0_name", "b6_name",
                           "ser_", "rep_", "../ap", "ap_ssid ", " ap_ssid" }) {
        chk::ok(b.set(k, "x") != nullptr, "refused");
    }
    chk::group("and a null is not a crash");
    chk::ok(b.set(nullptr, "x") != nullptr, "a null key");
    chk::ok(b.set("ap_ssid", nullptr) != nullptr, "a null value");
}

void numbers() {
    // The point of allDigits rather than strtol: strtol stops at the first
    // odd character and reports success for "80abc", so a sysop's typo
    // would silently become a different port.
    chk::group("a number is all digits or it is not a number");
    Box b;
    // The board list has to hold a board before ser_board will take one.
    taken(b, "b1_name", "Agentville", "a board to point at");
    for (const char* v : { "80abc", "8 0", "0x50", "+80", "-80", " 80", "80 ",
                           "8.0", "80;", "80\n", "e80", "", "abc" }) {
        refused(b, "ap_max", v, "not a number");
    }
    chk::eq(b.cfg.apMax, 15, "and none of them changed it");

    chk::group("a number out of range is refused, in range is taken");
    refused(b, "ap_max", "0", "0 phones");
    refused(b, "ap_max", "16", "16 phones");
    refused(b, "ap_max", "255", "255 phones");
    chk::eq(b.cfg.apMax, 15, "still unchanged");
    taken(b, "ap_max", "1", "1 phone is legal");
    chk::eq(b.cfg.apMax, 1, "and it took");
    taken(b, "ap_max", "15", "15 phones is legal");
    chk::eq(b.cfg.apMax, 15, "and it took");

    chk::group("the beacon must be a multiple of 100");
    for (const char* v : { "99", "150", "101", "1001", "0" }) {
        refused(b, "ap_beacon", v, "not a legal beacon interval");
    }
    chk::eq(b.cfg.apBeacon, 100, "unchanged");
    taken(b, "ap_beacon", "300", "300 TU is legal");
    chk::eq(b.cfg.apBeacon, 300, "and it took");
    taken(b, "ap_beacon", "1000", "1000 TU is the top");

    chk::group("DTIM is 1 to 3");
    refused(b, "ap_dtim", "0", "0");
    refused(b, "ap_dtim", "4", "4");
    taken(b, "ap_dtim", "1", "1 keeps a sleeping phone quick");
    chk::eq(b.cfg.apDtim, 1, "and it took");

    chk::group("a port is 1 to 65535");
    refused(b, "b1_port", "0", "port 0");
    refused(b, "b1_port", "65536", "port 65536");
    refused(b, "b1_port", "6400x", "port 6400x");
    chk::eq(b.cfg.boards[0].port, 6400, "unchanged");
    taken(b, "b1_port", "23", "port 23");
    chk::eq(b.cfg.boards[0].port, 23, "took");
    taken(b, "b1_port", "65535", "port 65535");
    chk::eq(b.cfg.boards[0].port, 65535, "took");
}

void addresses() {
    chk::group("an address is a dotted quad and nothing else");
    Box b;
    // inet_addr would take every one of these; a sysop reading the field
    // back would not recognise what they typed.
    for (const char* v : { "10", "10.0", "10.0.0", "10.0.0.0.1", "0x0a000001",
                           "10.0.0.256", "10.0.0.-1", "10..0.1", "10.0.0.",
                           ".10.0.0.1", "10.0.0.1 ", " 10.0.0.1", "10.0.0.1x",
                           "localhost", "board.local", "", "1000.1.1.1" }) {
        refused(b, "ap_addr", v, "not an address");
    }
    chk::ok(strcmp(b.cfg.apAddr, "172.16.0.1") == 0, "and none of them changed it");

    chk::group("the network and broadcast addresses of the /24 are refused");
    refused(b, "ap_addr", "172.16.0.0", "the network address");
    refused(b, "ap_addr", "172.16.0.255", "the broadcast address");

    chk::group("a real address is taken");
    taken(b, "ap_addr", "10.0.0.1", "10.0.0.1");
    chk::ok(strcmp(b.cfg.apAddr, "10.0.0.1") == 0, "and it took");
    taken(b, "ap_addr", "4.3.2.1", "4.3.2.1, which some phones prefer");
    taken(b, "ap_addr", "192.168.4.1", "and the IDF's own default");
}

void text_from_a_stranger() {
    // This is the class that matters most: these values reach an HTTP
    // header (the portal's Location), an HTML page (the board list) and a
    // console line. A newline in the first is header injection; a quote or
    // a bracket in the second is somebody else's browser.
    chk::group("control bytes in text are refused");
    Box b;
    const char* kKeys[] = { "ap_ssid", "net_ssid", "name", "b1_name", "b1_note",
                            "b1_host" };
    for (const char* k : kKeys) {
        // Split literals, because a hex escape eats every following hex
        // digit: "a\x7Fb" is one character 0x7FB, not 0x7F then 'b', and
        // -Werror says so.
        for (const char* v : { "a\nb", "a\rb", "a\tb", "a\x7f" "b", "a\x1b" "b",
                               "\n", "a\x01" "b", "a\x80" }) {
            chk::ok(b.set(k, v) != nullptr, "a control byte is refused");
        }
    }
    chk::group("and the header-injection shape specifically");
    for (const char* v : { "x\r\nLocation: http://evil",
                           "x\nSet-Cookie: gws=1",
                           "x\r\n\r\n<html>" }) {
        chk::ok(b.set("ap_ssid", v) != nullptr, "refused");
    }
    chk::ok(b.cfg.apSsid[0] == 0, "and nothing was written");

    chk::group("ordinary text is taken, including what HTML must escape");
    // These are accepted deliberately: a board may legitimately be called
    // "Rob & Sons" or "<Unleashed>". The escaping is web.cpp's job and it
    // is tested by inspection there; refusing them here would be refusing
    // real names.
    taken(b, "b1_name", "Rob & Sons", "an ampersand");
    taken(b, "b1_name", "<Unleashed>", "angle brackets");
    taken(b, "b1_name", "O'Brien", "an apostrophe");
    taken(b, "b1_name", "say \"hi\"", "a quote");
    taken(b, "ap_ssid", "The Rusty Antenna", "a real network name");
    chk::ok(strcmp(b.cfg.apSsid, "The Rusty Antenna") == 0, "and it took");

    chk::group("too long is refused and nothing is truncated into place");
    {
        Box c;
        std::string long33(33, 'a');
        refused(c, "ap_ssid", long33.c_str(), "a 33-character network name");
        chk::ok(c.cfg.apSsid[0] == 0, "and no truncated copy was written");
        std::string long32(32, 'a');
        taken(c, "ap_ssid", long32.c_str(), "32 is the limit and is legal");
        chk::eq(static_cast<long>(strlen(c.cfg.apSsid)), 32, "and all of it is there");

        std::string long17(17, 'b');
        refused(c, "b1_name", long17.c_str(), "a 17-character board name");
        std::string long16(16, 'b');
        taken(c, "b1_name", long16.c_str(), "16 is the limit");

        std::string long25(25, 'c');
        refused(c, "b1_note", long25.c_str(), "a 25-character note");
        taken(c, "b1_note", std::string(24, 'c').c_str(), "24 is the limit");

        std::string long65(65, 'd');
        refused(c, "b1_host", long65.c_str(), "a 65-character address");
        taken(c, "b1_host", std::string(64, 'd').c_str(), "64 is the limit");

        std::string long64(64, 'e');
        refused(c, "net_pass", long64.c_str(), "a 64-character Wi-Fi password");
        taken(c, "net_pass", std::string(63, 'e').c_str(), "63 is WPA2's limit");
    }
}

void the_wifi_password() {
    // Rob's decision of 2026-10-05: passworded by default, blank is the
    // deliberate exception, and a short one is REFUSED rather than quietly
    // becoming an open network. That last part is the shape of the trap
    // the core recorded on its own Wi-Fi page.
    chk::group("the access point's password");
    Box b;
    chk::ok(strcmp(b.cfg.apPass, "unleashed") == 0,
            "it ships as the published default");
    chk::eq(static_cast<long>(strlen(b.cfg.apPass)), 9,
            "which clears WPA2's eight-character minimum");

    for (const char* v : { "a", "ab", "abc", "abcd", "abcde", "abcdef", "abcdefg" }) {
        refused(b, "ap_pass", v, "one to seven characters is refused");
    }
    chk::ok(strcmp(b.cfg.apPass, "unleashed") == 0,
            "and NOT quietly turned into an open network");

    taken(b, "ap_pass", "abcdefgh", "eight is the minimum and is legal");
    chk::ok(strcmp(b.cfg.apPass, "abcdefgh") == 0, "and it took");
    taken(b, "ap_pass", "", "blank is the deliberate exception");
    chk::ok(b.cfg.apPass[0] == 0, "which is an open network");
    taken(b, "ap_pass", std::string(63, 'x').c_str(), "63 is the top");
    refused(b, "ap_pass", std::string(64, 'x').c_str(), "64 is too long");
}

void the_pins() {
    // board.h decides, and this is the WROOM profile. The point is that the
    // refusal NAMES what holds the pin.
    chk::group("a pin that is somebody else's is refused by name");
    Box b;
    struct Case { const char* pin; const char* holder; };
    const Case kBad[] = {
        { "1", "console" },    // UART0 TX
        { "3", "console" },    // UART0 RX
        { "6", "flash" },
        { "7", "flash" },
        { "8", "flash" },
        { "9", "flash" },
        { "10", "flash" },
        { "11", "flash" },
        { "2", "LED" },
        { "40", "no such pin" },
        { "99", "no such pin" },
    };
    for (const Case& c : kBad) {
        const char* why = b.set("ser_tx", c.pin);
        chk::ok(why != nullptr, "refused");
        if (why) {
            // The reason must name the holder, not just say "no".
            chk::ok(strstr(why, c.holder) != nullptr, "and says who holds it");
        }
    }
    chk::eq(b.cfg.serTx, -1, "and none of them changed it");

    chk::group("an input-only pin may be RX and not TX");
    // 34 to 39 are input-only on this part, which is what they are for:
    // 36 is a good RX pin.
    for (const char* p : { "34", "36", "39" }) {
        Box c;
        chk::ok(c.set("ser_tx", p) != nullptr, "refused as TX");
        chk::ok(c.set("ser_rx", p) == nullptr, "but taken as RX");
    }

    chk::group("a free pin is taken, and -1 is off");
    taken(b, "ser_tx", "17", "GPIO 17 as TX");
    chk::eq(b.cfg.serTx, 17, "and it took");
    taken(b, "ser_rx", "16", "GPIO 16 as RX");
    chk::eq(b.cfg.serRx, 16, "and it took");
    taken(b, "ser_tx", "-1", "-1 is off");
    chk::eq(b.cfg.serTx, -1, "and it took");
    taken(b, "ser_tx", "", "and so is blank");
    chk::eq(b.cfg.serTx, -1, "still off");
}

void refusals_that_name_the_phase() {
    // A setting that saves and then does nothing is the "Saved and live"
    // trap. Each of these is refused with the reason instead.
    chk::group("what this build does not have is refused, not saved");
    Box b;
    const char* why = b.set("repeat", "yes");
    chk::ok(why != nullptr, "the repeater is refused");
    if (why) chk::ok(strstr(why, "phase") != nullptr, "and the reason names the phase");
    chk::ok(!b.cfg.repeat, "and nothing was set");
    taken(b, "repeat", "no", "turning it off is fine");

    why = b.set("ser_flow", "rtscts");
    chk::ok(why != nullptr, "hardware flow control is refused");
    if (why) chk::ok(strstr(why, "RTS") != nullptr, "naming the pins it would need");
    taken(b, "ser_flow", "none", "none is fine");

    why = b.set("ser_hang", "dcd");
    chk::ok(why != nullptr, "carrier detect is refused");
    chk::ok(b.set("ser_hang", "dtr") != nullptr, "and so is DTR");
    taken(b, "ser_hang", "idle", "the idle timeout is what there is");
}

void the_terminal_server_switch() {
    // termsrv asks for its pins before it will turn on, because a role
    // switched on with no pins is a role that silently does nothing.
    chk::group("the terminal server will not turn on without pins");
    Box b;
    const char* why = b.set("termsrv", "yes");
    chk::ok(why != nullptr, "refused with no pins set");
    if (why) chk::ok(strstr(why, "pin") != nullptr, "and says so");
    chk::ok(!b.cfg.termsrv, "and it is still off");

    taken(b, "ser_tx", "17", "a TX pin");
    chk::ok(b.set("termsrv", "yes") != nullptr, "still refused with only one");
    taken(b, "ser_rx", "16", "and an RX pin");
    taken(b, "termsrv", "yes", "now it turns on");
    chk::ok(b.cfg.termsrv, "and it is on");
    chk::ok(b.restart, "and it asks for a restart");
}

void board_slots() {
    chk::group("clearing a board's name empties the slot");
    Box b;
    taken(b, "b2_name", "Agentville", "a name");
    taken(b, "b2_host", "192.168.0.50", "an address");
    taken(b, "b2_port", "6660", "a port");
    taken(b, "b2_note", "the test board", "a note");
    chk::ok(b.cfg.boards[1].used(), "the slot is in use");
    taken(b, "b2_name", "", "clearing the name");
    chk::ok(!b.cfg.boards[1].used(), "empties the slot");
    chk::ok(b.cfg.boards[1].host[0] == 0, "and the address with it");
    chk::ok(b.cfg.boards[1].note[0] == 0, "and the note");
    chk::eq(b.cfg.boards[1].port, 6400, "and the port goes back to the default");

    chk::group("every slot from 1 to 5, and no others");
    for (const char* k : { "b1_name", "b2_name", "b3_name", "b4_name", "b5_name" }) {
        chk::ok(b.set(k, "x") == nullptr, "a real slot");
    }
    for (const char* k : { "b0_name", "b6_name", "b9_name" }) {
        chk::ok(b.set(k, "x") != nullptr, "not a slot");
    }

    chk::group("ser_board must name a slot that is not empty");
    Box c;
    refused(c, "ser_board", "1", "slot 1 is empty");
    refused(c, "ser_board", "0", "0 is not a slot");
    refused(c, "ser_board", "6", "6 is not a slot");
    taken(c, "b1_name", "Agentville", "fill slot 1");
    taken(c, "ser_board", "1", "now it can be named");
    chk::eq(c.cfg.serBoard, 1, "and it took");
    taken(c, "ser_board", "", "blank means ask");
    chk::eq(c.cfg.serBoard, 0, "which is 0");
}

void the_restart_flag() {
    // The page says "some of what you changed needs a restart", and that
    // must be true: a page that says it after every save teaches a sysop
    // to ignore it.
    chk::group("a restart is asked for only when something changed");
    Box b;
    chk::ok(!b.restart, "nothing asked for yet");
    taken(b, "ap_addr", "172.16.0.1", "setting the address to what it already is");
    chk::ok(!b.restart, "asks for nothing");
    taken(b, "ap_max", "15", "and the phone count to what it already is");
    chk::ok(!b.restart, "still nothing");
    taken(b, "ap_addr", "10.0.0.1", "a real change");
    chk::ok(b.restart, "asks for a restart");

    chk::group("and a live setting never asks for one");
    Box c;
    taken(c, "ap_probe", "page", "the portal's probe answer");
    chk::ok(!c.restart, "is live");
    taken(c, "ser_idle", "30", "the idle timeout");
    chk::ok(!c.restart, "is live");
    taken(c, "b1_name", "x", "a board's name");
    chk::ok(!c.restart, "is live");
}

void yes_and_no() {
    // The first version of isYes tested only the FIRST LETTER against
    // y, Y, 1, o, O, t, T - so "off" read as YES, because it starts with
    // the same letter as "on", and a sysop typing it switched the role on.
    // These are the spellings a form actually sends, and this group is
    // what found it.
    chk::group("yes and no, in the spellings a form sends");
    Box b;
    for (const char* v : { "yes", "Yes", "YES", "y", "1", "on", "true", "t" }) {
        Box c;
        c.set("ap", v);
        chk::ok(c.cfg.ap, "reads as yes");
    }
    for (const char* v : { "no", "No", "0", "off", "false", "n", "", "x" }) {
        Box c;
        c.set("ap", "yes");
        c.set("ap", v);
        chk::ok(!c.cfg.ap, "reads as no");
    }
}

}  // namespace

int main() {
    printf("test_settings: GW_TEST_BREAK=%d\n", GW_TEST_BREAK);
    unknown_keys();
    numbers();
    addresses();
    text_from_a_stranger();
    the_wifi_password();
    the_pins();
    refusals_that_name_the_phase();
    the_terminal_server_switch();
    board_slots();
    the_restart_flag();
    yes_and_no();
    return chk::done("test_settings");
}

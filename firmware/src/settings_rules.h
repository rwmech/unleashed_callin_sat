// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/settings_rules.h
// Module:       The one validator, with no platform in it
//
// Purpose:      set() is the only way any value changes, whoever is
//              asking: the setup page today and `CONFIG sat <name>` over
//              the link in phase 3. So it is the one place a bad value is
//              judged, and the one place worth testing hard - and most of
//              what it judges is TEXT A STRANGER TYPED into a form on an
//              open network, which then reaches an HTTP header, an HTML
//              page and a console line.
//
//              It is a header with no ESP-IDF in it so host/test_settings
//              can drive it directly. State is passed in rather than
//              reached for, which is what makes that possible; settings.cpp
//              keeps NVS and the hashing and forwards to this.
//
//              Same shape the core uses for forums_ptr.h and
//              panel_photo_fit.h: the logic worth testing is lifted into a
//              header, and the .cpp keeps the platform.
//
// See also:     settings.cpp, board.h (pinProblem), host/test_settings.cpp
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

#include "board.h"
#include "repeat.h"
#include "settings.h"

// A switch that removes the guards one at a time, for the host tests only.
// Macros and not an enum: the preprocessor cannot see an enumerator, and
// written as an enum every `#if` here would compare 0 against an unknown
// identifier, read 0, and compile every guard OUT. That was made once in
// dnsparse.h and caught by -Werror.
#ifndef GW_TEST_BREAK
#define GW_TEST_BREAK 0
#endif
#define SBREAK_DIGITS    1   /* accept a number with rubbish after it */
#define SBREAK_PRINTABLE 2   /* accept control bytes in text */
#define SBREAK_IPV4      3   /* accept anything as an address */

namespace settings {

// Copies at most n-1 bytes and always terminates. Values here are ASCII
// settings, so there is no UTF-8 character to back off from the way the
// board's own board_name has to.
inline void copyStr(char* dst, size_t n, const char* src) {
    if (n == 0) return;
    size_t i = 0;
    for (; src[i] && i + 1 < n; ++i) dst[i] = src[i];
    dst[i] = 0;
}

// Whole words, not first letters.
//
// The first version tested v[0] against y, Y, 1, o, O, t and T - so "off"
// read as YES, because it starts with an o and so does "on". A sysop
// typing "off" into a yes/no field switched the role ON. Found by the host
// tests feeding it the spellings a form actually sends.
//
// The lesson generalises past this function: a prefix test on a set of
// words is only safe while no word in the set is a prefix of a word
// outside it, and nothing says when that stops being true.
inline bool sameWord(const char* a, const char* b) {
    size_t i = 0;
    for (; a[i] && b[i]; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
        if (x != y) return false;
    }
    return a[i] == 0 && b[i] == 0;
}

inline bool isYes(const char* v) {
    static const char* kYes[] = { "yes", "y", "1", "on", "true", "t" };
    for (const char* w : kYes) {
        if (sameWord(v, w)) return true;
    }
    return false;
}

// A number, strictly. strtol stops at the first odd character and reports
// success for "80abc", which here would mean a sysop's typo silently
// becoming a different port. Refuse anything that is not all digits.
inline bool allDigits(const char* v, long& out) {
    if (!*v) return false;
    long n = 0;
    for (const char* p = v; *p; ++p) {
#if GW_TEST_BREAK != SBREAK_DIGITS
        if (*p < '0' || *p > '9') return false;
#else
        if (*p < '0' || *p > '9') { out = n; return true; }   /* strtol's way */
#endif
        n = n * 10 + (*p - '0');
        if (n > 1000000L) return false;        // nothing here is that big
    }
    out = n;
    return true;
}

// A dotted IPv4 and nothing else. inet_addr would take "10" and "0x0a" and
// a sysop reading the field back would not recognise what they typed.
inline bool isIpv4(const char* v) {
#if GW_TEST_BREAK == SBREAK_IPV4
    return *v != 0;
#endif
    int parts = 0;
    const char* p = v;
    while (*p) {
        int n = 0, digits = 0;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); ++p; ++digits; }
        if (digits == 0 || digits > 3 || n > 255) return false;
        ++parts;
        if (*p == '.') { ++p; if (!*p) return false; }
        else if (*p) return false;
    }
    return parts == 4;
}

// The characters an SSID or a host may hold. Printable ASCII and no
// control bytes, because these end up in an HTTP header, an HTML page and
// a console line, and a newline in any of those is somebody else's bug.
inline bool printableOnly(const char* v) {
#if GW_TEST_BREAK == SBREAK_PRINTABLE
    (void)v;
    return true;
#else
    for (const char* p = v; *p; ++p) {
        if (static_cast<unsigned char>(*p) < 0x20 ||
            static_cast<unsigned char>(*p) > 0x7E) return false;
    }
    return true;
#endif
}

inline const char* set(Cfg& g_cfg, char* g_name, size_t nameCap,
                      bool& g_restart, const char* key, const char* value) {
    if (!key || !value) return "nothing to set";
    long n = 0;

    // --- the roles -------------------------------------------------------
    if (!strcmp(key, "ap")) {
        bool want = isYes(value);
        if (want && !g_cfg.ap) g_restart = true;
        if (!want && g_cfg.ap) g_restart = true;
        g_cfg.ap = want;
        return nullptr;
    }
    if (!strcmp(key, "termsrv")) {
        bool want = isYes(value);
        if (want) {
            // Refused here rather than at start-up, so the sysop is told on
            // the form. Same shape as the core's own console-pin rule.
            if (g_cfg.serTx < 0 || g_cfg.serRx < 0) {
                return "set the TX and RX pins first";
            }
            const char* p = board::pinProblem(g_cfg.serTx, true);
            if (p) return p;
            p = board::pinProblem(g_cfg.serRx, false);
            if (p) return p;
        }
        if (want != g_cfg.termsrv) g_restart = true;
        g_cfg.termsrv = want;
        return nullptr;
    }
    if (!strcmp(key, "repeat")) {
        if (isYes(value)) {
            // The role exists as a setting and not as code, which is the
            // honest state of it. Saying which phase is what stops somebody
            // hunting for a fault that is an unwritten feature.
            return repeat::unavailable;
        }
        g_cfg.repeat = false;
        return nullptr;
    }

    // --- the access point ------------------------------------------------
    if (!strcmp(key, "ap_ssid")) {
        if (strlen(value) > 32) return "a Wi-Fi name is at most 32 characters";
        if (!printableOnly(value)) return "plain characters only in a Wi-Fi name";
        if (strcmp(value, g_cfg.apSsid) != 0) g_restart = true;
        copyStr(g_cfg.apSsid, sizeof g_cfg.apSsid, value);
        return nullptr;
    }
    if (!strcmp(key, "ap_pass")) {
        // Blank is an open network, deliberately and not by accident: the
        // fairground case needs one. A short one is refused by name, with
        // WPA2's own minimum in the sentence, rather than quietly opening
        // the network the sysop just tried to close.
        const size_t n2 = strlen(value);
        if (n2 && n2 < 8) {
            return "at least 8 characters, or blank for an open network";
        }
        if (n2 > 63) return "a Wi-Fi password is at most 63 characters";
        if (n2 && !printableOnly(value)) return "plain characters only in a Wi-Fi password";
        if (strcmp(value, g_cfg.apPass) != 0) g_restart = true;
        copyStr(g_cfg.apPass, sizeof g_cfg.apPass, value);
        return nullptr;
    }
    if (!strcmp(key, "ap_addr")) {
        if (!isIpv4(value)) return "an address like 172.16.0.1";
        // .0 and .255 are the network and the broadcast on the /24 the DHCP
        // server hands out, and a gateway on either answers nothing.
        if (strlen(value) >= sizeof g_cfg.apAddr) return "too long for an address";
        const char* last = strrchr(value, '.');
        if (last && (!strcmp(last, ".0") || !strcmp(last, ".255"))) {
            return "not a network or broadcast address";
        }
        if (strcmp(value, g_cfg.apAddr) != 0) g_restart = true;
        copyStr(g_cfg.apAddr, sizeof g_cfg.apAddr, value);
        return nullptr;
    }
    if (!strcmp(key, "ap_chan")) {
        if (!*value) {                                    // blank follows the board
            if (g_cfg.apChan != 0) g_restart = true;
            g_cfg.apChan = 0;
            return nullptr;
        }
        if (!allDigits(value, n) || n < 1 || n > 13) return "a channel from 1 to 13, or blank";
        // The one hard impossibility in the whole design: one radio cannot
        // serve two channels. In phase 1 there is no pairing yet, so there
        // is no board channel to clash with and a number is simply taken.
        // Phase 4 refuses a number that differs from the boards' channel,
        // and names the channel they are on.
        if (g_cfg.apChan != static_cast<uint8_t>(n)) g_restart = true;
        g_cfg.apChan = static_cast<uint8_t>(n);
        return nullptr;
    }
    if (!strcmp(key, "ap_max")) {
        if (!allDigits(value, n) || n < 1 || n > 15) return "1 to 15 phones";
        if (g_cfg.apMax != static_cast<uint8_t>(n)) g_restart = true;
        g_cfg.apMax = static_cast<uint8_t>(n);
        return nullptr;
    }
    if (!strcmp(key, "ap_beacon")) {
        if (!allDigits(value, n) || n < 100 || n > 1000) return "100 to 1000";
        if (n % 100 != 0) return "a multiple of 100";
        if (g_cfg.apBeacon != static_cast<uint16_t>(n)) g_restart = true;
        g_cfg.apBeacon = static_cast<uint16_t>(n);
        return nullptr;
    }
    if (!strcmp(key, "ap_dtim")) {
        if (!allDigits(value, n) || n < 1 || n > 3) {
            return "1 to 3; 1 keeps a sleeping phone's keystrokes quickest";
        }
        if (g_cfg.apDtim != static_cast<uint8_t>(n)) g_restart = true;
        g_cfg.apDtim = static_cast<uint8_t>(n);
        return nullptr;
    }
    if (!strcmp(key, "ap_probe")) {
        g_cfg.apProbe = (value[0] == 'p' || value[0] == 'P') ? PROBE_PAGE : PROBE_REDIRECT;
        return nullptr;   // live: the next probe is answered the new way
    }

    // --- the network this gateway joins ---------------------------------
    if (!strcmp(key, "net_ssid")) {
        if (strlen(value) > 32) return "a Wi-Fi name is at most 32 characters";
        if (!printableOnly(value)) return "plain characters only in a Wi-Fi name";
        // The core's own trap, and it is worth refusing here for the same
        // reason: changing the network without retyping the password saves
        // the new name with the old key, which fails at the next restart
        // with nobody on the board to fix it.
        // Allowed with no password, because an open network to join is a
        // real thing - but deliberately rather than by accident, which is
        // what the form's own hint on the password row is for ("retype it
        // whenever you change the name above, or the gateway tries the new
        // network with the old key"). Nothing is logged here: this header
        // has no platform in it, which is what lets the host tests drive
        // exactly the validator that ships.
        if (strcmp(value, g_cfg.netSsid) != 0) g_restart = true;
        copyStr(g_cfg.netSsid, sizeof g_cfg.netSsid, value);
        return nullptr;
    }
    if (!strcmp(key, "net_pass")) {
        if (strlen(value) > 63) return "a Wi-Fi password is at most 63 characters";
        if (strcmp(value, g_cfg.netPass) != 0) g_restart = true;
        copyStr(g_cfg.netPass, sizeof g_cfg.netPass, value);
        return nullptr;
    }

    // --- the terminal server --------------------------------------------
    if (!strcmp(key, "ser_tx") || !strcmp(key, "ser_rx") || !strcmp(key, "ser_btn")) {
        int pin = -1;
        if (*value && strcmp(value, "-1") != 0) {
            if (!allDigits(value, n) || n > GW_GPIO_MAX) return "no such pin on this board";
            pin = static_cast<int>(n);
        }
        const bool out = (key[4] == 't');          // ser_tx drives; rx and btn read
        const char* p = board::pinProblem(pin, out);
        if (p) return p;
        int8_t* slot = (key[4] == 't' && key[5] == 'x') ? &g_cfg.serTx
                     : (key[4] == 'r') ? &g_cfg.serRx : &g_cfg.serBtn;
        if (*slot != static_cast<int8_t>(pin)) g_restart = true;
        *slot = static_cast<int8_t>(pin);
        return nullptr;
    }
    if (!strcmp(key, "ser_baud")) {
        static const uint32_t kBauds[] = {300, 1200, 2400, 9600, 19200, 38400, 57600, 115200};
        if (!allDigits(value, n)) return "a speed from the list";
        for (uint32_t b : kBauds) {
            if (static_cast<uint32_t>(n) == b) {
                if (g_cfg.serBaud != b) g_restart = true;
                g_cfg.serBaud = b;
                return nullptr;
            }
        }
        return "300, 1200, 2400, 9600, 19200, 38400, 57600 or 115200";
    }
    if (!strcmp(key, "ser_fmt")) {
        if (!strcmp(value, "8N1")) g_cfg.serFmt = FMT_8N1;
        else if (!strcmp(value, "7E1")) g_cfg.serFmt = FMT_7E1;
        else if (!strcmp(value, "7N1")) g_cfg.serFmt = FMT_7N1;
        else return "8N1, 7E1 or 7N1";
        return nullptr;
    }
    if (!strcmp(key, "ser_flow")) {
        if (value[0] == 'r' || value[0] == 'R') {
            // RTS and CTS need two more pins, and this profile has no rows
            // for them, so the honest answer is that it is not wired rather
            // than a setting that saves and does nothing.
            return "hardware flow control needs RTS and CTS pins, which this build has no rows for";
        }
        g_cfg.serFlow = FLOW_NONE;
        return nullptr;
    }
    if (!strcmp(key, "ser_hang")) {
        if (!strcmp(value, "idle")) { g_cfg.serHang = HANG_IDLE; return nullptr; }
        // Carrier detect and DTR each need a pin this build has no row for,
        // and a hang-up method that cannot see its pin is a line that never
        // hangs up. Refused by name, with the reason.
        return "only the idle timeout in this build; carrier detect and DTR need their own pin rows";
    }
    if (!strcmp(key, "ser_idle")) {
        if (!allDigits(value, n) || n < 1 || n > 120) return "1 to 120 minutes";
        g_cfg.serIdle = static_cast<uint8_t>(n);
        return nullptr;                             // live: the clock is read each pass
    }
    if (!strcmp(key, "ser_board")) {
        if (!*value) { g_cfg.serBoard = 0; return nullptr; }   // blank: ask
        if (!allDigits(value, n) || n < 1 || n > kBoards) return "a board from 1 to 5, or blank";
        if (!g_cfg.boards[n - 1].used()) return "that board slot is empty";
        g_cfg.serBoard = static_cast<uint8_t>(n);
        return nullptr;
    }

    // --- the repeater ----------------------------------------------------
    if (!strcmp(key, "rep_hops")) {
        if (!allDigits(value, n) || n < 1 || n > 2) return "1 or 2 hops";
        g_cfg.repHops = static_cast<uint8_t>(n);
        return nullptr;
    }
    if (!strcmp(key, "rep_up")) {
        if (!*value) { g_cfg.repUp = 0; return nullptr; }
        if (!allDigits(value, n) || n < 1 || n > kBoards) return "a board from 1 to 5, or blank";
        g_cfg.repUp = static_cast<uint8_t>(n);
        return nullptr;
    }

    // --- this gateway's own name ----------------------------------------
    if (!strcmp(key, "name")) {
        if (strlen(value) > 16) return "a name is at most 16 characters";
        if (!printableOnly(value)) return "plain characters only in a name";
        // The access point falls back to this when ap_ssid is blank, so
        // changing it changes the Wi-Fi name, which only happens at the
        // next restart. Saying "saved and live" for it would be the trap
        // the page's own restart notice exists to avoid.
        if (strcmp(value, g_name) != 0 && !g_cfg.apSsid[0]) g_restart = true;
        copyStr(g_name, nameCap, value);
        return nullptr;
    }

    // --- the board list --------------------------------------------------
    // b<n>_name, b<n>_host, b<n>_port, b<n>_note, with n from 1.
    if (key[0] == 'b' && key[1] >= '1' && key[1] <= static_cast<char>('0' + kBoards)
        && key[2] == '_') {
        Board& b = g_cfg.boards[key[1] - '1'];
        const char* what = key + 3;
        if (!strcmp(what, "name")) {
            if (strlen(value) > 16) return "a board name is at most 16 characters";
            if (!printableOnly(value)) return "plain characters only in a board name";
            if (b.from == SRC_PAIRED) return "this board's name comes from its pairing";
            copyStr(b.name, sizeof b.name, value);
            // Clearing the name empties the slot, which is how a board is
            // removed: there is no separate delete to forget to write.
            if (!value[0]) { b.host[0] = 0; b.note[0] = 0; b.port = 6400; }
            return nullptr;
        }
        if (!strcmp(what, "host")) {
            if (strlen(value) > 64) return "an address is at most 64 characters";
            if (!printableOnly(value)) return "plain characters only in an address";
            if (b.from == SRC_PAIRED) return "a paired board is reached over the link, not by address";
            copyStr(b.host, sizeof b.host, value);
            return nullptr;
        }
        if (!strcmp(what, "port")) {
            if (!allDigits(value, n) || n < 1 || n > 65535) return "a port from 1 to 65535";
            b.port = static_cast<uint16_t>(n);
            return nullptr;
        }
        if (!strcmp(what, "note")) {
            if (strlen(value) > 24) return "a note is at most 24 characters";
            if (!printableOnly(value)) return "plain characters only in a note";
            copyStr(b.note, sizeof b.note, value);
            return nullptr;
        }
    }

    return "no such setting";
}

}  // namespace settings

// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/settings.h
// Module:       What the gateway keeps across a power cut, and the one
//               place a value is judged
//
// Purpose:      The three roles and every page behind them, in NVS on the
//               device. On the device and not on the board, because of a
//               chicken and egg the camera satellite never had: in the
//               no-router case the gateway IS the board's access point, so
//               the gateway has to be fully configured before any board can
//               reach it to configure it. Hence its own setup page.
//
//               set() is the only way a value changes, whoever is asking:
//               the setup page today, and `CONFIG sat <name>` over the link
//               in phase 3. One validator, one refusal, one set of words. A
//               second path would be a second opinion about what is legal.
//
//               A refusal is a sentence, not a code. "GPIO 1 is the console
//               port, which must keep working" is the difference between a
//               sysop fixing it in ten seconds and a sysop reflashing to
//               find out, and it is the same reason the core refuses a pin
//               by name rather than by range.
//
// See also:     board.h (pinProblem), web.cpp (the setup page), the spec's
//               §4.2, whose tables these keys follow
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

namespace settings {

// Five boards, matching the link engine's kHosts, so the list does not have
// to change shape when phase 4 fills slots by pairing instead of by typing.
constexpr uint8_t kBoards = 5;

// Where a slot came from. The field exists so that another source could
// fill one later (an ESP32 running a directory is its own item, and Rob has
// separated it out); it is the cheapest way to keep that seam visible.
// Nothing here learns about a board it was not told about, and that is what
// makes the list trustworthy.
enum : uint8_t { SRC_TYPED = 0, SRC_PAIRED = 1 };

struct Board {
    char     name[17] = {0};    // what a caller sees
    char     host[65] = {0};    // an address or a name, for an IP board
    uint16_t port     = 6400;
    uint8_t  from     = SRC_TYPED;
    char     note[25] = {0};    // a word of description on the portal
    bool used() const { return name[0] != 0; }
    bool byIp() const { return from == SRC_TYPED && host[0] != 0; }
};

// How the portal answers an operating system's "am I behind a portal?"
// probe. Both work in theory; ESP32 portals are reported not to pop on some
// recent Android and Samsung devices, and answering 200 with a body rather
// than 302 is one of the two things people report fixing it. Secondary and
// unexplained, free to switch, and phase 2 tries both on a real phone.
enum : uint8_t { PROBE_REDIRECT = 0, PROBE_PAGE = 1 };

enum : uint8_t { FMT_8N1 = 0, FMT_7E1 = 1, FMT_7N1 = 2 };
enum : uint8_t { FLOW_NONE = 0, FLOW_RTSCTS = 1 };
enum : uint8_t { HANG_IDLE = 0, HANG_DCD = 1, HANG_DTR = 2 };

struct Cfg {
    // The roles. All three default off, so a freshly flashed gateway has no
    // roles on and SAYS SO rather than looking broken: that is the state a
    // box is in between the flash and the setup, and refusing it would be
    // refusing the only way in.
    bool ap      = false;
    bool termsrv = false;
    bool repeat  = false;

    // The access point
    char     apSsid[33]  = {0};      // blank: this gateway's own name
    char     apAddr[16]  = "172.16.0.1";
    uint8_t  apChan      = 0;        // 0: follow the board (blank on the form)
    uint8_t  apMax       = 15;
    uint16_t apBeacon    = 100;      // TU
    uint8_t  apDtim      = 1;        // 1 keeps a sleeping phone's keystrokes quick
    uint8_t  apProbe     = PROBE_REDIRECT;

    // The network this gateway joins, when there is a router to join. Not
    // in the spec's §4.2 tables, and needed by its own use-case rows: a
    // gateway at home runs its access point beside a station on the house
    // router. Blank means it joins nothing, which is the field case where
    // the BOARD joins this gateway instead.
    char netSsid[33] = {0};
    char netPass[65] = {0};

    // The terminal server. -1 on a pin is off, the lights' convention.
    int8_t   serTx    = -1;
    int8_t   serRx    = -1;
    int8_t   serBtn   = -1;
    uint32_t serBaud  = 9600;
    uint8_t  serFmt   = FMT_8N1;
    uint8_t  serFlow  = FLOW_NONE;
    uint8_t  serHang  = HANG_IDLE;
    uint8_t  serIdle  = 20;          // minutes
    uint8_t  serBoard = 0;           // 0: ask; 1..kBoards: that one

    // The repeater. The rows exist in this build and the role does not; see
    // repeat.h, which says so in one sentence rather than hiding the fact.
    uint8_t repHops = 1;
    uint8_t repUp   = 0;

    Board boards[kBoards];

    // The setup page's password: a salted SHA-256, never the password.
    uint8_t salt[16] = {0};
    uint8_t pwHash[32] = {0};
    bool    pwSet = false;
};

// Reads NVS. Returns false only if NVS itself could not be opened, in which
// case the defaults above are what runs and the console says so.
bool begin();

// The live settings. Read freely; change only through set().
const Cfg& get();

// Changes one value. Returns nullptr when it was taken, or the sentence
// explaining why it was not. Nothing is written to NVS until save().
//
// Keys are the spec's: ap, termsrv, repeat, ap_ssid, ap_addr, ap_chan,
// ap_max, ap_beacon, ap_dtim, ap_probe, net_ssid, net_pass, ser_tx, ser_rx,
// ser_baud, ser_fmt, ser_flow, ser_hang, ser_idle, ser_board, ser_btn,
// rep_hops, rep_up, and b<n>_name / b<n>_host / b<n>_port / b<n>_note.
const char* set(const char* key, const char* value);

// Writes everything changed since the last save. One call after a form, not
// one per field.
bool save();

// Which settings need a restart before they do anything, so the page can
// say so rather than claiming "saved and live" for something that is not.
// Cleared by save() reporting it; true while any such change is pending.
bool restartPending();

// This gateway's own name: what NVS holds, or "gateway-" and the last two
// bytes of its MAC.
void name(char* out, size_t n);

// The setup password. Checking is constant-time; setting an empty password
// clears it, which puts the page back to its first-boot state and says so.
bool passwordSet();
bool passwordOk(const char* candidate);
const char* passwordSetTo(const char* plain);

// The board a line should call, for a source that was given a number.
// nullptr if that slot is empty.
const Board* board(uint8_t oneBased);

}  // namespace settings

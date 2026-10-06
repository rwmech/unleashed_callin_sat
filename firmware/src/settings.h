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
//
// PHASE 2 RAN IT, 2026-10-06, and the reports are right: a Samsung on
// Android showed "Internet may not be available" rather than "Sign in to
// network" under PROBE_REDIRECT, which is NetworkMonitor's no-internet
// branch rather than its portal branch. PROBE_PAGE is the default now and
// REDIRECT is kept, because the evidence is one handset and the setting
// costs a byte: a phone that wants the 302 can still have it.
enum : uint8_t { PROBE_REDIRECT = 0, PROBE_PAGE = 1 };

// The access point's published password, in one place so the default and
// every line that reasons about it cannot drift. It is meant to be printed
// on a sign, not kept: see apPass below.
#define GW_AP_PASS_PUBLISHED "unleashed"

enum : uint8_t { FMT_8N1 = 0, FMT_7E1 = 1, FMT_7N1 = 2 };
enum : uint8_t { FLOW_NONE = 0, FLOW_RTSCTS = 1 };
enum : uint8_t { HANG_IDLE = 0, HANG_DCD = 1, HANG_DTR = 2 };

struct Cfg {
    // THE ACCESS POINT DEFAULTS ON, and the other two off.
    //
    // It has to. The portal is the only way to configure a gateway, so with
    // every role off a freshly flashed box boots, says "no roles are on",
    // and there is NO WAY IN - not even a console, which is write-only
    // here. The specification said `ap` defaults to no AND that the portal
    // "is how a node is set up from nothing" AND that all-roles-off is the
    // legal state "between flashing and setting up"; those three cannot
    // all be true, and the default is the part that gives.
    //
    // THIS IS ONLY SAFE BECAUSE THE ACCESS POINT SHIPS PASSWORDED, and the
    // two settings are now coupled. Under the original design the access
    // point was open, and defaulting it on would have meant every freshly
    // flashed gateway broadcasting an open network to anyone in range -
    // a much worse decision than the one it looks like. What turns it into
    // the obvious default is apPass below shipping as the published
    // "unleashed" (Rob, 2026-10-05).
    //
    // So: ANYBODY WHO LATER MAKES THE ACCESS POINT OPEN BY DEFAULT MUST
    // ALSO TURN THIS OFF, and then provide another way in. Meeting that
    // here is the point of saying it.
    bool ap      = true;
    bool termsrv = false;
    bool repeat  = false;

    // The access point.
    //
    // THE ACCESS POINT IS PASSWORDED BY DEFAULT (Rob, 2026-10-05), and
    // "unleashed" is a PUBLISHED default, the same word as the board's own
    // published sysop password. It is meant to be printed on the sign at
    // the fairground beside the network's name, not kept secret: what it
    // buys is CCMP on the air, so a passer-by with a laptop reads nothing,
    // while a caller still only has to read a sign.
    //
    // Nine characters, which clears WPA2's eight-character minimum. A
    // shorter default later would not, so check before changing it.
    //
    // The authmode follows the password, and the pinned IDF gives exactly
    // two usable choices with nothing in between: WPA2_PSK, where the
    // driver overwrites the pairwise cipher with CCMP for us
    // (esp_wifi_types_generic.h:339), or open. OWE, which would encrypt a
    // network with no password at all, is not merely missing on 5.3.1: the
    // same comment says soft-AP does not support it.
    //
    // BLANK is still allowed and is now the deliberate exception rather
    // than the default: a sysop who clears it gets an open network and the
    // honest line with it.
    //
    // A password of 1 to 7 characters is REFUSED rather than quietly
    // becoming an open network. Silently becoming open is the exact shape
    // of the trap the core already recorded on its own Wi-Fi page, where a
    // changed network name with an untouched password saved an SSID with no
    // key and the next boot tried it as an open network. The sysop is told
    // instead.
    char     apSsid[33]  = {0};                 // blank: this gateway's own name
    char     apPass[65]  = GW_AP_PASS_PUBLISHED;  // blank: open; set: WPA2 + CCMP
    // 4.3.2.1, which is WLED's (Rob, 2026-10-06: "this should also be like
    // WLED where its 4.3.2.1 ... as its easier to remember"). The address
    // goes on the sign beside the network's name at a fairground, so being
    // sayable is the requirement, and four descending digits is as good as
    // that gets. It is also well away from 192.168.4.1, which is the ESP32
    // default every tutorial uses and one of the two things people report
    // fixing a portal that will not pop.
    //
    // It IS public address space (4.0.0.0/8), and WLED has shipped it for
    // years regardless: this box routes nothing, so the subnet is an island
    // and no packet for the real 4.3.2.1 ever leaves a phone that is on it.
    // A gateway given an uplink later would need to think about that again.
    //
    // That is not free, and it bit within a minute of the first flash: a
    // tool on the bench refused to reach 4.3.2.1 because it classes public
    // space as the internet, and VPNs, firewalls and device policies make
    // the same distinction. Rob weighed it and kept the address anyway
    // (2026-10-06): "its proven to work with wled and im guessing hes had a
    // lot more people report bugs that youve seen on the topic". Which is
    // the right way to settle it - a thing shipped to that many boxes has
    // better evidence behind it than an argument from first principles.
    char     apAddr[16]  = "4.3.2.1";
    uint8_t  apChan      = 0;        // 0: follow the board (blank on the form)
    uint8_t  apMax       = 15;
    uint16_t apBeacon    = 100;      // TU
    uint8_t  apDtim      = 1;        // 1 keeps a sleeping phone's keystrokes quick
    uint8_t  apProbe     = PROBE_PAGE;   // phase 2 on a real Samsung; see above

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

// set() applies as it validates, and some of what it applies is live at
// once (the portal's probe answer, the serial line's idle clock, every
// board address). So a form refused part way would already have changed
// the rows before the refusal.
//
// snapshot() before applying a form, rollback() on any refusal, commit()
// once save() has succeeded. One copy of the struct, which is cheaper than
// a second validator that could disagree with the first.
void snapshot();
void rollback();
void commit();

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

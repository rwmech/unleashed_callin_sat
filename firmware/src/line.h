// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/line.h
// Module:       A caller line: one stream of bytes, whatever carries it
//
// Purpose:      THE architectural piece, and the reason it exists before
//               any role does. A caller line has a SOURCE (what carries the
//               caller's bytes to and from this box) and an UPLINK (how
//               this box reaches a board). Neither knows what the other is.
//
//                   source                         uplink
//                   ------                         ------
//                   access point  --\         /--  IP (telnet)   phase 1
//                   terminal server -+-- Line -+
//                   (a modem, a dock) /         \--  link (CALLIN) phase 4
//
//               Build it this way and a role is a setting: a UART is a
//               second source feeding the same uplink, and CALLIN later is
//               a second uplink under the same sources. Build it the other
//               way and you get three programs in one repository, which is
//               the two-box design Rob rejected wearing one name.
//
//               Note what a "line" is NOT: it is not a node. A node is a
//               caller line ON THE BOARD, numbered 1 of 10 in everything a
//               caller reads. These are this gateway's own lines, and there
//               are four of them because the gateway's sockets and heap say
//               so, not because the board has ten.
//
//               The rules a source and an uplink both obey:
//
//               - **Bytes to the caller are offered, never pushed.** The
//                 pump reads the uplink only while the source has room, so
//                 a phone that has wandered out of range cannot make the
//                 gateway buffer a screen it will never take. That is the
//                 board's own BBS_RX_ROOM rule, and it is why there is a
//                 room() before every send().
//               - **A send takes all of it or none of it.** A half-taken
//                 burst would split an escape sequence, and a terminal
//                 draws the pieces.
//               - **A line's state outlives neither side.** Lines come from
//                 a static pool, so anything left set on a closed line is
//                 inherited by the next caller on it. Every field is reset
//                 in open(), not in close(): an exit path that did not run
//                 cannot be relied on, which is the shape of every bug of
//                 this kind in the core.
//
// See also:     web.cpp (the access point source), uart.cpp (the terminal
//               server source), telnet.h (the IP uplink's protocol)
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
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "ring.h"
#include "telnet.h"

namespace line {

// How many callers this gateway carries at once.
//
// Sized by the gateway, not by the board. The arithmetic: sixteen lwIP
// sockets at IDF 5.3.1, less one for the DNS redirector and three for
// esp_http_server's own working, leaves twelve; each caller takes one for
// the browser's WebSocket and one for the uplink, so six would fit, and
// four leaves room for the phones that are only reading the portal. RAM is
// the other half: see kToBoardBytes below.
//
// The board is still the binding constraint at ten caller lines, which is
// where a sysop would expect it to be.
constexpr uint8_t kLines = 4;

// A caller's keystrokes waiting to go to the board. Only keystrokes go this
// way, so this is generous: a second of fast typing is 20 bytes and the
// largest realistic burst is somebody pasting. It is a power of two for
// Ring's mask.
constexpr size_t kToBoardBytes = 1024;

// What this gateway offers the caller, and how it reaches the board.
enum class Src : uint8_t { None = 0, Ap, Wire };
enum class Up  : uint8_t { None = 0, Ip, Link };

enum class St : uint8_t { Idle = 0, Connecting, Up, Closing };

// A source carries a caller's bytes. `ctx` is whatever the source wants;
// the line only keeps it and hands it back.
struct SourceOps {
    // How many bytes the source could take right now. 0 means "not now",
    // which is backpressure and not an error.
    //
    // This is authoritative: the pump reads at most this much from the
    // uplink and then expects send() to take it.
    size_t (*room)(void* ctx);
    // All of it or none of it. True if it was taken.
    bool (*send)(void* ctx, const uint8_t* p, size_t n);
    // The caller's line is finished. `why` is in the board's own voice and
    // may be shown; a source that cannot show it ignores it.
    //
    // Called on the pump task, and never after closeFromSource().
    void (*close)(void* ctx, const char* why);
};

// What the gateway stages towards the board: a telnet opening, a reply to
// the board's negotiation, a window size, or a burst of the caller's own
// keystrokes escaped. One at a time, which is the invariant that makes a
// partial send() safe: nothing new is staged until what is staged has gone.
//
// 192 holds the opening (15 bytes), any reply burst (telnet::kReplyMax),
// a NAWS subnegotiation (13) and about 90 escaped keystrokes, which is
// more than a second of fast typing.
constexpr size_t kStageBytes = 192;

// One caller.
struct Line {
    std::atomic<bool> claimed{false};   // taken out of the pool
    std::atomic<St>   st{St::Idle};

    Src src = Src::None;
    Up  up  = Up::None;

    const SourceOps* ops    = nullptr;
    void*            ctx    = nullptr;

    int      sock   = -1;               // the IP uplink's socket
    uint8_t  board  = 0;                // which board in the list
    uint16_t cols   = 80;               // what the caller's terminal is
    uint16_t rows   = 24;
    std::atomic<bool> sized{false};     // the size changed; tell the board

    telnet::State tel;
    Ring<kToBoardBytes> toBoard;

    // The staging buffer, and the one invariant: it is refilled only when
    // it is empty, so a send() that took half of it loses nothing and the
    // order of what the board hears cannot be shuffled.
    uint8_t  stage[kStageBytes] = {0};
    uint16_t stageLen = 0;
    uint16_t stageOff = 0;

    bool     tellSource = true;      // false once the source has gone
    // When the connect was STARTED, not when it expires. A stamp in the
    // future cannot be compared against a clock safely: see line.cpp's
    // since(), and the core's own twelve sites that had to learn it.
    uint32_t connectAt  = 0;
    uint32_t openedAt = 0;
    uint32_t lastAt   = 0;              // last byte either way, for the idle clock
    uint32_t idleMs   = 0;              // 0: no idle limit on this line
    uint64_t inBytes  = 0;              // caller to board
    uint64_t outBytes = 0;              // board to caller

    char why[40] = {0};                 // why it closed, for the console and the page
};

// ---------------------------------------------------------------------------
//  The pool
// ---------------------------------------------------------------------------

// Starts the pump task. Called once, before any role.
bool begin();

// Takes a free line, or nullptr when every one is busy (which is the
// gateway's own "all lines are busy", distinct from the board's).
// The line is reset here, so nothing from its last caller survives.
Line* open(Src src, const SourceOps* ops, void* ctx, uint8_t board,
           uint16_t cols, uint16_t rows);

// Gives a line back. Safe from any task and safe twice. The line is not
// freed here: the pump reaps it, because only the pump may touch a socket.
//
// close()           the source is still there and will be told why.
// closeFromSource() the source has gone; its close() is not called.
void close(Line* l, const char* why);
void closeFromSource(Line* l, const char* why);

// Called by a source when the caller has typed something. Returns what was
// taken; a short return is backpressure and the source should offer the
// rest in a moment rather than dropping it.
size_t fromCaller(Line* l, const uint8_t* p, size_t n);

// The caller's terminal changed size. Cheap and may be called often.
void resized(Line* l, uint16_t cols, uint16_t rows);

// How many lines are busy, for the portal and the console.
uint8_t busy();

// A line by index, for the status page. Never nullptr for i < kLines.
const Line* at(uint8_t i);

}  // namespace line

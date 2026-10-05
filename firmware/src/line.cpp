// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/line.cpp
// Module:       The caller lines, and the one task that pumps them
//
// Purpose:      line.h says what a line is and why it is shaped this way.
//               This is the pool and the pump.
//
//               One task for every line, not one per line. A terminal is
//               stop-and-wait by nature (somebody types, the board answers)
//               so four lines is four mostly-idle sockets, and four tasks
//               would be four stacks to pay for the privilege of blocking
//               separately. select() with a short timeout costs one stack
//               and gives the backpressure rule somewhere to live.
//
//               The pump is pinned to core 1. Core 0 is where the Wi-Fi
//               driver runs on this part, and keeping the radio's work off
//               the path a caller's keystroke takes is the board's own rule
//               applied one box out.
//
//               Only the pump touches a socket. A source that wants a line
//               gone says so and the pump reaps it, because a source runs
//               on the HTTP server's task or the serial reader's and a
//               socket closed under select() is a file descriptor handed to
//               whoever opens the next one.
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
#include "line.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "settings.h"

namespace line {
namespace {

const char* TAG = "gw-line";

Line g_lines[kLines];

constexpr uint32_t kConnectMs = 10000;   // long enough for a board that is busy booting
constexpr uint32_t kPassMs    = 20;      // the pump's own tick
constexpr size_t   kReadMax   = 512;     // off the uplink in one pass

uint32_t ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// Elapsed time, always unsigned and always this way round. A stamp taken on
// another task can be a few milliseconds AHEAD of this pass's `now`, and an
// unsigned subtraction then reads as 49 days. The core paid for this rule
// twice; here it is one function.
uint32_t since(uint32_t now, uint32_t at) {
    const uint32_t d = now - at;
    return (d > 0xFFFF0000u) ? 0u : d;
}

void say(Line& l, const char* why) {
    if (!l.why[0] && why) snprintf(l.why, sizeof l.why, "%s", why);
}

// -------------------------------------------------------------------------
//  The uplink: a plain telnet connection to the board's own address.
//
//  Phase 1's whole uplink. No pairing, no ESP-NOW, no CALLIN, and no change
//  to the board: as far as the board is concerned this is a telnet caller
//  like any other, which is also this phase's one honest limitation (see
//  the README).
// -------------------------------------------------------------------------
bool startConnect(Line& l) {
    const settings::Board* b = settings::board(static_cast<uint8_t>(l.board + 1));
    if (!b || !b->host[0]) {
        say(l, "no address for that board");
        return false;
    }

    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port   = htons(b->port);

    if (inet_pton(AF_INET, b->host, &to.sin_addr) != 1) {
        // A name rather than an address. getaddrinfo BLOCKS, and this is
        // the one task every line shares, so resolving here would stall
        // every other caller for as long as the resolver takes. In the
        // field there is no resolver at all: the gateway's own access point
        // answers every DNS question with its own address, by design.
        //
        // So a name is refused and the sysop is told to type the address.
        // Phase 4 reaches boards by pairing and the question disappears.
        say(l, "type the board's address, not its name");
        ESP_LOGW(TAG, "board %u host '%s' is not an address", l.board + 1, b->host);
        return false;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        say(l, "this gateway has no socket free");
        return false;
    }

    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    // A terminal is small writes, so Nagle would hold every keystroke until
    // the last one was acknowledged. This is the single most important
    // socket option on the whole path.
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);

    // And keepalive, so a board that was switched off is noticed rather
    // than leaving a caller looking at a dead terminal. The same figures
    // the board uses on its own caller sockets.
    int idle = 60, intvl = 10, cnt = 3;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);

    const int r = connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof to);
    if (r != 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "connect %s:%u: errno %d", b->host, b->port, errno);
        ::close(fd);
        say(l, "could not reach that board");
        return false;
    }

    l.sock      = fd;
    l.connectAt = ms();
    l.st.store(St::Connecting, std::memory_order_release);
    return true;
}

// Writability says a non-blocking connect has finished; SO_ERROR says
// whether it worked. The two are not interchangeable: SO_ERROR is 0 while
// the connect is still in progress, so testing it first reports success for
// a connection that has not happened. On loopback it would even appear to
// work, which is how this gets shipped.
bool connectFinished(Line& l) {
    int err = 0;
    socklen_t n = sizeof err;
    if (getsockopt(l.sock, SOL_SOCKET, SO_ERROR, &err, &n) != 0 || err != 0) {
        ESP_LOGW(TAG, "connect failed: errno %d", err);
        say(l, "the board refused the connection");
        return false;
    }
    l.connectAt = 0;
    l.st.store(St::Up, std::memory_order_release);

    // Speak first, which is what gets character mode and the board's
    // immediate terminal probe rather than its key prompt.
    size_t n2 = telnet::hello(l.stage, sizeof l.stage);
    l.stageLen = static_cast<uint16_t>(n2);
    l.stageOff = 0;
    return true;
}

// Fills the staging buffer when it is empty. The order is the order the
// board hears: anything the protocol owes first, then a window size, then
// the caller's own typing.
void stageNext(Line& l) {
    if (l.stageOff < l.stageLen) return;        // still sending the last one
    l.stageLen = 0;
    l.stageOff = 0;

    if (l.sized.exchange(false, std::memory_order_acq_rel)) {
        const size_t n = telnet::naws(l.tel, l.stage, sizeof l.stage, l.cols, l.rows);
        if (n) { l.stageLen = static_cast<uint16_t>(n); return; }
        // The board has not said DO NAWS, so there is nothing to send and
        // nothing to remember: it will ask if it wants to know.
    }

    // Half the staging buffer at a time, because escaping can double it.
    uint8_t raw[kStageBytes / 2];
    const size_t got = l.toBoard.peek(raw, sizeof raw);
    if (!got) return;
    const size_t w = telnet::escape(raw, got, l.stage, sizeof l.stage);
    if (!w) return;                             // cannot happen at this size; refuse rather than split
    l.toBoard.drop(got);
    l.stageLen = static_cast<uint16_t>(w);
    l.inBytes += got;
}

// Hands the board whatever is staged. A partial write is normal and is why
// stageOff exists.
bool drain(Line& l) {
    while (l.stageOff < l.stageLen) {
        const int w = send(l.sock, l.stage + l.stageOff,
                           static_cast<size_t>(l.stageLen - l.stageOff), 0);
        if (w > 0) {
            l.stageOff = static_cast<uint16_t>(l.stageOff + w);
            l.lastAt = ms();
            continue;
        }
        if (w < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) return true;
        say(l, "lost the board");
        return false;
    }
    return true;
}

// Reads the board and gives it to the caller.
//
// Only while the staging buffer is empty, which is the invariant that makes
// a reply from telnet::filter always fit, and only as much as the source
// says it can take, which is the backpressure rule.
bool pumpDown(Line& l) {
    if (l.stageOff < l.stageLen) return true;           // staging is busy
    size_t room = l.ops->room(l.ctx);
    if (room == 0) return true;                         // the caller is not keeping up
    if (room > kReadMax) room = kReadMax;

    uint8_t buf[kReadMax];
    const int got = recv(l.sock, buf, room, 0);
    if (got == 0) {
        say(l, "the board hung up");
        return false;
    }
    if (got < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) return true;
        say(l, "lost the board");
        return false;
    }
    l.lastAt = ms();

    size_t replyLen = 0;
    uint8_t reply[telnet::kReplyMax];
    const size_t data = telnet::filter(l.tel, buf, static_cast<size_t>(got),
                                       reply, sizeof reply, replyLen);
    if (replyLen) {
        // Fits by construction: staging is empty here and kStageBytes is
        // three times kReplyMax. Checked anyway, because a later change to
        // either number should fail loudly rather than quietly truncate.
        if (replyLen <= sizeof l.stage) {
            memcpy(l.stage, reply, replyLen);
            l.stageLen = static_cast<uint16_t>(replyLen);
            l.stageOff = 0;
        } else {
            ESP_LOGE(TAG, "telnet reply %u will not stage", static_cast<unsigned>(replyLen));
        }
    }
    if (data) {
        // room() was authoritative and filter() only ever shrinks, so this
        // cannot be refused. If it ever is, the bytes are lost and the
        // console says so rather than the terminal silently gapping.
        if (!l.ops->send(l.ctx, buf, data)) {
            ESP_LOGW(TAG, "node %d: source refused %u bytes it had room for",
                     static_cast<int>(&l - g_lines), static_cast<unsigned>(data));
        } else {
            l.outBytes += data;
        }
    }
    return true;
}

void reap(Line& l) {
    if (l.sock >= 0) {
        ::close(l.sock);
        l.sock = -1;
    }
    if (l.tellSource && l.ops && l.ops->close) {
        l.ops->close(l.ctx, l.why[0] ? l.why : "the line closed");
    }
    ESP_LOGI(TAG, "line %d closed: %s (in %llu out %llu)",
             static_cast<int>(&l - g_lines), l.why[0] ? l.why : "-",
             static_cast<unsigned long long>(l.inBytes),
             static_cast<unsigned long long>(l.outBytes));
    l.ops = nullptr;
    l.ctx = nullptr;
    l.st.store(St::Idle, std::memory_order_release);
    l.claimed.store(false, std::memory_order_release);
}

void pumpTask(void*) {
    for (;;) {
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        int maxfd = -1;
        bool any = false;
        const uint32_t now = ms();

        for (Line& l : g_lines) {
            const St st = l.st.load(std::memory_order_acquire);
            if (st == St::Idle) continue;
            if (st == St::Closing) { reap(l); continue; }
            if (l.sock < 0) { say(l, "no socket"); l.st.store(St::Closing); continue; }

            if (st == St::Connecting) {
                if (since(now, l.connectAt) > kConnectMs) {
                    say(l, "that board did not answer");
                    l.st.store(St::Closing);
                    continue;
                }
                FD_SET(l.sock, &wr);
            } else {
                // Read only while the caller can take it, which is the
                // whole of the backpressure rule.
                if (l.stageOff >= l.stageLen && l.ops->room(l.ctx) > 0) FD_SET(l.sock, &rd);
                stageNext(l);
                if (l.stageOff < l.stageLen) FD_SET(l.sock, &wr);

                if (l.idleMs && since(now, l.lastAt) > l.idleMs) {
                    say(l, "quiet too long; the line was freed");
                    l.st.store(St::Closing);
                    continue;
                }
            }
            if (l.sock > maxfd) maxfd = l.sock;
            any = true;
        }

        if (!any) {
            vTaskDelay(pdMS_TO_TICKS(kPassMs));
            continue;
        }

        timeval tv = { 0, static_cast<long>(kPassMs) * 1000 };
        const int r = select(maxfd + 1, &rd, &wr, nullptr, &tv);
        if (r < 0) {
            ESP_LOGW(TAG, "select: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(kPassMs));
            continue;
        }
        if (r == 0) continue;

        for (Line& l : g_lines) {
            const St st = l.st.load(std::memory_order_acquire);
            if (l.sock < 0) continue;
            if (st == St::Connecting) {
                if (FD_ISSET(l.sock, &wr) && !connectFinished(l)) l.st.store(St::Closing);
                continue;
            }
            if (st != St::Up) continue;
            if (FD_ISSET(l.sock, &wr) && !drain(l)) { l.st.store(St::Closing); continue; }
            if (FD_ISSET(l.sock, &rd) && !pumpDown(l)) { l.st.store(St::Closing); continue; }
        }
    }
}

}  // namespace

// -------------------------------------------------------------------------
bool begin() {
    for (Line& l : g_lines) {
        l.sock = -1;
        l.st.store(St::Idle);
        l.claimed.store(false);
    }
    // Core 1: the radio owns core 0 on this part, and a keystroke's path
    // should not queue behind it. Priority 5 is above the idle tasks and
    // well below the Wi-Fi driver's and lwIP's, so neither waits for us.
    const BaseType_t ok = xTaskCreatePinnedToCore(pumpTask, "gw-pump", 4096, nullptr,
                                                  5, nullptr, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "cannot start the pump task");
        return false;
    }
    ESP_LOGI(TAG, "%u lines, %u bytes of ring and staging each",
             static_cast<unsigned>(kLines),
             static_cast<unsigned>(kToBoardBytes + kStageBytes));
    return true;
}

Handle open(Src src, const SourceOps* ops, void* ctx, uint8_t board,
            uint16_t cols, uint16_t rows) {
    if (!ops || !ops->room || !ops->send || !ops->close) return Handle{};

    for (Line& l : g_lines) {
        bool was = false;
        if (!l.claimed.compare_exchange_strong(was, true, std::memory_order_acq_rel)) {
            continue;
        }
        // Everything is reset HERE and not in reap(), because a line comes
        // from a static pool: anything an exit path forgot would be
        // inherited by the next caller on this line, which is the shape of
        // every leak of this kind in the core.
        l.src        = src;
        l.up         = Up::Ip;
        l.ops        = ops;
        l.ctx        = ctx;
        l.board      = board;
        l.cols       = cols ? cols : 80;
        l.rows       = rows ? rows : 24;
        l.sized.store(false, std::memory_order_relaxed);
        l.tel        = telnet::State{};
        l.toBoard.clear();
        l.stageLen   = 0;
        l.stageOff   = 0;
        l.tellSource = true;
        l.sock       = -1;
        l.connectAt  = 0;
        l.openedAt   = ms();
        l.lastAt     = l.openedAt;
        l.idleMs     = 0;
        l.inBytes    = 0;
        l.outBytes   = 0;
        l.why[0]     = 0;
        // Last, and never reset to 0: this is what tells a stale handle
        // from a live one, so it counts up for the life of the board.
        const uint32_t serial = l.serial.fetch_add(1, std::memory_order_acq_rel) + 1;

        if (!startConnect(l)) {
            // Tell nobody: the caller of open() is the source and it has
            // not been handed the line yet, so it reports its own failure.
            if (l.sock >= 0) { ::close(l.sock); l.sock = -1; }
            const char* why = l.why[0] ? l.why : "could not reach that board";
            ESP_LOGW(TAG, "open refused: %s", why);
            l.ops = nullptr;
            l.ctx = nullptr;
            l.st.store(St::Idle, std::memory_order_release);
            l.claimed.store(false, std::memory_order_release);
            return Handle{};
        }
        ESP_LOGI(TAG, "line %d open (caller %u), board %u, %ux%u",
                 static_cast<int>(&l - g_lines), static_cast<unsigned>(serial),
                 board + 1, cols, rows);
        return Handle{ &l, serial };
    }
    return Handle{};
}

bool live(Handle h) {
    return h.l && h.l->serial.load(std::memory_order_acquire) == h.serial &&
           h.l->st.load(std::memory_order_acquire) != St::Idle;
}

void close(Handle h, const char* why) {
    if (!live(h)) return;
    Line* l = h.l;
    say(*l, why);
    // Checked again after the write below cannot help: the serial is read
    // once more so a reap that happened in between does not leave this
    // call marking the NEXT caller's line as closing.
    if (l->serial.load(std::memory_order_acquire) != h.serial) return;
    l->st.store(St::Closing, std::memory_order_release);
}

void closeFromSource(Handle h, const char* why) {
    if (!live(h)) return;
    // Set before the state, so the pump cannot reap between the two and
    // call back into a source that has gone.
    h.l->tellSource = false;
    close(h, why);
}

size_t fromCaller(Handle h, const uint8_t* p, size_t n) {
    if (!live(h)) return 0;
    Line* l = h.l;
    if (l->st.load(std::memory_order_acquire) != St::Up) return 0;
    const size_t took = l->toBoard.push(p, n);
    if (took) l->lastAt = ms();
    return took;
}

void resized(Handle h, uint16_t cols, uint16_t rows) {
    if (!live(h) || !cols || !rows) return;
    Line* l = h.l;
    // Clamp rather than refuse: a browser in a strange state can report
    // nonsense, and a terminal drawn at 1 column is worse than one drawn at
    // the last sane size. 255 is also where a NAWS byte would need escaping
    // in a way no board expects.
    if (cols > 240) cols = 240;
    if (rows > 240) rows = 240;
    if (l->cols == cols && l->rows == rows) return;
    l->cols = cols;
    l->rows = rows;
    l->sized.store(true, std::memory_order_release);
}

uint8_t busy() {
    uint8_t n = 0;
    for (const Line& l : g_lines) {
        if (l.st.load(std::memory_order_acquire) != St::Idle) ++n;
    }
    return n;
}

const Line* at(uint8_t i) { return (i < kLines) ? &g_lines[i] : nullptr; }

}  // namespace line

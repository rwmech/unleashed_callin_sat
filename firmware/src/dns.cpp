// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/dns.cpp
// Module:       The DNS redirector
//
// Purpose:      dns.h says what and why. This is the wire format.
//
//               Every bound in here is there because the packets come from
//               strangers: this listens on an open network that anybody in
//               radio range can join, so a malformed question must produce
//               a dropped packet and never a loop or a read past the end.
//               Each check below says which malformation it is for.
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
#include "dns.h"

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

namespace dns {
namespace {

const char* TAG = "gw-dns";

// A DNS message over UDP is 512 bytes without EDNS0. A question longer than
// that is not one this box needs to answer.
constexpr size_t kMsgMax = 512;

constexpr uint16_t kTypeA    = 1;
constexpr uint16_t kTypeAAAA = 28;
constexpr uint16_t kClassIn  = 1;

int           g_sock = -1;
// The /24 this gateway's access point hands out, in network order. Only a
// query from inside it is answered: the socket is bound to every
// interface, so with a house network joined as well this would otherwise
// answer DNS on the HOME LAN with its own address, racing the router and
// breaking name resolution there intermittently. dns.h is unambiguous that
// this is the access point's own responder.
uint32_t      g_net  = 0;
TaskHandle_t  g_task = nullptr;
volatile bool g_run  = false;
uint32_t      g_count = 0;
uint32_t      g_addr  = 0;      // network order

uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

void wr16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

// Walks one question's name and returns the offset just past its QTYPE and
// QCLASS, or 0 if the question is malformed.
//
// Three malformations are refused here, and each of them is a real packet
// somebody has sent a DNS server:
//   - a label length of 64 or more, which is the two high bits set and so
//     a COMPRESSION POINTER. A pointer in a question is illegal, and
//     following one is how a parser is made to loop for ever on a packet
//     that points at itself;
//   - a name that never terminates before the end of the packet;
//   - a name whose labels add up past 255, the protocol's own limit.
size_t walkName(const uint8_t* msg, size_t n, size_t at, size_t& nameLen) {
    nameLen = 0;
    while (at < n) {
        const uint8_t len = msg[at];
        if (len == 0) {
            ++at;
            // QTYPE and QCLASS, four bytes, must also be inside the packet.
            return (at + 4 <= n) ? at + 4 : 0;
        }
        if (len >= 0x40) return 0;                   // a pointer, or a reserved form
        if (nameLen + len + 1 > 255) return 0;
        at += static_cast<size_t>(len) + 1;
        nameLen += static_cast<size_t>(len) + 1;
    }
    return 0;                                        // ran off the end
}

void task(void*) {
    uint8_t msg[kMsgMax];
    while (g_run) {
        sockaddr_in from = {};
        socklen_t   flen = sizeof from;
        const int got = recvfrom(g_sock, msg, sizeof msg, 0,
                                 reinterpret_cast<sockaddr*>(&from), &flen);
        if (got < 0) {
            if (!g_run) break;
            // A socket error on a UDP listener is almost always the netif
            // going down under us. Pause rather than spin: a tight loop
            // here would starve the idle task into the watchdog.
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        // Not our network, not our question. Checked before anything is
        // parsed, so a stranger on another interface cannot even reach the
        // parser.
        if (g_net && (from.sin_addr.s_addr & 0x00FFFFFFu) != g_net) continue;

        const size_t n = static_cast<size_t>(got);
        if (n < 12) continue;                        // shorter than a header

        const uint16_t flags = rd16(msg + 2);
        if (flags & 0x8000) continue;                // already an answer; not ours to answer
        if (((flags >> 11) & 0x0F) != 0) continue;    // not a standard query (OPCODE 0)

        const uint16_t qd = rd16(msg + 4);
        if (qd != 1) continue;                       // exactly one question, which is every real one

        size_t nameLen = 0;
        const size_t qEnd = walkName(msg, n, 12, nameLen);
        if (!qEnd) continue;

        const uint16_t qtype  = rd16(msg + qEnd - 4);
        const uint16_t qclass = rd16(msg + qEnd - 2);
        if (qclass != kClassIn) continue;

        // The reply is the question with the header rewritten, which is what
        // keeps the name's bytes exactly as they were asked and lets the
        // answer point at offset 12 rather than repeating them.
        uint8_t out[kMsgMax];
        if (qEnd + 16 > sizeof out) continue;        // no room for the answer record
        memcpy(out, msg, qEnd);
        size_t w = qEnd;

        const bool answerable = (qtype == kTypeA);
        // QR=1, AA=1, RD copied from the question, RA=0, RCODE=0.
        // NOERROR with no records for anything but A: that tells a phone
        // "there is no AAAA here" and it stops waiting, where NXDOMAIN
        // would tell it the NAME does not exist and some resolvers then
        // give up on the A as well.
        wr16(out + 2, static_cast<uint16_t>(0x8400 | (flags & 0x0100)));
        wr16(out + 4, 1);                            // QDCOUNT
        wr16(out + 6, answerable ? 1 : 0);           // ANCOUNT
        wr16(out + 8, 0);                            // NSCOUNT
        wr16(out + 10, 0);                           // ARCOUNT

        if (answerable) {
            out[w++] = 0xC0; out[w++] = 0x0C;        // the name, as a pointer to offset 12
            wr16(out + w, kTypeA);   w += 2;
            wr16(out + w, kClassIn); w += 2;
            // TTL 0: see dns.h. A cached hijack travels with the phone.
            out[w++] = 0; out[w++] = 0; out[w++] = 0; out[w++] = 0;
            wr16(out + w, 4); w += 2;
            memcpy(out + w, &g_addr, 4); w += 4;     // already network order
            ++g_count;
        }

        sendto(g_sock, out, w, 0, reinterpret_cast<sockaddr*>(&from), flen);
    }
    g_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

bool begin(const char* dotted) {
    if (g_sock >= 0) return true;
    if (inet_pton(AF_INET, dotted, &g_addr) != 1) {
        ESP_LOGE(TAG, "'%s' is not an address", dotted);
        return false;
    }

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock < 0) {
        ESP_LOGE(TAG, "no socket for the DNS responder: errno %d", errno);
        return false;
    }
    sockaddr_in me = {};
    me.sin_family      = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    me.sin_port        = htons(53);
    if (bind(g_sock, reinterpret_cast<sockaddr*>(&me), sizeof me) != 0) {
        ESP_LOGE(TAG, "cannot bind port 53: errno %d", errno);
        ::close(g_sock);
        g_sock = -1;
        return false;
    }

    g_net = g_addr & 0x00FFFFFFu;

    g_run = true;
    if (xTaskCreate(task, "gw-dns", 3072, nullptr, 4, &g_task) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the DNS task");
        g_run = false;
        ::close(g_sock);
        g_sock = -1;
        return false;
    }
    ESP_LOGI(TAG, "every name answers as %s", dotted);
    return true;
}

void stop() {
    g_run = false;
    if (g_sock >= 0) {
        // Closing the socket is what wakes the task out of recvfrom.
        ::close(g_sock);
        g_sock = -1;
    }
}

uint32_t answered() { return g_count; }

}  // namespace dns

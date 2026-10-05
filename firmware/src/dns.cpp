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

#include <atomic>
#include <cstring>

// The wire format lives in a header with no platform in it, so the host
// tests drive exactly the code that ships. See dnsparse.h for why that
// matters here more than elsewhere: every byte comes from a stranger.
#include "dnsparse.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

namespace dns {
namespace {

const char* TAG = "gw-dns";

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
// Written on the DNS task, read by main's console line.
std::atomic<uint32_t> g_count{0};
uint32_t      g_addr  = 0;      // network order

void task(void*) {
    uint8_t msg[dnsparse::kMsgMax];
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

        const dnsparse::Question q = dnsparse::read(msg, static_cast<size_t>(got));
        if (!q.ok) continue;

        uint8_t out[dnsparse::kMsgMax];
        const size_t w = dnsparse::reply(msg, static_cast<size_t>(got), q,
                                         g_addr, out, sizeof out);
        if (!w) continue;
        if (q.isA) g_count.fetch_add(1, std::memory_order_relaxed);

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

uint32_t answered() { return g_count.load(std::memory_order_relaxed); }

}  // namespace dns

// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/main.cpp
// Module:       Boot, and the three roles
//
// Purpose:      Reads the settings and starts whichever roles they ask for.
//               There is no #ifdef on a role anywhere in this firmware:
//               that is the design, and this file is where it shows. Three
//               build variants would be the two-box design wearing one
//               repository, and flash is what pays for one image: the
//               whole web payload is about 80 KB against roughly 3.3 MB
//               free on a 4 MB part.
//
//               Phase 1's roles: the access point (an open network, a
//               portal, a browser terminal) and the terminal server (a
//               serial port carrying one caller). Both at once, which is
//               the point. The repeater is a setting and a refusal; see
//               repeat.h for what it will be and why it is not here.
//
//               The uplink is IP: a plain telnet connection to the board's
//               own address, because either the board joined THIS
//               gateway's access point (the field case, which also settles
//               the channel) or both are on a router (the home case).
//               Nothing on the board changes for any of it.
//
//               The console says what it is and what it is not, every
//               boot, because a box planted on a post has no other way to
//               tell anybody and because "no roles on" is a real state a
//               gateway sits in between its flash and its setup.
//
// Targets:      ESP32 (ESP-IDF 5.3.1). See board.h for the profiles.
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
#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ap.h"
#include "board.h"
#include "dns.h"
#include "line.h"
#include "repeat.h"
#include "settings.h"
#include "uart.h"
#include "version.h"
#include "web.h"

namespace {

const char* TAG = "gateway";

// Internal heap, which is the one figure phase 1 exists to learn besides
// "does the portal pop". Flash is not the worry on a 4 MB part; internal
// RAM on a part with no PSRAM is, because a SoftAP with a dozen stations,
// lwIP with sixteen sockets and an HTTP server with its own task is a great
// deal of heap for a WROOM's usable DRAM.
//
// The lowest ever seen matters more than the figure right now: a gateway
// that has 40 KB free and dipped to 3 KB while four phones joined at once
// is a gateway that will fail on the day it is busy.
void heapLine(const char* when) {
    const size_t freeNow = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t low     = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL |
                                                           MALLOC_CAP_8BIT);
    const size_t big     = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                            MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "heap %s: %u free, %u lowest, %u biggest block",
             when, static_cast<unsigned>(freeNow), static_cast<unsigned>(low),
             static_cast<unsigned>(big));
}

// Stack headroom for every task this firmware starts, by name, because
// the figure that matters is the LOWEST a stack ever got and nothing else
// can see it. An overflow panics and reboots, which on a box in a field is
// a reboot nobody can explain.
//
// The project's own rule, learnt the hard way on the board: the cure for
// reasoning about a thing from the outside is making it say. Phase 2 reads
// these off the console with the access point up and callers on.
void stackLine() {
    static const char* kTasks[] = { "gw-pump", "gw-dns", "gw-wire", "httpd" };
    char line[128];
    int w = snprintf(line, sizeof line, "stack free:");
    for (const char* name : kTasks) {
        const TaskHandle_t h = xTaskGetHandle(name);
        if (!h) continue;
        const UBaseType_t low = uxTaskGetStackHighWaterMark(h);
        if (w < static_cast<int>(sizeof line)) {
            w += snprintf(line + w, sizeof line - static_cast<size_t>(w),
                          " %s %u,", name, static_cast<unsigned>(low));
        }
    }
    if (w > 0 && line[w - 1] == ',') line[w - 1] = 0;
    ESP_LOGI(TAG, "%s", line);
}

void sayWhatIAm() {
    const settings::Cfg& c = settings::get();
    char nm[24];
    settings::name(nm, sizeof nm);

    ESP_LOGI(TAG, "%s " GW_VERSION " (" GW_BOARD_TAG " " GW_BOARD_VERSION "), \"%s\"",
             GW_WHAT, nm);
    ESP_LOGI(TAG, "roles: access point %s, terminal server %s, repeater %s",
             c.ap ? "ON" : "off", c.termsrv ? "ON" : "off",
             repeat::available ? (c.repeat ? "ON" : "off") : "not in this build");

    uint8_t boards = 0;
    for (uint8_t i = 1; i <= settings::kBoards; ++i) {
        if (settings::board(i)) ++boards;
    }
    ESP_LOGI(TAG, "%u board%s set up, %u caller line%s on this gateway",
             boards, boards == 1 ? "" : "s",
             static_cast<unsigned>(line::kLines), line::kLines == 1 ? "" : "s");

    // What this says has to be true in each state, and the first version
    // was not: it told the operator to "switch on the access point", which
    // is advice they cannot act on, because the portal is the only way to
    // change a setting and the portal is what is off. A message asserting
    // something the code cannot do is this project's own recurring shape.
    if (c.ap) {
        // The default, and the way in. The address is logged by ap.cpp.
        ESP_LOGI(TAG, "set this gateway up at http://%s/ on its own Wi-Fi",
                 ap::addr());
    } else if (c.termsrv) {
        ESP_LOGW(TAG, "the access point is off, so there is no portal and no "
                      "way to change a setting from here.");
        ESP_LOGW(TAG, "the serial line still works. To get back in, erase "
                      "this gateway's settings: pio run -t erase");
    } else {
        // Legal, and a sysop may have meant it. Said as a state, with the
        // honest way back rather than advice that cannot be taken.
        ESP_LOGW(TAG, "every role is off, so this gateway does nothing.");
        ESP_LOGW(TAG, "and there is no way to change that from here: the "
                      "portal is the only way in and it is off.");
        ESP_LOGW(TAG, "to start again: pio run -t erase, which clears the "
                      "settings a reflash on its own would keep.");
    }
    if (!boards) {
        ESP_LOGW(TAG, "no boards are set up, so there is nothing for a caller "
                      "to call. Add one on the portal's setup page.");
    }
    if (!settings::passwordSet() && c.ap) {
        // Three different situations, and "open network" was the wording for
        // only one of them. The published password is barely better than
        // open here: it is meant to be printed on a sign, so it keeps a
        // passer-by off the AIR and nobody out of the setup page. Saying
        // "open" on a WPA2 box was wrong, and saying "Wi-Fi" on a box still
        // on the published word would have been worse, because it implies a
        // barrier that is written on the sign next to it.
        if (!c.apPass[0]) {
            ESP_LOGW(TAG, "this gateway's setup page has NO PASSWORD and its "
                          "network is open: anyone in range can change its "
                          "settings. Set one.");
        } else if (strcmp(c.apPass, GW_AP_PASS_PUBLISHED) == 0) {
            ESP_LOGW(TAG, "this gateway's setup page has NO PASSWORD and its "
                          "network is on the published \"" GW_AP_PASS_PUBLISHED
                          "\": anyone who reads the sign can change its "
                          "settings. Set one.");
        } else {
            ESP_LOGW(TAG, "this gateway's setup page has NO PASSWORD: anyone "
                          "on its Wi-Fi can change its settings. Set one.");
        }
    }
}

}  // namespace

extern "C" void app_main() {
    esp_log_level_set("*", ESP_LOG_INFO);

    const esp_app_desc_t* app = esp_app_get_description();
    ESP_LOGI(TAG, "----");
    ESP_LOGI(TAG, "built %s %s, idf %s", app->date, app->time, app->idf_ver);
    heapLine("at boot");

    if (!settings::begin()) {
        // Running on defaults. Everything still works; nothing is kept.
        ESP_LOGW(TAG, "settings could not be read, so these are the defaults "
                      "and nothing will be saved.");
    }
    sayWhatIAm();

    // The lines come up before any role, because a role is a source and a
    // source with nowhere to put a caller is a role that cannot start.
    if (!line::begin()) {
        ESP_LOGE(TAG, "the caller lines could not start; rebooting");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    // The radio: an access point, a station, or both, as the settings ask.
    const bool radio = ap::begin();

    if (radio && ap::apUp()) {
        // Both halves of the portal popping up, in order: every name
        // answers as this box, then this box answers every probe.
        dns::begin(ap::addr());
        web::begin();
    }

    // The terminal server, which works whether or not the access point is
    // up. That is the whole claim of the three-switch design and it is one
    // call here with no condition on the other role.
    uartsrv::begin();

    heapLine("with the roles up");
    stackLine();
    ESP_LOGI(TAG, "web payload %u bytes of gzip in the app image",
             static_cast<unsigned>(web::payloadBytes()));
    ESP_LOGI(TAG, "----");

    // One slow tick. Nothing a caller waits on happens here: the pump has
    // its own task, the serial reader has its own, and the HTTP server has
    // its own. This is the console's clock and the station's redial.
    uint32_t n = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ap::tick();
        ++n;

        // Every five minutes, and only when something has happened. A box
        // on a post is read through its console log weeks later, and a line
        // a second would bury what mattered.
        if (n % 300 == 0) {
            const uint8_t busy = line::busy();
            if (busy || ap::phones() || uartsrv::busy()) {
                ESP_LOGI(TAG, "%u of %u lines, %u phones, wire %s, %u names "
                              "answered",
                         busy, static_cast<unsigned>(line::kLines), ap::phones(),
                         uartsrv::busy() ? "busy" : "free",
                         static_cast<unsigned>(dns::answered()));
                heapLine("now");
                stackLine();
            }
        }
    }
}

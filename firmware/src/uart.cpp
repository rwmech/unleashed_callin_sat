// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/uart.cpp
// Module:       The terminal server role
//
// Purpose:      uart.h says what and why. This is the port and its reader.
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
#include "uart.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "line.h"
#include "settings.h"

namespace uartsrv {
namespace {

const char* TAG = "gw-wire";

// The driver's own rings, which are where a terminal's bytes wait while the
// pump is busy with another line. 2 KB in and 2 KB out: a screen redraw at
// 9600 baud takes two seconds to reach the glass, so there is no point in a
// bigger one, and the board's own output is what the pump throttles anyway.
constexpr int kRxRing = 2048;
constexpr int kTxRing = 2048;

constexpr size_t kReadChunk = 128;

bool g_up = false;
line::Handle g_ln;
std::atomic<bool> g_want{false};     // a caller should be on the line
TaskHandle_t g_task = nullptr;
volatile bool g_run = false;

uint32_t ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// --- the source ----------------------------------------------------------
size_t wireRoom(void*) {
    size_t free_ = 0;
    if (uart_get_tx_buffer_free_size(static_cast<uart_port_t>(GW_UART_PORT), &free_)
        != ESP_OK) {
        return 0;
    }
    // Room is authoritative: the pump reads at most this much and then
    // expects send() to take it, so uart_tx_chars cannot come up short.
    return free_;
}

bool wireSend(void*, const uint8_t* p, size_t n) {
    // uart_tx_chars, not uart_write_bytes: write_bytes BLOCKS until the
    // whole lot is queued, and this runs on the pump task, which every
    // other caller's line shares. A terminal at 300 baud would hold them
    // all. tx_chars queues what fits and returns, which is why room() is
    // asked first.
    const int w = uart_tx_chars(static_cast<uart_port_t>(GW_UART_PORT),
                                reinterpret_cast<const char*>(p), n);
    return w == static_cast<int>(n);
}

void wireClose(void*, const char* why) {
    // The terminal is a physical thing that cannot be closed, so this is a
    // message and a reset of the state. The words go out as the board's own
    // line, because somebody is sitting in front of it wondering.
    char msg[64];
    const int n = snprintf(msg, sizeof msg, "\r\n--> %s\r\n",
                           why && *why ? why : "The line closed.");
    if (n > 0) {
        uart_write_bytes(static_cast<uart_port_t>(GW_UART_PORT), msg,
                         static_cast<size_t>(n));
    }
    g_ln.clear();
    // Not re-dialled at once: a terminal whose board has gone would
    // otherwise reconnect in a loop. The next keystroke calls again.
    g_want.store(false, std::memory_order_release);
}

const line::SourceOps kWireOps = { wireRoom, wireSend, wireClose };

// --- which board, and the opening line -----------------------------------
void offer() {
    const settings::Cfg& c = settings::get();
    char msg[192];
    int n = 0;
    if (c.serBoard) {
        const settings::Board* b = settings::board(c.serBoard);
        n = snprintf(msg, sizeof msg, "\r\n--> Calling %s...\r\n",
                     b ? b->name : "the board");
    } else {
        // Blank means ask. A list rather than a guess, because a gateway
        // with five boards and no choice made would otherwise pick one
        // silently.
        n = snprintf(msg, sizeof msg, "\r\n--> Which board? Press a number.\r\n");
        for (uint8_t i = 1; i <= settings::kBoards && n > 0; ++i) {
            const settings::Board* b = settings::board(i);
            if (!b) continue;
            n += snprintf(msg + n, sizeof msg - static_cast<size_t>(n),
                          "    %u  %s\r\n", i, b->name);
            if (n >= static_cast<int>(sizeof msg)) { n = sizeof msg - 1; break; }
        }
    }
    if (n > 0) {
        uart_write_bytes(static_cast<uart_port_t>(GW_UART_PORT), msg,
                         static_cast<size_t>(n));
    }
}

bool dial(uint8_t oneBased) {
    const settings::Board* b = settings::board(oneBased);
    if (!b) return false;

    // 80x24 is the honest default for a terminal that cannot be asked. A
    // VT220 is 80x24; a wider one will redraw at 80 and look narrow, which
    // is better than a narrower one wrapping every line. There is no NAWS
    // from a serial terminal to correct it with, so this is a setting worth
    // adding the day somebody wires a 132-column terminal.
    g_ln = line::open(line::Src::Wire, &kWireOps, nullptr,
                      static_cast<uint8_t>(oneBased - 1), 80, 24);
    if (!g_ln.set()) {
        const char* msg = "\r\n--> Could not reach that board.\r\n";
        uart_write_bytes(static_cast<uart_port_t>(GW_UART_PORT), msg, strlen(msg));
        return false;
    }

    // The connection line, in the board's own voice. In phase 1 the second
    // hop is plain telnet over IP, so this says so: once CALLIN seals it
    // in phase 3 the wording becomes "Wired line, then encrypted", which
    // is the one CALLIN path that is honestly private end to end.
    const char* warn = "--> Wired line, then plain telnet\r\n";
    uart_write_bytes(static_cast<uart_port_t>(GW_UART_PORT), warn, strlen(warn));
    return true;
}

// --- the reader ----------------------------------------------------------
void task(void*) {
    uint8_t buf[kReadChunk];
    uint32_t lastKey = ms();
    bool offered = false;

    while (g_run) {
        const int got = uart_read_bytes(static_cast<uart_port_t>(GW_UART_PORT), buf,
                                        sizeof buf, pdMS_TO_TICKS(50));
        const bool haveLine = line::live(g_ln);

        if (got <= 0) {
            // Nobody has typed. If a line is up, its own idle clock is the
            // pump's; if one is not, offer the board list once so somebody
            // walking up to a cold terminal sees something.
            if (!haveLine && !offered && settings::get().serBoard == 0) {
                offer();
                offered = true;
            }
            continue;
        }
        lastKey = ms();
        (void)lastKey;

        if (!haveLine) {
            offered = false;
            const settings::Cfg& c = settings::get();
            if (c.serBoard) {
                // A fixed board: any keystroke calls it, and the keystroke
                // itself is NOT passed on. It was a knock, not input, and
                // sending it would put a stray character into the handle
                // prompt (which is exactly the bug the board's own setup
                // flow had with a stray Enter).
                offer();
                if (!dial(c.serBoard)) continue;
            } else {
                // Asking: the first digit that names a board calls it, and
                // anything else re-offers rather than being swallowed.
                uint8_t pick = 0;
                for (int i = 0; i < got; ++i) {
                    if (buf[i] >= '1' && buf[i] <= '0' + settings::kBoards) {
                        pick = static_cast<uint8_t>(buf[i] - '0');
                        break;
                    }
                }
                if (!pick) { offer(); continue; }
                if (!dial(pick)) continue;
            }
            // The line's idle clock, in the pump's own units.
            if (line::live(g_ln) && g_ln.l) {
                g_ln.l->idleMs = static_cast<uint32_t>(settings::get().serIdle)
                                 * 60u * 1000u;
            }
            continue;
        }

        // A caller is on the line. A short take is backpressure; offer the
        // rest in slices rather than dropping a keystroke, bounded so a
        // wedged board cannot hold this task for ever.
        size_t at = 0;
        for (int tries = 0; at < static_cast<size_t>(got) && tries < 20; ++tries) {
            at += line::fromCaller(g_ln, buf + at, static_cast<size_t>(got) - at);
            if (at < static_cast<size_t>(got)) vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (at < static_cast<size_t>(got)) {
            ESP_LOGW(TAG, "dropped %u typed bytes: the board is not keeping up",
                     static_cast<unsigned>(static_cast<size_t>(got) - at));
        }
    }
    g_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

// -------------------------------------------------------------------------
bool begin() {
    if (g_up) return true;
    const settings::Cfg& c = settings::get();
    if (!c.termsrv) return false;

    // Refused here as well as on the form, because a hand-written NVS or a
    // settings file from another board profile can still name a pin this
    // one cannot have. The core's own rule: the form is where a sysop is
    // told, and the start-up is where it is enforced.
    if (c.serTx < 0 || c.serRx < 0) {
        ESP_LOGW(TAG, "the terminal server is on but has no TX or RX pin");
        return false;
    }
    const char* p = board::pinProblem(c.serTx, true);
    if (p) { ESP_LOGE(TAG, "TX on GPIO %d is %s", c.serTx, p); return false; }
    p = board::pinProblem(c.serRx, false);
    if (p) { ESP_LOGE(TAG, "RX on GPIO %d is %s", c.serRx, p); return false; }

    uart_config_t u = {};
    u.baud_rate = static_cast<int>(c.serBaud);
    switch (c.serFmt) {
        case settings::FMT_7E1:
            u.data_bits = UART_DATA_7_BITS;
            u.parity    = UART_PARITY_EVEN;
            break;
        case settings::FMT_7N1:
            u.data_bits = UART_DATA_7_BITS;
            u.parity    = UART_PARITY_DISABLE;
            break;
        default:
            u.data_bits = UART_DATA_8_BITS;
            u.parity    = UART_PARITY_DISABLE;
            break;
    }
    u.stop_bits = UART_STOP_BITS_1;
    u.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;   // see settings.cpp: refused by name
    u.source_clk = UART_SCLK_DEFAULT;

    const uart_port_t port = static_cast<uart_port_t>(GW_UART_PORT);
    esp_err_t e = uart_driver_install(port, kRxRing, kTxRing, 0, nullptr, 0);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(e));
        return false;
    }
    e = uart_param_config(port, &u);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config: %s", esp_err_to_name(e));
        uart_driver_delete(port);
        return false;
    }
    e = uart_set_pin(port, c.serTx, c.serRx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin: %s", esp_err_to_name(e));
        uart_driver_delete(port);
        return false;
    }

    g_run = true;
    // Core 1 with the pump: the two hand bytes to each other and core 0
    // belongs to the radio.
    if (xTaskCreatePinnedToCore(task, "gw-wire", 3072, nullptr, 5, &g_task, 1)
        != pdPASS) {
        ESP_LOGE(TAG, "cannot start the serial reader");
        g_run = false;
        uart_driver_delete(port);
        return false;
    }

    g_up = true;
    static const char* kFmt[] = { "8N1", "7E1", "7N1" };
    ESP_LOGI(TAG, "terminal server on UART%d, TX %d RX %d, %u %s, idle %u min",
             GW_UART_PORT, c.serTx, c.serRx, static_cast<unsigned>(c.serBaud),
             kFmt[c.serFmt <= settings::FMT_7N1 ? c.serFmt : 0], c.serIdle);
    return true;
}

void stop() {
    if (!g_up) return;
    g_run = false;
    line::close(g_ln, "this gateway's serial line was switched off");
    g_ln.clear();
    // The task notices g_run within its 50 ms read timeout and deletes
    // itself; the driver goes only once it has, or the read would be
    // against a deleted driver.
    for (int i = 0; i < 20 && g_task; ++i) vTaskDelay(pdMS_TO_TICKS(10));
    uart_driver_delete(static_cast<uart_port_t>(GW_UART_PORT));
    g_up = false;
}

bool up()   { return g_up; }
bool busy() { return line::live(g_ln); }

}  // namespace uartsrv

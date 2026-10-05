// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/settings.cpp
// Module:       Settings in NVS, and the one validator
//
// Purpose:      settings.h says what and why. This reads and writes NVS and
//               judges every value that arrives.
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
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "board.h"
#include "repeat.h"
#include "settings_rules.h"

namespace settings {
namespace {

const char* TAG = "gw-cfg";
const char* kNs = "gw";

Cfg  g_cfg;
bool g_open    = false;
bool g_restart = false;
char g_name[24] = {0};

// -------------------------------------------------------------------------
//  Small helpers
// -------------------------------------------------------------------------

void hash(const uint8_t* salt, const char* plain, uint8_t out[32]) {
    // Salted SHA-256, once. Deliberately simple rather than PBKDF2: the
    // only way to reach this hash is to hold the device and read its flash,
    // and anybody holding the device can reflash it. Same reasoning the
    // core records for users.txt.
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, salt, 16);
    mbedtls_sha256_update(&c, reinterpret_cast<const uint8_t*>(plain), strlen(plain));
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
}

bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) d |= static_cast<uint8_t>(a[i] ^ b[i]);
    return d == 0;
}

// -------------------------------------------------------------------------
//  NVS
// -------------------------------------------------------------------------
void getStr(nvs_handle_t h, const char* k, char* dst, size_t cap) {
    size_t n = cap;
    if (nvs_get_str(h, k, dst, &n) != ESP_OK) return;
    dst[cap - 1] = 0;
}

void getU8(nvs_handle_t h, const char* k, uint8_t& v) { nvs_get_u8(h, k, &v); }
void getU16(nvs_handle_t h, const char* k, uint16_t& v) { nvs_get_u16(h, k, &v); }

void getBool(nvs_handle_t h, const char* k, bool& v) {
    uint8_t b = v ? 1 : 0;
    if (nvs_get_u8(h, k, &b) == ESP_OK) v = (b != 0);
}

void getI8(nvs_handle_t h, const char* k, int8_t& v) {
    int8_t b = v;
    if (nvs_get_i8(h, k, &b) == ESP_OK) v = b;
}

// NVS keys are at most 15 characters; every key below is inside that, and
// the board slots are b0name .. b4note for the same reason.
void boardKey(char* out, size_t n, uint8_t i, const char* what) {
    snprintf(out, n, "b%u%s", static_cast<unsigned>(i), what);
}

}  // namespace

// -------------------------------------------------------------------------
bool begin() {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs: %s; running on defaults, nothing will be kept", esp_err_to_name(e));
        return false;
    }

    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed; running on defaults");
        return false;
    }
    g_open = true;

    getBool(h, "ap", g_cfg.ap);
    getBool(h, "termsrv", g_cfg.termsrv);
    getBool(h, "repeat", g_cfg.repeat);

    getStr(h, "ap_ssid", g_cfg.apSsid, sizeof g_cfg.apSsid);
    getStr(h, "ap_pass", g_cfg.apPass, sizeof g_cfg.apPass);
    getStr(h, "ap_addr", g_cfg.apAddr, sizeof g_cfg.apAddr);
    getU8(h, "ap_chan", g_cfg.apChan);
    getU8(h, "ap_max", g_cfg.apMax);
    getU16(h, "ap_beacon", g_cfg.apBeacon);
    getU8(h, "ap_dtim", g_cfg.apDtim);
    getU8(h, "ap_probe", g_cfg.apProbe);

    getStr(h, "net_ssid", g_cfg.netSsid, sizeof g_cfg.netSsid);
    getStr(h, "net_pass", g_cfg.netPass, sizeof g_cfg.netPass);

    getI8(h, "ser_tx", g_cfg.serTx);
    getI8(h, "ser_rx", g_cfg.serRx);
    getI8(h, "ser_btn", g_cfg.serBtn);
    uint32_t baud = g_cfg.serBaud;
    if (nvs_get_u32(h, "ser_baud", &baud) == ESP_OK) g_cfg.serBaud = baud;
    getU8(h, "ser_fmt", g_cfg.serFmt);
    getU8(h, "ser_flow", g_cfg.serFlow);
    getU8(h, "ser_hang", g_cfg.serHang);
    getU8(h, "ser_idle", g_cfg.serIdle);
    getU8(h, "ser_board", g_cfg.serBoard);

    getU8(h, "rep_hops", g_cfg.repHops);
    getU8(h, "rep_up", g_cfg.repUp);

    for (uint8_t i = 0; i < kBoards; ++i) {
        char k[16];
        boardKey(k, sizeof k, i, "name");
        getStr(h, k, g_cfg.boards[i].name, sizeof g_cfg.boards[i].name);
        boardKey(k, sizeof k, i, "host");
        getStr(h, k, g_cfg.boards[i].host, sizeof g_cfg.boards[i].host);
        boardKey(k, sizeof k, i, "port");
        getU16(h, k, g_cfg.boards[i].port);
        boardKey(k, sizeof k, i, "note");
        getStr(h, k, g_cfg.boards[i].note, sizeof g_cfg.boards[i].note);
        boardKey(k, sizeof k, i, "from");
        getU8(h, k, g_cfg.boards[i].from);
    }

    size_t n = sizeof g_cfg.salt;
    if (nvs_get_blob(h, "salt", g_cfg.salt, &n) != ESP_OK || n != sizeof g_cfg.salt) {
        esp_fill_random(g_cfg.salt, sizeof g_cfg.salt);
        nvs_set_blob(h, "salt", g_cfg.salt, sizeof g_cfg.salt);
        nvs_commit(h);
    }
    n = sizeof g_cfg.pwHash;
    g_cfg.pwSet = (nvs_get_blob(h, "pwhash", g_cfg.pwHash, &n) == ESP_OK &&
                   n == sizeof g_cfg.pwHash);

    getStr(h, "name", g_name, sizeof g_name);
    nvs_close(h);
    return true;
}

const Cfg& get() { return g_cfg; }

// One spare copy, static because the one caller is the HTTP server's single
// task and a Cfg is about a kilobyte, which is more than its stack should
// hold for this.
namespace {
Cfg  g_saved;
char g_savedName[24];
bool g_savedRestart = false;
bool g_haveSnapshot = false;
}  // namespace

void snapshot() {
    g_saved        = g_cfg;
    memcpy(g_savedName, g_name, sizeof g_savedName);
    g_savedRestart = g_restart;
    g_haveSnapshot = true;
}

void rollback() {
    if (!g_haveSnapshot) return;
    g_cfg    = g_saved;
    memcpy(g_name, g_savedName, sizeof g_name);
    g_restart = g_savedRestart;
    g_haveSnapshot = false;
    // The copy held a Wi-Fi key and is no longer needed.
    memset(&g_saved, 0, sizeof g_saved);
}

void commit() {
    g_haveSnapshot = false;
    memset(&g_saved, 0, sizeof g_saved);
}

bool restartPending() { return g_restart; }

void name(char* out, size_t n) {
    if (g_name[0]) { copyStr(out, n, g_name); return; }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, n, "gateway-%02x%02x", mac[4], mac[5]);
}

const Board* board(uint8_t oneBased) {
    if (oneBased < 1 || oneBased > kBoards) return nullptr;
    const Board& b = g_cfg.boards[oneBased - 1];
    return b.used() ? &b : nullptr;
}

// -------------------------------------------------------------------------
//  The validator
// -------------------------------------------------------------------------
// The validator itself is settings_rules.h's, so that the host tests drive
// exactly the code that ships. This is the only caller, and the state it
// passes in is this file's.
const char* set(const char* key, const char* value) {
    return set(g_cfg, g_name, sizeof g_name, g_restart, key, value);
}

// -------------------------------------------------------------------------
bool save() {
    if (!g_open) return false;
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;

    nvs_set_u8(h, "ap", g_cfg.ap ? 1 : 0);
    nvs_set_u8(h, "termsrv", g_cfg.termsrv ? 1 : 0);
    nvs_set_u8(h, "repeat", g_cfg.repeat ? 1 : 0);

    nvs_set_str(h, "ap_ssid", g_cfg.apSsid);
    nvs_set_str(h, "ap_pass", g_cfg.apPass);
    nvs_set_str(h, "ap_addr", g_cfg.apAddr);
    nvs_set_u8(h, "ap_chan", g_cfg.apChan);
    nvs_set_u8(h, "ap_max", g_cfg.apMax);
    nvs_set_u16(h, "ap_beacon", g_cfg.apBeacon);
    nvs_set_u8(h, "ap_dtim", g_cfg.apDtim);
    nvs_set_u8(h, "ap_probe", g_cfg.apProbe);

    nvs_set_str(h, "net_ssid", g_cfg.netSsid);
    nvs_set_str(h, "net_pass", g_cfg.netPass);

    nvs_set_i8(h, "ser_tx", g_cfg.serTx);
    nvs_set_i8(h, "ser_rx", g_cfg.serRx);
    nvs_set_i8(h, "ser_btn", g_cfg.serBtn);
    nvs_set_u32(h, "ser_baud", g_cfg.serBaud);
    nvs_set_u8(h, "ser_fmt", g_cfg.serFmt);
    nvs_set_u8(h, "ser_flow", g_cfg.serFlow);
    nvs_set_u8(h, "ser_hang", g_cfg.serHang);
    nvs_set_u8(h, "ser_idle", g_cfg.serIdle);
    nvs_set_u8(h, "ser_board", g_cfg.serBoard);

    nvs_set_u8(h, "rep_hops", g_cfg.repHops);
    nvs_set_u8(h, "rep_up", g_cfg.repUp);

    for (uint8_t i = 0; i < kBoards; ++i) {
        char k[16];
        boardKey(k, sizeof k, i, "name"); nvs_set_str(h, k, g_cfg.boards[i].name);
        boardKey(k, sizeof k, i, "host"); nvs_set_str(h, k, g_cfg.boards[i].host);
        boardKey(k, sizeof k, i, "port"); nvs_set_u16(h, k, g_cfg.boards[i].port);
        boardKey(k, sizeof k, i, "note"); nvs_set_str(h, k, g_cfg.boards[i].note);
        boardKey(k, sizeof k, i, "from"); nvs_set_u8(h, k, g_cfg.boards[i].from);
    }

    if (g_name[0]) nvs_set_str(h, "name", g_name);
    if (g_cfg.pwSet) nvs_set_blob(h, "pwhash", g_cfg.pwHash, sizeof g_cfg.pwHash);
    else             nvs_erase_key(h, "pwhash");

    const esp_err_t e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs commit: %s", esp_err_to_name(e));
        return false;
    }
    return true;
}

// -------------------------------------------------------------------------
bool passwordSet() { return g_cfg.pwSet; }

bool passwordOk(const char* candidate) {
    // An empty candidate never matches, whatever is stored, so a blank
    // field cannot be a way in. The core's own rule.
    if (!g_cfg.pwSet || !candidate || !*candidate) return false;
    uint8_t h[32];
    hash(g_cfg.salt, candidate, h);
    return ctEqual(h, g_cfg.pwHash, sizeof h);
}

const char* passwordSetTo(const char* plain) {
    if (!plain || !*plain) {
        // Clearing it puts the setup page back to its first-boot state,
        // which is a real thing to want and is said out loud on the page
        // rather than silently leaving it open.
        g_cfg.pwSet = false;
        memset(g_cfg.pwHash, 0, sizeof g_cfg.pwHash);
        return nullptr;
    }
    if (strlen(plain) < 6) return "at least 6 characters";
    if (strlen(plain) > 64) return "at most 64 characters";
    hash(g_cfg.salt, plain, g_cfg.pwHash);
    g_cfg.pwSet = true;
    return nullptr;
}

}  // namespace settings

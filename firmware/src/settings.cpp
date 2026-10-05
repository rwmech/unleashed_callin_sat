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

// Copies at most n-1 bytes and always terminates. Values here are ASCII
// settings, so there is no UTF-8 character to back off from the way the
// board's own board_name has to.
void copyStr(char* dst, size_t n, const char* src) {
    if (n == 0) return;
    size_t i = 0;
    for (; src[i] && i + 1 < n; ++i) dst[i] = src[i];
    dst[i] = 0;
}

bool isYes(const char* v) {
    return v[0] == 'y' || v[0] == 'Y' || v[0] == '1' ||
           v[0] == 'o' || v[0] == 'O' || v[0] == 't' || v[0] == 'T';
}

// A number, strictly. strtol stops at the first odd character and reports
// success for "80abc", which here would mean a sysop's typo silently
// becoming a different port. Refuse anything that is not all digits.
bool allDigits(const char* v, long& out) {
    if (!*v) return false;
    long n = 0;
    for (const char* p = v; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        n = n * 10 + (*p - '0');
        if (n > 1000000L) return false;        // nothing here is that big
    }
    out = n;
    return true;
}

// A dotted IPv4 and nothing else. inet_addr would take "10" and "0x0a" and
// a sysop reading the field back would not recognise what they typed.
bool isIpv4(const char* v) {
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
bool printableOnly(const char* v) {
    for (const char* p = v; *p; ++p) {
        if (static_cast<unsigned char>(*p) < 0x20 ||
            static_cast<unsigned char>(*p) > 0x7E) return false;
    }
    return true;
}

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
const char* set(const char* key, const char* value) {
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
        if (!allDigits(value, n) || n < 1 || n > 3) return "1 to 3";
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
        if (strcmp(value, g_cfg.netSsid) != 0 && value[0] && !g_cfg.netPass[0]) {
            // Allowed, but only deliberately: an open network is a real
            // thing. Saying it is what stops it being a surprise.
            ESP_LOGW(TAG, "net_ssid set with no password: joining as an open network");
        }
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
        if (!board(static_cast<uint8_t>(n))) return "that board slot is empty";
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
        copyStr(g_name, sizeof g_name, value);
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

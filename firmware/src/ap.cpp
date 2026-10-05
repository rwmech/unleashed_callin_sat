// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/ap.cpp
// Module:       The radio
//
// Purpose:      ap.h says what and why. This brings it up.
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
#include "ap.h"

#include <cstdio>
#include <cstring>

#include "dhcpserver/dhcpserver.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"

#include "settings.h"

namespace ap {
namespace {

const char* TAG = "gw-radio";

esp_netif_t* g_ap  = nullptr;
esp_netif_t* g_sta = nullptr;

bool  g_apUp   = false;
bool  g_secure = false;      // the access point has a password: WPA2
bool  g_staUp  = false;
char  g_ssid[33] = {0};
char  g_addr[16] = {0};
char  g_staAddr[16] = {0};
uint8_t g_phones = 0;

// Backing off a failed join, so a gateway in a field with a typo in its
// network name is not a radio transmitting a scan every two seconds for a
// week. First drop at once, then doubling to five minutes.
uint32_t g_redialAt  = 0;
uint32_t g_redialGap = 0;

uint32_t ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

uint32_t since(uint32_t now, uint32_t at) {
    const uint32_t d = now - at;
    return (d > 0xFFFF0000u) ? 0u : d;
}

// Into one of the driver's fixed fields, terminated, truncating if it must.
//
// memcpy and not snprintf: GCC 13 refuses snprintf("%s") into a buffer that
// the source could exactly fill, as -Wformat-truncation, and it is right to
// — the truncation would be silent. The core hit the same wall and took the
// same answer.
void copyField(uint8_t* dst, size_t cap, const char* src) {
    if (!cap) return;
    size_t n = 0;
    while (src[n] && n + 1 < cap) ++n;
    memcpy(dst, src, n);
    dst[n] = 0;
}

void onWifi(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_AP_STACONNECTED: {
                auto* e = static_cast<wifi_event_ap_staconnected_t*>(data);
                if (g_phones < 0xFF) ++g_phones;
                ESP_LOGI(TAG, "a phone joined: %02x:%02x:%02x:%02x:%02x:%02x (%u on)",
                         e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                         g_phones);
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                if (g_phones) --g_phones;
                // A caller who walked away still holds a line until their
                // WebSocket times out. Telling the line at once is phase 4's
                // clean CLOSE; on a ten-line board it is the difference
                // between ten lines and three, and it needs the association
                // tied to the socket, which this phase does not have.
                ESP_LOGI(TAG, "a phone left (%u on)", g_phones);
                break;
            }
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                auto* e = static_cast<wifi_event_sta_disconnected_t*>(data);
                g_staUp = false;
                g_staAddr[0] = 0;
                g_redialGap = g_redialGap ? (g_redialGap * 2) : 0;
                if (g_redialGap > 300000u) g_redialGap = 300000u;
                g_redialAt = ms();
                ESP_LOGW(TAG, "left %s, reason %d; trying again in %u s",
                         settings::get().netSsid, e->reason,
                         static_cast<unsigned>(g_redialGap / 1000));
                break;
            }
            default:
                break;
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* e = static_cast<ip_event_got_ip_t*>(data);
        snprintf(g_staAddr, sizeof g_staAddr, IPSTR, IP2STR(&e->ip_info.ip));
        g_staUp = true;
        g_redialGap = 0;

        // Power save OFF, re-asserted here rather than once after
        // esp_wifi_start(). The core paid for this twice: starting the
        // station raises an event whose handler connects at once, so a call
        // beside esp_wifi_start races association; and a reconnect that
        // keeps its lease never raises GOT_IP's sibling, so a one-off call
        // is undone by the first reconnect. The return is checked, because
        // the first version of this in the core was not and silently did
        // nothing for two releases.
        //
        // It matters here for the same reason it matters on the board: a
        // dozing radio buffers a keystroke's answer for up to a beacon
        // period, and a caller reads that as the board being slow.
        const esp_err_t r = esp_wifi_set_ps(WIFI_PS_NONE);
        if (r != ESP_OK) ESP_LOGW(TAG, "set_ps: %s", esp_err_to_name(r));

        wifi_ap_record_t ap_info = {};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI(TAG, "joined %s on channel %u, %d dBm, address %s",
                     reinterpret_cast<const char*>(ap_info.ssid), ap_info.primary,
                     ap_info.rssi, g_staAddr);
        } else {
            ESP_LOGI(TAG, "joined, address %s", g_staAddr);
        }
    }
}

// The access point's own address, netmask and the range its DHCP server
// hands out, plus the one option that makes the portal work at all: every
// phone is told that THIS BOX is its name server.
bool setApAddress(const char* dotted) {
    esp_netif_ip_info_t ip = {};
    ip.ip.addr      = ipaddr_addr(dotted);
    ip.gw.addr      = ip.ip.addr;
    ip.netmask.addr = ipaddr_addr("255.255.255.0");
    if (ip.ip.addr == IPADDR_NONE) {
        ESP_LOGE(TAG, "'%s' is not an address", dotted);
        return false;
    }

    // The server has to be stopped to be reconfigured, and it is started
    // again below whether or not each step worked, so a gateway with one
    // bad setting still hands out leases.
    esp_netif_dhcps_stop(g_ap);

    esp_err_t e = esp_netif_set_ip_info(g_ap, &ip);
    if (e != ESP_OK) ESP_LOGE(TAG, "set_ip_info: %s", esp_err_to_name(e));

    dhcps_offer_t offer = OFFER_DNS;
    e = esp_netif_dhcps_option(g_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                               &offer, sizeof offer);
    if (e != ESP_OK) ESP_LOGE(TAG, "offer DNS: %s", esp_err_to_name(e));

    esp_netif_dns_info_t dns = {};
    dns.ip.type         = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip.ip.addr;
    e = esp_netif_set_dns_info(g_ap, ESP_NETIF_DNS_MAIN, &dns);
    if (e != ESP_OK) ESP_LOGE(TAG, "set_dns_info: %s", esp_err_to_name(e));

    e = esp_netif_dhcps_start(g_ap);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "dhcps_start: %s", esp_err_to_name(e));
        return false;
    }

    snprintf(g_addr, sizeof g_addr, "%s", dotted);
    return true;
}

}  // namespace

// -------------------------------------------------------------------------
bool begin() {
    const settings::Cfg& c = settings::get();
    const bool wantAp  = c.ap;
    const bool wantSta = (c.netSsid[0] != 0);

    if (!wantAp && !wantSta) {
        // Nothing to bring up. Said once, plainly: a box between its flash
        // and its setup is in exactly this state, and the only way in is
        // the console, so refusing or crashing would be refusing the way in.
        ESP_LOGW(TAG, "no access point and no network to join: the radio stays off");
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (wantAp)  g_ap  = esp_netif_create_default_wifi_ap();
    if (wantSta) g_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &onWifi, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &onWifi, nullptr, nullptr));

    // Nothing is kept in flash: the settings are ours in NVS, and leaving
    // the driver its own copy means a changed setting can be shadowed by a
    // stale one nobody can see.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    const wifi_mode_t mode = (wantAp && wantSta) ? WIFI_MODE_APSTA
                           : (wantAp ? WIFI_MODE_AP : WIFI_MODE_STA);
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));

    if (wantAp) {
        if (c.apSsid[0]) snprintf(g_ssid, sizeof g_ssid, "%s", c.apSsid);
        else             settings::name(g_ssid, sizeof g_ssid);

        wifi_config_t wc = {};
        // ssid_len is set, so the field need not be terminated; copyField
        // terminates anyway and the length follows what it really copied.
        copyField(wc.ap.ssid, sizeof wc.ap.ssid, g_ssid);
        // Open or WPA2, by whether there is a password. Two quite different
        // deployments out of one setting:
        //
        //   blank  an open network. The fairground: nothing to type, the
        //          portal is the front door, and the hop from a phone to
        //          this box is readable by anyone in range. Said plainly
        //          on the portal and in the board's own connection line.
        //   set    WPA2-PSK, so the link is CCMP-encrypted and a passer-by
        //          reads nothing. The portal is plain HTTP over it and
        //          needs nothing more, exactly as a home router's own
        //          admin page is: HTTPS would add nothing here.
        //
        // settings.cpp refuses a password under WPA2's own 8-character
        // minimum rather than letting one quietly open the network.
        g_secure = (c.apPass[0] != 0);
        if (g_secure) {
            copyField(wc.ap.password, sizeof wc.ap.password, c.apPass);
            wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
            // Capable but not required: PMF is WPA3's, and requiring it on
            // a WPA2 network turns away a phone that cannot do it, which
            // is the opposite of what a front door is for.
            wc.ap.pmf_cfg.capable  = true;
            wc.ap.pmf_cfg.required = false;
        } else {
            wc.ap.authmode = WIFI_AUTH_OPEN;
        }
        wc.ap.ssid_len       = static_cast<uint8_t>(
            strnlen(reinterpret_cast<const char*>(wc.ap.ssid), sizeof wc.ap.ssid));
        wc.ap.max_connection = c.apMax;
        wc.ap.beacon_interval = c.apBeacon;
        // 1 is the whole reason this is a setting. A phone in power save
        // holds a sleeping station's traffic until the next DTIM beacon, so
        // at the IDF's default of 2 with a 100 TU beacon that is about
        // 205 ms a keystroke, which is the largest single term in the whole
        // chain. Phase 2 measures 1 against 2 on a real phone.
        wc.ap.dtim_period    = c.apDtim;
        // If the access point ever has to follow a board onto another
        // channel, tell the phones to come rather than dropping them.
        wc.ap.csa_count      = 3;
        // 0 means "pick one" nowhere in the driver: it has to be a real
        // channel. Blank in the settings means "follow the board", which in
        // phase 1 is settled by the board joining us, so 1 is the honest
        // default and the portal shows what it settled on.
        wc.ap.channel        = c.apChan ? c.apChan : 1;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    }

    if (wantSta) {
        wifi_config_t wc = {};
        copyField(wc.sta.ssid, sizeof wc.sta.ssid, c.netSsid);
        copyField(wc.sta.password, sizeof wc.sta.password, c.netPass);
        wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    // Off on both interfaces, and again at every GOT_IP. See onWifi().
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (wantAp) {
        g_apUp = setApAddress(c.apAddr);
        uint8_t ch = 0;
        wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&ch, &sec);
        ESP_LOGI(TAG, "access point \"%s\" %s, %s, channel %u, up to %u phones, "
                      "beacon %u TU, DTIM %u",
                 g_ssid, g_secure ? "WPA2" : "OPEN (anyone nearby can read it)",
                 g_addr, ch, c.apMax, c.apBeacon, c.apDtim);
        if (wantSta) {
            ESP_LOGI(TAG, "a station as well, so the router will decide the channel "
                          "and the phones follow it");
        }
    }
    return true;
}

bool        apUp()    { return g_apUp; }
bool        secure()  { return g_secure; }
const char* ssid()    { return g_ssid; }
const char* addr()    { return g_addr; }
uint8_t     phones()  { return g_phones; }
bool        staUp()   { return g_staUp; }
const char* staAddr() { return g_staAddr; }
const char* staSsid() { return settings::get().netSsid; }

uint8_t channel() {
    uint8_t ch = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&ch, &sec) != ESP_OK) return 0;
    return ch;
}

// Called once a second from main, so the redial's own clock lives on one
// task and the event handler only ever stamps it.
void tick() {
    if (!settings::get().netSsid[0] || g_staUp) return;
    if (!g_redialAt) return;
    if (since(ms(), g_redialAt) < g_redialGap) return;
    g_redialAt = 0;
    esp_wifi_connect();
}

}  // namespace ap

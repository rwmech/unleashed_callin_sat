// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/web.cpp
// Module:       The portal, the terminal, the WebSocket source, the setup
//
// Purpose:      web.h says what and why. This is the server.
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
#include "web.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "ap.h"
#include "board.h"
#include "dns.h"
#include "elapsed.h"
#include "line.h"
#include "settings.h"
#include "version.h"
#include "web_assets.h"

namespace web {
namespace {

const char* TAG = "gw-web";

// ---------------------------------------------------------------------------
//  The socket budget, asserted rather than assumed.
//
//  lwIP gives sixteen at IDF 5.3.1 and a value out of range is discarded
//  rather than clamped, so the number is checked against what is actually
//  built. Who takes one:
//
//    the DNS redirector                                  1
//    esp_http_server's own working                       3
//    one per open HTTP or WebSocket client   kMaxClients
//    one per caller line's uplink             line::kLines
//
//  A spare is left over on purpose: a board that runs out loses a caller
//  at accept() with nothing said, which is the worst way to find out.
// ---------------------------------------------------------------------------
constexpr int kMaxClients = 7;        // esp_http_server's own default
constexpr int kHttpdOwn   = 3;        // esp_http_server.h:187 says so
constexpr int kDnsSockets = 1;
static_assert(kDnsSockets + kHttpdOwn + kMaxClients + line::kLines
              < CONFIG_LWIP_MAX_SOCKETS,
              "the gateway's sockets do not fit lwIP's budget: see the table above");

// The httpd task's stack comes from the heap, not from static DRAM, so it
// is not part of the figure a WROOM is judged on; it IS part of the heap
// figure phase 2 reads. The IDF's default is 4,096 and one real project of
// this shape raised it to 10,240; the handlers here render a page from a
// few hundred bytes of stack, so 6,144 is the compromise and phase 2's
// stack high-water reading is what settles it.
constexpr uint32_t kHttpdStack = 6144;

// The largest WebSocket message taken from a caller in one frame. A
// keystroke is one byte and the longest realistic message is somebody
// pasting; esp_http_server does not reassemble fragments (its own header
// says so), so a browser that fragments a bigger paste loses the tail and
// the console says it did rather than the terminal silently gapping.
constexpr size_t kWsRecvMax = 1536;

// What one caller's line can have in flight towards their browser. One
// frame at a time, posted to the server's own task, which is what keeps
// every write to a socket on one task and so unable to interleave with the
// server's own ping and close frames.
constexpr size_t kWsOutMax = 1024;

httpd_handle_t g_hd = nullptr;

// ---------------------------------------------------------------------------
//  The WebSocket as a caller source
// ---------------------------------------------------------------------------
struct WsCtx {
    httpd_handle_t hd = nullptr;
    int            fd = -1;
    // Which caller has this slot, counted up at every claim. The same
    // problem line::Handle solves, for the same reason: work queued onto
    // the server's task outlives the caller it was queued for, and a slot
    // freed in between belongs to somebody else by the time it runs.
    uint32_t       gen = 0;
    line::Handle   ln;
    uint8_t        out[kWsOutMax] = {0};
    size_t         outLen = 0;
    std::atomic<bool> busy{false};    // out[] is queued; do not touch it
    std::atomic<bool> gone{false};    // the socket closed
};

WsCtx g_ws[line::kLines];

WsCtx* ctxForFd(int fd) {
    for (WsCtx& c : g_ws) {
        if (c.fd == fd && !c.gone.load(std::memory_order_acquire)) return &c;
    }
    return nullptr;
}

uint32_t g_wsGen = 0;

// A slot is free only when its socket has gone AND nothing of its is still
// queued on the server's task. Without the `busy` half, a sendWork already
// in the queue would run against the slot after a new caller took it, and
// send that new caller up to a kilobyte of the previous one's screen - and
// clear the one-frame-in-flight flag under them while it was at it.
WsCtx* freeCtx() {
    for (WsCtx& c : g_ws) {
        if (c.fd < 0 && !c.busy.load(std::memory_order_acquire)) return &c;
    }
    return nullptr;
}

// Runs on the server's own task, which is the point: every frame this
// gateway sends a browser is sent from one task, so it cannot interleave
// with the ping or close frames the server sends itself.
void sendWork(void* arg) {
    auto* c = static_cast<WsCtx*>(arg);
    if (!c->gone.load(std::memory_order_acquire)) {
        httpd_ws_frame_t f = {};
        f.type    = HTTPD_WS_TYPE_BINARY;
        f.payload = c->out;
        f.len     = c->outLen;
        f.final   = true;
        const esp_err_t e = httpd_ws_send_frame_async(c->hd, c->fd, &f);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "ws send: %s", esp_err_to_name(e));
            c->gone.store(true, std::memory_order_release);
            line::closeFromSource(c->ln, "the browser stopped listening");
        }
    }
    c->busy.store(false, std::memory_order_release);
}

struct CloseWork {
    httpd_handle_t hd;
    int            fd;
    uint8_t        slot;     // which WsCtx queued it
    uint32_t       gen;      // and for which caller
    char           why[40];
};
CloseWork g_closeWork[line::kLines];

void closeWork(void* arg) {
    auto* w = static_cast<CloseWork*>(arg);
    // Three guards, and each one is a socket this would otherwise touch
    // that is not the one it was queued for:
    //
    //  - fd < 0: this item already ran, or was superseded by a later close
    //    for the same slot;
    //  - the slot has been claimed again, so this fd number may have been
    //    closed and handed by lwIP to a brand new caller. Sending them the
    //    PREVIOUS caller's goodbye and closing their socket is the bug;
    //  - and the server's own check that the fd is still a WebSocket.
    if (w->fd < 0) return;
    if (g_ws[w->slot].gen != w->gen) { w->fd = -1; return; }
    if (httpd_ws_get_fd_info(w->hd, w->fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        w->fd = -1;
        return;
    }
    // Why it ended, as a text frame, so the page can show it rather than
    // the caller seeing a terminal that simply stopped. A failure here is
    // not worth reporting: the socket is going either way.
    if (w->why[0]) {
        char msg[48];
        const int n = snprintf(msg, sizeof msg, "x %s", w->why);
        httpd_ws_frame_t f = {};
        f.type    = HTTPD_WS_TYPE_TEXT;
        f.payload = reinterpret_cast<uint8_t*>(msg);
        f.len     = (n > 0) ? static_cast<size_t>(n) : 0;
        f.final   = true;
        httpd_ws_send_frame_async(w->hd, w->fd, &f);
    }
    httpd_sess_trigger_close(w->hd, w->fd);
    w->fd = -1;
}

size_t wsRoom(void* ctx) {
    auto* c = static_cast<WsCtx*>(ctx);
    if (c->gone.load(std::memory_order_acquire)) return 0;
    if (c->busy.load(std::memory_order_acquire)) return 0;
    return sizeof c->out;
}

bool wsSend(void* ctx, const uint8_t* p, size_t n) {
    auto* c = static_cast<WsCtx*>(ctx);
    if (c->gone.load(std::memory_order_acquire)) return true;   // swallowed, not failed
    if (n > sizeof c->out) return false;
    bool was = false;
    if (!c->busy.compare_exchange_strong(was, true, std::memory_order_acq_rel)) {
        return false;                                            // still sending the last one
    }
    memcpy(c->out, p, n);
    c->outLen = n;
    if (httpd_queue_work(c->hd, sendWork, c) != ESP_OK) {
        // The server's work queue is full, which is backpressure of its
        // own. Give the buffer back and let the pump offer it again.
        c->busy.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void wsClose(void* ctx, const char* why) {
    auto* c = static_cast<WsCtx*>(ctx);
    if (c->gone.exchange(true, std::memory_order_acq_rel)) return;
    const size_t i = static_cast<size_t>(c - g_ws);
    CloseWork& w = g_closeWork[i];
    w.hd   = c->hd;
    w.fd   = c->fd;
    w.slot = static_cast<uint8_t>(i);
    w.gen  = c->gen;
    snprintf(w.why, sizeof w.why, "%s", why ? why : "");
    if (httpd_queue_work(c->hd, closeWork, &w) != ESP_OK) {
        // Nothing can be sent and the socket cannot be closed from here,
        // because only the server's task may touch it. The server's own
        // keepalive reaps it, which is why keep_alive_enable is on.
        ESP_LOGW(TAG, "could not queue the close for fd %d", c->fd);
    }
    c->fd = -1;
    c->ln.clear();
}

const line::SourceOps kWsOps = { wsRoom, wsSend, wsClose };

// ---------------------------------------------------------------------------
//  Small helpers
// ---------------------------------------------------------------------------
// ms() and since() are when::'s, in elapsed.h: one rule in one place,
// because three copies in three files is the drift the core's own
// plat::since exists to prevent.
using when::ms;
using when::since;

// Everything a sysop typed into a setting reaches a page, and
// printableOnly() allows < > & and ", so every one of them is escaped
// here. A board called `<script>` is a sysop's own foot, but it is also
// every OTHER caller's browser, which is why this is not optional.
void esc(httpd_req_t* req, const char* s) {
    char buf[96];
    size_t w = 0;
    for (const char* p = s; *p; ++p) {
        const char* rep = nullptr;
        switch (*p) {
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '&': rep = "&amp;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default: break;
        }
        const size_t need = rep ? strlen(rep) : 1;
        if (w + need >= sizeof buf) {
            httpd_resp_send_chunk(req, buf, w);
            w = 0;
        }
        if (rep) { memcpy(buf + w, rep, need); w += need; }
        else     { buf[w++] = *p; }
    }
    if (w) httpd_resp_send_chunk(req, buf, w);
}

void say(httpd_req_t* req, const char* s) {
    httpd_resp_send_chunk(req, s, HTTPD_RESP_USE_STRLEN);
}

void sayf(httpd_req_t* req, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void sayf(httpd_req_t* req, const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) httpd_resp_send_chunk(req, buf, (n < static_cast<int>(sizeof buf))
                                                 ? static_cast<size_t>(n) : sizeof buf - 1);
}

int queryNum(httpd_req_t* req, const char* key, int lo, int hi, int dflt) {
    char q[96];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return dflt;
    char v[12];
    if (httpd_query_key_value(q, key, v, sizeof v) != ESP_OK) return dflt;
    long n = 0;
    for (const char* p = v; *p; ++p) {
        if (*p < '0' || *p > '9') return dflt;
        n = n * 10 + (*p - '0');
        if (n > 100000) return dflt;
    }
    if (n < lo || n > hi) return dflt;
    return static_cast<int>(n);
}

// Which phone asked, as its DHCP address. Only used to slow down guesses
// at the setup password, and it is a weak key on purpose: a phone changes
// its address by rejoining. See locked() for why it is still worth having.
int peerIp(httpd_req_t* req) {
    const int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return 0;
    sockaddr_storage a = {};
    socklen_t n = sizeof a;
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&a), &n) != 0) return 0;
    if (a.ss_family != AF_INET) return 0;      // IPv6 is off in this build
    return static_cast<int>(reinterpret_cast<sockaddr_in*>(&a)->sin_addr.s_addr);
}

// ---------------------------------------------------------------------------
//  The static assets
// ---------------------------------------------------------------------------
// Does this request's PATH name this asset?
//
// req->uri holds the whole URI, query string and all: the IDF reads a
// query out of it as `r->uri + field_data[UF_QUERY].off`. The handler was
// still FOUND, because httpd_uri_match_simple is given the path's length
// separately - so a plain strcmp against req->uri here compiled, looked
// right, and 404'd every request that carried a query.
//
// That is the terminal: the portal links to /t?b=1, so tapping a board
// would 404, the 404 handler would redirect to the portal, and the caller
// would bounce between the two for ever with the whole access-point role
// dead. The one thing phase 1 exists to prove, broken by a comparison that
// reads as obviously correct.
bool pathIs(const httpd_req_t* req, const char* path) {
    const size_t n = strlen(path);
    if (strncmp(req->uri, path, n) != 0) return false;
    const char c = req->uri[n];
    return c == 0 || c == '?' || c == '#';
}

esp_err_t assetGet(httpd_req_t* req) {
    for (const WebAsset& a : kWebAssets) {
        if (!pathIs(req, a.path)) continue;
        httpd_resp_set_type(req, a.type);
        // Precompressed at build: there is no gzip in esp_http_server and
        // there will not be, so the header is set by hand.
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        // Nothing here changes without a reflash, so a phone that comes
        // back should not fetch 70 KB again. Not "immutable": a reflash
        // does change it, and an hour is short enough to be forgiven.
        httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
        return httpd_resp_send(req, reinterpret_cast<const char*>(a.gz), a.gzLen);
    }
    httpd_resp_send_404(req);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
//  The portal
// ---------------------------------------------------------------------------
void portalBody(httpd_req_t* req) {
    char nm[24];
    settings::name(nm, sizeof nm);

    say(req, "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<title>");
    esc(req, nm);
    say(req, "</title><link rel=\"stylesheet\" href=\"/s.css\"></head><body>"
             "<div class=\"wrap\"><h1><span class=\"mu\">\xC2\xB5</span>nleashed</h1>"
             "<p class=\"sub\">");
    esc(req, nm);
    sayf(req, " &middot; " GW_WHAT " " GW_VERSION " (" GW_BOARD_TAG " "
              GW_BOARD_VERSION ")</p>");

    // The honest line, first, because it is what somebody needs before
    // they type anything rather than after. TWO MODES AND A SENTENCE EACH,
    // not one apology: whether this network has a password is what decides
    // the whole story, and saying "cannot be encrypted" when it can was an
    // overclaim that had to be corrected once already.
    if (ap::secure()) {
        // The default, and it is short because there is nothing to
        // apologise for: WPA2, so CCMP between the phone and this box.
        // Because THAT hop is encrypted, whether this page is HTTP or
        // HTTPS makes no difference to anybody and is not worth a
        // caller's attention.
        //
        // The copy says "this network", and it means this one hop, which
        // is what it can honestly claim. It does NOT say every hop: in
        // phase 1 the second hop is plain telnet, protected by whichever
        // Wi-Fi carries it rather than by anything this firmware does. An
        // earlier version of this comment claimed the link's own
        // AES-128-CCM here, which does not exist until phase 4 - the same
        // overclaim the README had to have corrected, two lines under a
        // note about overclaiming.
        say(req, "<div class=\"open\" style=\"border-left-color:#5fd38d\">"
                 "<strong>This network is encrypted.</strong> Nobody nearby can "
                 "read what you type or what you see. Its password is printed "
                 "rather than secret: it is there to keep passers-by off the air, "
                 "not to keep you out.</div>");
    } else {
        // The exception: somebody cleared the password on purpose. This is
        // the only place the certificate argument belongs, because it is
        // the only place anybody might reach for HTTPS to fix things.
        say(req, "<div class=\"open\"><strong>Whoever set this box up left its "
                 "Wi-Fi open, with no password.</strong> Anything you type here "
                 "can be read by anyone nearby with the right equipment, a "
                 "password included. HTTPS could not rescue it either: a page a "
                 "network pops up by itself cannot carry a certificate anybody "
                 "has signed. Say what you would say out loud, and use a password "
                 "you use nowhere else.</div>");
    }

    say(req, "<h2>Bulletin boards</h2>");

    uint8_t shown = 0;
    for (uint8_t i = 1; i <= settings::kBoards; ++i) {
        const settings::Board* b = settings::board(i);
        if (!b) continue;
        ++shown;
        const bool reachable = b->byIp();
        sayf(req, "<a class=\"board%s\" href=\"/t?b=%u\"><span><span class=\"nm\">",
             reachable ? "" : " down", i);
        esc(req, b->name);
        say(req, "</span>");
        if (b->note[0]) {
            say(req, "<br><span class=\"note\">");
            esc(req, b->note);
            say(req, "</span>");
        }
        say(req, "</span><span class=\"go\">");
        // An IP board is NOT probed, and that is a decision rather than
        // laziness: a periodic connect-and-close would appear in the
        // board's own caller log as a caller who never logged in, once an
        // interval, for ever. The caller log is the sysop's security
        // record. So a board is listed and a caller finds out one tap
        // later, which is a better trade than polluting it.
        say(req, reachable ? "Call &rsaquo;" : "no address set");
        say(req, "</span></a>");
    }
    if (!shown) {
        say(req, "<p class=\"none\">This gateway has no boards set up yet. "
                 "Whoever planted it can add one on its setup page.</p>");
    }

    const uint8_t busy = line::busy();
    sayf(req, "<h2>This gateway</h2><table class=\"kv\">"
              "<tr><td>Lines in use</td><td>%u of %u</td></tr>",
         busy, static_cast<unsigned>(line::kLines));
    sayf(req, "<tr><td>Phones on this network</td><td>%u</td></tr>", ap::phones());
    sayf(req, "<tr><td>Channel</td><td>%u</td></tr>", ap::channel());
    say(req, "</table>");

    // Each line, because the spec's own answer to "how is a fault at a
    // fairground debugged with no laptop" is that the portal says. It is
    // nearly free: the table machinery is already here for the board list.
    if (busy) {
        say(req, "<table class=\"kv\">");
        for (uint8_t i = 0; i < line::kLines; ++i) {
            const line::Line* l = line::at(i);
            if (!l) continue;
            const line::St st = l->st.load(std::memory_order_acquire);
            if (st == line::St::Idle) continue;
            const char* what = (st == line::St::Connecting) ? "calling"
                             : (st == line::St::Up) ? "on" : "closing";
            sayf(req, "<tr><td>Line %u</td><td>%s, %s, %ux%u, %u s</td></tr>",
                 i + 1, (l->src == line::Src::Wire) ? "wire" : "phone", what,
                 l->cols, l->rows,
                 static_cast<unsigned>((esp_timer_get_time() / 1000000) -
                                       (l->openedAt / 1000)));
        }
        say(req, "</table>");
    }

    if (busy >= line::kLines) {
        say(req, "<p class=\"bad\">Every line on this gateway is in use. "
                 "Try again in a moment, or use the address below with a "
                 "terminal program.</p>");
    }

    say(req, "<p class=\"fall\">If this page did not appear by itself, open your "
             "browser and go to <code>http://");
    esc(req, ap::addr());
    say(req, "</code> &mdash; type it exactly, with the <code>http://</code>, "
             "because a search will not reach this box. A caller with a real "
             "terminal program can dial a board above directly instead. "
             "Walking between two of these boxes drops your line: the board "
             "sees a clean hang-up and you log in again."
             "<br><br><a href=\"/setup\">Set this gateway up</a> &middot; "
             "<a href=\"/licence\">Licences</a></p>");
    say(req, "</div></body></html>");
    httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t portalGet(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    // Never cached: the board list and the line count are the whole point.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    portalBody(req);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
//  Every other URL: the operating systems' own probes
//
//  One handler, because every OS probe is a path this gateway does not
//  serve, so the 404 handler catches all of them at once:
//
//    Apple    captive.apple.com/hotspot-detect.html, wanting a page whose
//             title and body are the word Success
//    Android  /generate_204, wanting 204 No Content. Its NetworkMonitor
//             reads 3xx as a portal and takes the Location header as the
//             login URL, and reads a 200 with a body as an intercept
//    Windows  /connecttest.txt and the older /ncsi.txt, wanting the text
//             "Microsoft Connect Test". Its own FAQ names a 302 as the
//             portal case, and from Windows 11 the probe is always HTTP
//    Linux    NetworkManager's various, wanting a header or a body it set
//    Firefox  /canonical.html, its own
//
//  Two things it must NOT do, each of which breaks a different phone:
//  never answer 204, and never answer an empty redirect. iOS needs content
//  in the response to decide it is behind a portal; Espressif's own example
//  carries the same comment.
//
//  And one thing to know before anybody gives a gateway an uplink: Android
//  probes https://www.google.com/generate_204 in parallel, and if THAT
//  succeeds the network is marked as having internet and Android never
//  offers "Sign in to network" whatever this answers. A gateway routes
//  nothing, so it cannot happen here; it could if somebody added routing.
// ---------------------------------------------------------------------------
esp_err_t probeAll(httpd_req_t* req, httpd_err_code_t) {
    const settings::Cfg& c = settings::get();

    if (c.apProbe == settings::PROBE_PAGE) {
        // The Samsung and recent-Android workaround: 200 with the portal
        // itself rather than a redirect. Four or more years of reports
        // against arduino-esp32 say moving off 192.168.4.1 and answering
        // 200-with-a-body are the two things people report fixing it.
        // Secondary, unexplained, and free to switch, which is why it is a
        // setting and phase 2 tries both on a real phone.
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        portalBody(req);
        return ESP_OK;
    }

    char to[48];
    snprintf(to, sizeof to, "http://%s/", ap::addr());
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Location", to);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // The body is not decoration: iOS wants content before it decides it
    // is behind a portal, and a bare redirect is not enough.
    say(req, "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
             "<meta http-equiv=\"refresh\" content=\"0;url=/\"></head><body>"
             "<p>This network has a bulletin board on it. "
             "<a href=\"/\">Open the portal</a>.</p></body></html>");
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
//  The WebSocket: a caller's line
// ---------------------------------------------------------------------------
uint8_t g_wsBuf[kWsRecvMax];

esp_err_t wsHandler(httpd_req_t* req) {
    const int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
        // The handshake. The server has already answered it; this is where
        // the line is taken.
        //
        // THIS DEPENDS ON IDF 5.3.1 BEHAVIOUR THAT LATER IDF REMOVED, and
        // it is worth naming because nothing would warn. On the pinned
        // framework httpd_uri() answers the handshake and then falls
        // through to the URI's handler (httpd_uri.c:320-328), which is how
        // this block runs at all. From IDF 6.1.0 that path ends with
        // "If the request is websocket handshake, then do not call the
        // uri->handler" and returns. Under that framework this block
        // becomes dead code: no slot claimed, no line opened, no
        // connection line, and every keystroke dropped at the `if (!c)`
        // below. The browser would say "connected" and the terminal would
        // simply never do anything - no compile error, no log, no partial
        // symptom, and the whole access-point role gone.
        //
        // The portable shape is to claim the line lazily on the first data
        // frame instead. It is not done here because the handshake is also
        // where the query string lives (which board, and the window size),
        // and a frame does not carry it. A framework bump has to come back
        // to this.
        const int b = queryNum(req, "b", 1, settings::kBoards, 1);
        // The same bounds line::resized clamps to, so the two cannot
        // disagree about what a legal window is: the first version took
        // 20..240 here and clamped only the top end there, so a 15-column
        // glass was told to the board as 80 while "s 10 5" was accepted.
        const int cols = queryNum(req, "c", line::kColsMin, line::kColsMax, 80);
        const int rows = queryNum(req, "r", line::kRowsMin, line::kRowsMax, 24);

        WsCtx* c = freeCtx();
        if (!c) {
            ESP_LOGW(TAG, "no gateway line slot for fd %d", fd);
            return ESP_FAIL;      // the server closes the socket
        }
        c->hd  = g_hd;
        c->fd  = fd;
        c->gen = ++g_wsGen;
        c->gone.store(false, std::memory_order_release);
        c->busy.store(false, std::memory_order_release);
        c->outLen = 0;
        c->ln.clear();

        c->ln = line::open(line::Src::Ap, &kWsOps, c, static_cast<uint8_t>(b - 1),
                           static_cast<uint16_t>(cols), static_cast<uint16_t>(rows));
        if (!c->ln.set()) {
            // Say so in words down the socket rather than closing it
            // silently: a terminal that opens and vanishes is the worst
            // failure to debug from a phone in a field.
            const char* msg = (line::busy() >= line::kLines)
                ? "x Every line on this gateway is in use."
                : "x This gateway could not reach that board.";
            httpd_ws_frame_t f = {};
            f.type    = HTTPD_WS_TYPE_TEXT;
            f.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(msg));
            f.len     = strlen(msg);
            f.final   = true;
            httpd_ws_send_frame(req, &f);
            c->fd = -1;
            // ESP_OK, not ESP_FAIL: failing here closes the socket at once
            // and would race the frame that explains why, which is the one
            // thing a caller in a field needs. The socket holds nothing (no
            // line, and ctxForFd will not find it again), and the server's
            // own keepalive and lru_purge reap it.
            return ESP_OK;
        }

        // The connection line, in the board's own voice and before the
        // board's own bytes. The gateway says it rather than the board,
        // because in this phase the board cannot tell a gateway caller
        // from any other telnet caller: the uplink is an ordinary socket.
        // From phase 3 the board says it itself and this goes.
        // One line per mode, and the two are deliberately parallel: the
        // same sentence with one word changed, so a caller who has seen
        // the other one reads the difference rather than the words.
        //
        // The 40-column forms are 33 characters, which with the "--> " the
        // board's own markedLine adds is 37 of 39. The phrase is the
        // board's own ("This connection is not securely encrypted" /
        // "... is securely encrypted"), because inventing a third style
        // for the same fact is how a board ends up with three. Final copy
        // is explain's.
        const char* warn;
        if (ap::secure()) {
            warn = (cols >= 80)
                ? "--> You came in over a Wi-Fi gateway, securely encrypted\r\n"
                : "--> Wi-Fi gateway, securely encrypted\r\n";    // 33 characters
        } else {
            warn = (cols >= 80)
                ? "--> You came in over an open Wi-Fi gateway, not encrypted\r\n"
                : "--> Open Wi-Fi gateway, not encrypted\r\n";    // 33 characters
        }
        httpd_ws_frame_t f = {};
        f.type    = HTTPD_WS_TYPE_BINARY;
        f.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(warn));
        f.len     = strlen(warn);
        f.final   = true;
        httpd_ws_send_frame(req, &f);

        ESP_LOGI(TAG, "a phone took a line to board %d at %dx%d", b, cols, rows);
        return ESP_OK;
    }

    WsCtx* c = ctxForFd(fd);

    httpd_ws_frame_t f = {};
    // Length first, with a null payload, which is how the length of a
    // frame is learnt without a buffer big enough to hold anything.
    esp_err_t e = httpd_ws_recv_frame(req, &f, 0);
    if (e != ESP_OK) return e;

    if (f.type == HTTPD_WS_TYPE_CLOSE) {
        if (c) {
            c->gone.store(true, std::memory_order_release);
            line::closeFromSource(c->ln, "you closed the page");
            c->fd = -1;
            c->ln.clear();
        }
        return ESP_OK;
    }
    if (f.len == 0) return ESP_OK;
    if (f.len > sizeof g_wsBuf) {
        // ESP_FAIL, not ESP_OK, and the difference is the whole bug.
        //
        // The length-only call above is NOT a peek: httpd_ws_recv_frame
        // with max_len 0 has already taken the length bytes and the 4-byte
        // mask key off the socket, and the IDF never purges a WebSocket
        // payload (its remaining_len machinery belongs to the HTTP body).
        // So returning OK here would leave f.len bytes of payload in the
        // stream, and the NEXT frame header read would be that payload:
        // either a refusal that kills the session anyway, or - worse - a
        // scrambled subset of the caller's own paste forwarded to the board
        // as keystrokes.
        //
        // Failing closes the socket deterministically. The caller sees the
        // line drop and calls again, which is a bad minute rather than a
        // scrambled board. term.js chunks what it sends so an ordinary
        // paste never reaches this; it is the backstop for a client that
        // does not.
        ESP_LOGW(TAG, "a %u byte frame is too big; closing the line",
                 static_cast<unsigned>(f.len));
        if (c) {
            c->gone.store(true, std::memory_order_release);
            line::closeFromSource(c->ln, "too much at once; call again");
            c->fd = -1;
            c->ln.clear();
        }
        return ESP_FAIL;
    }

    f.payload = g_wsBuf;
    e = httpd_ws_recv_frame(req, &f, sizeof g_wsBuf);
    if (e != ESP_OK) return e;
    if (!c) return ESP_OK;                       // a frame on a line already gone

    if (f.type == HTTPD_WS_TYPE_TEXT) {
        // Control, and the only message is the window size. Splitting
        // control from the caller's bytes by FRAME TYPE rather than by an
        // escape is what makes "a caller types the control sequence"
        // impossible rather than merely unlikely.
        if (f.len >= 3 && g_wsBuf[0] == 's') {
            char buf[24];
            const size_t n = (f.len < sizeof buf - 1) ? f.len : sizeof buf - 1;
            memcpy(buf, g_wsBuf, n);
            buf[n] = 0;
            int cols = 0, rows = 0;
            if (sscanf(buf + 1, "%d %d", &cols, &rows) == 2) {
                line::resized(c->ln, static_cast<uint16_t>(cols),
                              static_cast<uint16_t>(rows));
            }
        }
        return ESP_OK;
    }

    // Fragments. esp_http_server does not reassemble (its own header says
    // so), so a continuation is the tail of a message whose head has
    // already gone to the board. Sending the board half a paste and
    // silently dropping the rest is the worst of the three choices, and
    // "the console says it did" was not true either: a CONTINUE frame used
    // to fall out of the type test below with no log at all.
    if (f.type == HTTPD_WS_TYPE_CONTINUE || !f.final) {
        ESP_LOGW(TAG, "a fragmented message: closing the line rather than "
                      "sending the board half of it");
        c->gone.store(true, std::memory_order_release);
        line::closeFromSource(c->ln, "too much at once; call again");
        c->fd = -1;
        c->ln.clear();
        return ESP_FAIL;
    }
    if (f.type != HTTPD_WS_TYPE_BINARY) return ESP_OK;   // ping and pong are the server's

    // The caller's own bytes. A short take is backpressure, so offer the
    // rest in slices rather than dropping a keystroke: this is the server's
    // task, so the wait is bounded and small.
    // The wait is deliberately SMALL rather than merely bounded: this is
    // the server's one task, so every millisecond here is a millisecond the
    // portal is unserved and every other caller's screen is frozen, because
    // sendWork is queued onto this task too. Three tries of 2 ms, not
    // twenty of ten.
    //
    // It is almost never reached: term.js chunks at 512 bytes against a
    // 1,024-byte ring the pump drains every 20 ms.
    size_t at = 0;
    for (int tries = 0; at < f.len && tries < 3; ++tries) {
        const size_t took = line::fromCaller(c->ln, g_wsBuf + at, f.len - at);
        at += took;
        if (at < f.len) vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (at < f.len) {
        ESP_LOGW(TAG, "dropped %u typed bytes: the board is not keeping up",
                 static_cast<unsigned>(f.len - at));
    }
    return ESP_OK;
}

// The server telling us a socket has gone, which is how a phone that
// simply walked away is noticed. With a close_fn set the server does NOT
// close the socket itself (its own header says so), so that is ours.
void onSockClose(httpd_handle_t hd, int fd) {
    (void)hd;
    for (WsCtx& c : g_ws) {
        if (c.fd != fd) continue;
        // THE ORDER OF THESE THREE LINES IS LOAD-BEARING, here and in
        // every other place a slot is given up. closeFromSource must reach
        // the line BEFORE c.fd = -1 makes the slot available to freeCtx:
        // the pump re-reads the line's state with acquire before it calls
        // room() or send(), so a line already Closing is never touched,
        // whereas a slot handed to a new caller while its line is still Up
        // would have the pump writing into the new caller's context.
        c.gone.store(true, std::memory_order_release);
        line::closeFromSource(c.ln, "your phone left the network");
        c.fd = -1;
        c.ln.clear();
        break;
    }
    ::close(fd);
}

// ---------------------------------------------------------------------------
//  This gateway's own setup page
// ---------------------------------------------------------------------------

// One sysop at a time, which is also the board's own rule for CONFIG: two
// people editing one page is two people overwriting each other.
struct Session {
    char     token[33] = {0};
    uint32_t lastAt    = 0;
    int      ip        = 0;
};
Session g_sess;
constexpr uint32_t kSessIdleMs = 15u * 60u * 1000u;

// Wrong guesses, keyed on the address the phone was given. Five in fifteen
// minutes and that address waits fifteen minutes, which is the board's own
// shape. It is a weak lock by construction (a phone changes its address by
// rejoining) and it is still worth having: it turns an unlimited guessing
// path into a slow one, and the password is the only thing between a
// stranger on an open network and this gateway's settings.
struct Fail {
    int      ip    = 0;
    uint8_t  count = 0;
    uint32_t firstAt = 0;
};
Fail g_fails[4];
constexpr uint32_t kFailWindowMs = 15u * 60u * 1000u;
constexpr uint8_t  kFailMax      = 5;

bool locked(int ip) {
    const uint32_t now = ms();
    for (Fail& f : g_fails) {
        if (f.ip != ip || !f.count) continue;
        if (since(now, f.firstAt) > kFailWindowMs) { f.count = 0; f.ip = 0; return false; }
        return f.count >= kFailMax;
    }
    return false;
}

void failed(int ip) {
    const uint32_t now = ms();
    for (Fail& f : g_fails) {
        if (f.ip == ip && f.count) {
            if (since(now, f.firstAt) > kFailWindowMs) { f.count = 0; f.firstAt = now; }
            if (f.count < 0xFF) ++f.count;
            return;
        }
    }
    // A free slot, an expired one, or the oldest that is NOT ALREADY
    // LOCKED. That last clause is the whole point and the first version
    // had it exactly backwards: evicting the oldest meant somebody who had
    // used up their five tries could send four failures from four other
    // addresses, evict their own record and get five more. The core
    // recorded the same shape last week, where a full ban table could lift
    // a live ban.
    //
    // With every slot locked, nothing new is recorded and locked() says no
    // for an address it has never seen - which fails OPEN for a stranger
    // while four others are locked out. Four slots against fifteen phones
    // makes that unlikely, and the alternative (refusing everybody) would
    // let one attacker lock the sysop out of their own box.
    Fail* pick = nullptr;
    for (Fail& f : g_fails) {
        if (!f.count || since(now, f.firstAt) > kFailWindowMs) { pick = &f; break; }
        if (f.count >= kFailMax) continue;                  // locked: never evicted
        if (!pick || since(now, f.firstAt) > since(now, pick->firstAt)) pick = &f;
    }
    if (!pick) {
        ESP_LOGW(TAG, "every lockout slot is in use; this guess is not counted");
        return;
    }
    pick->ip = ip;
    pick->count = 1;
    pick->firstAt = now;
}

// Is this request from a phone on this gateway's own access point, as
// opposed to the house network it may also have joined? httpd_start binds
// every interface, so without this the whole surface is on the home LAN
// too.
bool onOwnAp(httpd_req_t* req) {
    if (!ap::apUp()) return false;
    const int ip = peerIp(req);
    if (!ip) return false;
    // The same /24 the DHCP server hands out, which is what ap.cpp
    // configures. Compared in network order, so the top three octets are
    // the low three bytes.
    uint32_t mine = 0;
    if (inet_pton(AF_INET, ap::addr(), &mine) != 1) return false;
    return (static_cast<uint32_t>(ip) & 0x00FFFFFFu) == (mine & 0x00FFFFFFu);
}

bool signedIn(httpd_req_t* req) {
    // First boot: the page is open, because a gateway with no password set
    // has no other way to be set up and refusing would refuse the only way
    // in. But ONLY from its own access point, which is the board's own rule
    // for its published default ("local only, and only while it is the
    // default") rather than a weaker version of it. A gateway that has also
    // joined a house router does not offer its settings to that network.
    if (!settings::passwordSet()) return onOwnAp(req);
    if (!g_sess.token[0]) return false;
    if (since(ms(), g_sess.lastAt) > kSessIdleMs) { g_sess.token[0] = 0; return false; }

    char cookie[160];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof cookie) != ESP_OK) {
        return false;
    }
    // "gws=" has to START a cookie, or a cookie called "Xgws" would match
    // on its tail. Cookies are separated by "; ".
    const char* at = nullptr;
    for (const char* p = cookie; (p = strstr(p, "gws=")) != nullptr; p += 4) {
        if (p == cookie || p[-1] == ' ' || p[-1] == ';') { at = p; break; }
    }
    if (!at) return false;
    at += 4;
    const size_t want = strlen(g_sess.token);
    if (strncmp(at, g_sess.token, want) != 0) return false;
    // The value must END there, or a token that is a prefix of another
    // would match. Cookies are separated by ';'.
    if (at[want] != 0 && at[want] != ';' && at[want] != ' ') return false;
    g_sess.lastAt = ms();
    return true;
}

void newSession(httpd_req_t* req, int ip) {
    uint8_t r[16];
    esp_fill_random(r, sizeof r);
    for (int i = 0; i < 16; ++i) {
        snprintf(g_sess.token + i * 2, 3, "%02x", r[i]);
    }
    g_sess.lastAt = ms();
    g_sess.ip     = ip;
    // STATIC, and that is not tidiness. httpd_resp_set_hdr stores the
    // POINTER and does not copy ("Make sure that the lifetime of the field
    // value strings are valid till send function is called",
    // esp_http_server.h). Nothing is sent until the page is rendered, so a
    // local here is dangling by then: the cookie would carry whatever the
    // rendering left on the stack, the session would never match again, and
    // unterminated stack bytes would go out over HTTP. The server is
    // single-threaded, so one buffer is enough.
    //
    // HttpOnly and SameSite=Strict: no script needs it and no other site
    // should be able to send it. Not Secure, because this may be plain
    // HTTP and a Secure cookie would then never be sent at all.
    static char hdr[96];
    snprintf(hdr, sizeof hdr, "gws=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=900",
             g_sess.token);
    httpd_resp_set_hdr(req, "Set-Cookie", hdr);
}

// --- the form's own rendering ---------------------------------------------
void row(httpd_req_t* req, const char* key, const char* label, const char* value,
         const char* hint, const char* type = "text") {
    say(req, "<label for=\"");
    esc(req, key);
    say(req, "\">");
    esc(req, label);
    say(req, "</label><input id=\"");
    esc(req, key);
    say(req, "\" name=\"");
    esc(req, key);
    sayf(req, "\" type=\"%s\" value=\"", type);
    esc(req, value);
    say(req, "\">");
    if (hint && *hint) {
        say(req, "<p class=\"hint\">");
        esc(req, hint);
        say(req, "</p>");
    }
}

void yesno(httpd_req_t* req, const char* key, const char* label, bool on,
           const char* hint) {
    say(req, "<label for=\"");
    esc(req, key);
    say(req, "\">");
    esc(req, label);
    say(req, "</label><select id=\"");
    esc(req, key);
    say(req, "\" name=\"");
    esc(req, key);
    say(req, "\">");
    sayf(req, "<option value=\"no\"%s>No</option>", on ? "" : " selected");
    sayf(req, "<option value=\"yes\"%s>Yes</option>", on ? " selected" : "");
    say(req, "</select>");
    if (hint && *hint) {
        say(req, "<p class=\"hint\">");
        esc(req, hint);
        say(req, "</p>");
    }
}

void setupHead(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    say(req, "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<title>Set up</title><link rel=\"stylesheet\" href=\"/s.css\"></head>"
             "<body><div class=\"wrap\">");
}

void setupFoot(httpd_req_t* req) {
    say(req, "<p class=\"fall\"><a href=\"/\">Back to the boards</a></p>"
             "</div></body></html>");
    httpd_resp_send_chunk(req, nullptr, 0);
}

void askPassword(httpd_req_t* req, const char* problem) {
    setupHead(req);
    say(req, "<h1>Set up</h1><p class=\"sub\">This gateway's own settings.</p>");
    if (problem) {
        say(req, "<p class=\"bad\">");
        esc(req, problem);
        say(req, "</p>");
    }
    say(req, "<form method=\"post\" action=\"/setup\">"
             "<input type=\"hidden\" name=\"do\" value=\"in\">");
    row(req, "pw", "Password", "", "The password whoever planted this box set.",
        "password");
    say(req, "<button type=\"submit\">Sign in</button></form>");
    setupFoot(req);
}

void setupForm(httpd_req_t* req, const char* problem, const char* done) {
    const settings::Cfg& c = settings::get();
    char nm[24];
    settings::name(nm, sizeof nm);
    char num[16];

    setupHead(req);
    say(req, "<h1>Set up</h1><p class=\"sub\">");
    esc(req, nm);
    say(req, " &middot; " GW_WHAT " " GW_VERSION "</p>");

    if (problem) { say(req, "<p class=\"bad\">"); esc(req, problem); say(req, "</p>"); }
    if (done)    { say(req, "<p class=\"good\">"); esc(req, done); say(req, "</p>"); }
    if (settings::restartPending()) {
        say(req, "<p class=\"bad\">Some of what you changed needs this gateway "
                 "restarted before it does anything. Nothing here claims to be "
                 "live when it is not.</p>");
    }
    if (!settings::passwordSet()) {
        say(req, "<div class=\"open\"><strong>This page has no password.</strong> "
                 "Anyone on this open network can change these settings. Set one "
                 "below.</div>");
    }

    say(req, "<form method=\"post\" action=\"/setup\">"
             "<input type=\"hidden\" name=\"do\" value=\"save\">");

    say(req, "<h2>Roles</h2>");
    // A note and not a refusal, because switching the access point off is a
    // legitimate thing to want - a gateway that is only a terminal server
    // does not need one. What is not legitimate is finding out afterwards
    // that it was the only way in, so it is said at the moment of doing it.
    // The same pattern as the board's own rows that warn before saving
    // something that takes effect in an awkward way.
    yesno(req, "ap", "Access point", c.ap,
          "A Wi-Fi network and this portal, for phones. THIS PAGE IS THE ONLY "
          "WAY TO CHANGE A SETTING: switching it off leaves no way back in, "
          "and a reflash will not help because these settings survive one. "
          "Erasing them (pio run -t erase) is the only way back.");
    yesno(req, "termsrv", "Terminal server", c.termsrv,
          "A serial port carrying one terminal. It works at the same time as "
          "the access point.");
    say(req, "<label>Repeater</label>"
             "<input type=\"text\" value=\"not in this build (phase 6)\" disabled>"
             "<p class=\"hint\">Forwarding another gateway's traffic over ESP-NOW. "
             "The setting exists and the role does not yet; it is not switched "
             "off, it is unbuilt.</p>");

    say(req, "<h2>This gateway</h2>");
    row(req, "name", "Name", nm, "Up to 16 characters. Also the Wi-Fi name, "
        "unless you set one below.");
    row(req, "ap_sysop", "Password for this page", "",
        "At least 6 characters. Leave blank to keep the one you have; type a "
        "single space to remove it.", "password");

    say(req, "<h2>Access point</h2>");
    row(req, "ap_ssid", "Wi-Fi name", c.apSsid,
        "Blank uses this gateway's name. On an OPEN network make it specific: "
        "somebody can put up an open network with the same name and there is no "
        "defence at the Wi-Fi layer, so a name people recognise is the whole "
        "mitigation. A password closes that hole as well.");
    row(req, "ap_pass",
        c.apPass[0] ? "Wi-Fi password (set)" : "Wi-Fi password (OPEN network)", "",
        "At least 8 characters, which is WPA2's own minimum. Blank keeps what is "
        "set. It ships as \"unleashed\", which is published rather than secret: "
        "print it on the sign beside the network's name and the air is still "
        "encrypted. A single space removes it and the network becomes open, which "
        "nothing needs and which every caller is then warned about.",
        "password");
    row(req, "ap_addr", "Address", c.apAddr,
        "What a phone reaches this box on, and what to print on the sticker. "
        "172.16.0.1 rather than the usual 192.168.4.1, because some recent "
        "phones are reported not to pop a portal on that one.");
    snprintf(num, sizeof num, "%u", c.apChan);
    row(req, "ap_chan", "Channel", c.apChan ? num : "",
        "Blank follows the board. One radio cannot serve two channels, so an "
        "access point has to be on the channel its boards are on.");
    snprintf(num, sizeof num, "%u", c.apMax);
    row(req, "ap_max", "Phones at once", num, "1 to 15.");
    snprintf(num, sizeof num, "%u", c.apBeacon);
    row(req, "ap_beacon", "Beacon interval", num,
        "In TU, a multiple of 100. Higher saves airtime where there are many of "
        "these boxes, and makes the network slower for a phone to find.");
    snprintf(num, sizeof num, "%u", c.apDtim);
    row(req, "ap_dtim", "DTIM", num,
        "1 to 3. At 1 a sleeping phone's keystrokes come back quickest, which "
        "is the largest single delay in the whole chain.");
    say(req, "<label for=\"ap_probe\">Portal popping</label>"
             "<select id=\"ap_probe\" name=\"ap_probe\">");
    sayf(req, "<option value=\"redirect\"%s>Redirect (302)</option>",
         c.apProbe == settings::PROBE_REDIRECT ? " selected" : "");
    sayf(req, "<option value=\"page\"%s>The page itself (200)</option>",
         c.apProbe == settings::PROBE_PAGE ? " selected" : "");
    say(req, "</select><p class=\"hint\">How a phone's own check is answered. "
             "Try the other one if the portal does not appear by itself.</p>");

    say(req, "<h2>Network this gateway joins</h2>"
             "<p class=\"hint\">Leave blank in a field: there the BOARD joins "
             "this gateway instead, which also settles the channel. Fill it in "
             "at home, where the board is on the house router.</p>");
    row(req, "net_ssid", "Wi-Fi name", c.netSsid, "");
    row(req, "net_pass", "Password", "",
        "Blank keeps the one you have. Retype it whenever you change the name "
        "above, or the gateway tries the new network with the old key.",
        "password");

    say(req, "<h2>Terminal server</h2>");
    snprintf(num, sizeof num, "%d", c.serTx);
    row(req, "ser_tx", "TX pin", num,
        board::pinIsStrap(c.serTx)
            ? "This gateway's TX, to the terminal's RX. -1 is off. This one is a "
              "strapping pin: it works, but it has a job at reset."
            : "This gateway's TX, to the terminal's RX. -1 is off.");
    snprintf(num, sizeof num, "%d", c.serRx);
    row(req, "ser_rx", "RX pin", num,
        board::pinIsStrap(c.serRx)
            ? "This gateway's RX, from the terminal's TX. This one is a strapping "
              "pin: it works, but it has a job at reset."
            : "This gateway's RX, from the terminal's TX.");
    snprintf(num, sizeof num, "%u", static_cast<unsigned>(c.serBaud));
    row(req, "ser_baud", "Baud", num,
        "300, 1200, 2400, 9600, 19200, 38400, 57600 or 115200, matching the "
        "terminal's own setting.");
    static const char* kFmtName[] = { "8N1", "7E1", "7N1" };
    say(req, "<label for=\"ser_fmt\">Format</label>"
             "<select id=\"ser_fmt\" name=\"ser_fmt\">");
    for (unsigned i = 0; i < 3; ++i) {
        sayf(req, "<option value=\"%s\"%s>%s</option>", kFmtName[i],
             (c.serFmt == i) ? " selected" : "", kFmtName[i]);
    }
    say(req, "</select><p class=\"hint\">Data bits, parity and stop bits, "
             "matching the terminal's own setting. A VT220 is often 7E1.</p>");
    snprintf(num, sizeof num, "%d", c.serBtn);
    row(req, "ser_btn", "Button pin", num,
        "A press hangs up, and a press on an idle line calls. -1 is off.");
    snprintf(num, sizeof num, "%u", c.serIdle);
    row(req, "ser_idle", "Idle minutes", num,
        "Silence before the line is freed. 1 to 120.");
    snprintf(num, sizeof num, "%u", c.serBoard);
    row(req, "ser_board", "Board", c.serBoard ? num : "",
        "Which board this line calls, 1 to 5. Blank asks.");

    say(req, "<h2>Boards</h2>");
    for (uint8_t i = 1; i <= settings::kBoards; ++i) {
        const settings::Board& b = settings::get().boards[i - 1];
        char key[12];
        sayf(req, "<h2 style=\"margin-top:1.2rem\">%u</h2>", i);
        snprintf(key, sizeof key, "b%u_name", i);
        row(req, key, "Name", b.name, "Blank empties the slot.");
        snprintf(key, sizeof key, "b%u_host", i);
        row(req, key, "Address", b.host,
            "The board's own address, as numbers. A name cannot be used: there "
            "is nothing on this network to look it up with.");
        snprintf(key, sizeof key, "b%u_port", i);
        snprintf(num, sizeof num, "%u", b.port);
        row(req, key, "Port", num, "6400 unless the sysop changed it.");
        snprintf(key, sizeof key, "b%u_note", i);
        row(req, key, "Note", b.note, "A word or two shown on the portal.");
    }

    say(req, "<button type=\"submit\">Save</button></form>");
    setupFoot(req);
}

esp_err_t setupGet(httpd_req_t* req) {
    if (!signedIn(req)) { askPassword(req, nullptr); return ESP_OK; }
    setupForm(req, nullptr, nullptr);
    return ESP_OK;
}

// --- the form coming back -------------------------------------------------
int hexNib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// In place, and it can only shrink.
void urlDecode(char* s) {
    char* w = s;
    for (char* p = s; *p; ++p) {
        if (*p == '+') { *w++ = ' '; continue; }
        if (*p == '%' && p[1] && p[2]) {
            const int hi = hexNib(p[1]), lo = hexNib(p[2]);
            if (hi >= 0 && lo >= 0) {
                *w++ = static_cast<char>(hi * 16 + lo);
                p += 2;
                continue;
            }
        }
        *w++ = *p;
    }
    *w = 0;
}

esp_err_t setupPost(httpd_req_t* req) {
    const int ip = peerIp(req);

    // A body bigger than this is not a form from this page, so it is
    // refused rather than read: the alternative is reading whatever a
    // stranger on an open network cares to send.
    static char body[2048];
    if (req->content_len == 0 || req->content_len >= sizeof body) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "too much");
        return ESP_OK;
    }
    // Bounded, because esp_http_server is ONE task and this is reachable
    // without a password. A client that sends one byte of a declared 2,000
    // and then nothing would otherwise hold this loop for ever, and with it
    // the portal, every asset, every new WebSocket handshake AND every live
    // caller's screen, because sendWork is queued onto this same task. It
    // would not even trip the watchdog: httpd_req_recv blocks rather than
    // spinning, so the board would hang quietly instead of rebooting.
    //
    // recv_wait_timeout is 5 s, so three stalls is about fifteen seconds
    // for a form being submitted from a phone in the same room. Past that
    // it is not a form.
    size_t at = 0;
    int stalls = 0;
    while (at < req->content_len) {
        const int r = httpd_req_recv(req, body + at, req->content_len - at);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++stalls <= 3) continue;
            memset(body, 0, sizeof body);
            return ESP_FAIL;
        }
        stalls = 0;
        at += static_cast<size_t>(r);
    }
    body[at] = 0;

    // Signing in.
    char what[8] = {0};
    httpd_query_key_value(body, "do", what, sizeof what);
    if (!strcmp(what, "in")) {
        if (locked(ip)) {
            askPassword(req, "Too many wrong tries from this phone. "
                             "Wait fifteen minutes.");
            return ESP_OK;
        }
        char pw[80] = {0};
        if (httpd_query_key_value(body, "pw", pw, sizeof pw) == ESP_OK) {
            urlDecode(pw);
            if (settings::passwordOk(pw)) {
                memset(pw, 0, sizeof pw);     // not left in the stack for the next handler
                newSession(req, ip);
                setupForm(req, nullptr, nullptr);
                return ESP_OK;
            }
        }
        memset(pw, 0, sizeof pw);
        failed(ip);
        askPassword(req, "That is not the password.");
        return ESP_OK;
    }

    if (!signedIn(req)) { askPassword(req, nullptr); return ESP_OK; }

    // Saving. Every value goes through settings::set(), which is the only
    // validator there is, and the FIRST refusal stops the save: a form that
    // applied half of itself and then complained would leave a sysop
    // guessing which half.
    static const char* kKeys[] = {
        "name", "ap", "termsrv",
        "ap_ssid", "ap_pass", "ap_addr", "ap_chan", "ap_max", "ap_beacon",
        "ap_dtim", "ap_probe",
        "net_ssid", "net_pass",
        "ser_tx", "ser_rx", "ser_baud", "ser_fmt", "ser_idle", "ser_board",
        "ser_btn",
    };
    char problem[96] = {0};

    // settings::set applies as it validates, and some of what it applies is
    // live at once: ap_probe answers the next OS probe, ser_idle is read
    // every pass, and every board address is read by the next call. So a
    // form refused on its fourth row would already have changed the first
    // three, and the page would show the half-applied values as though they
    // were saved. Snapshot first and put it all back on a refusal: one
    // copy of a settings struct is cheaper than a second validator that
    // could disagree with the first.
    settings::snapshot();

    for (const char* k : kKeys) {
        char v[80];
        if (httpd_query_key_value(body, k, v, sizeof v) != ESP_OK) continue;
        urlDecode(v);
        // A blank password field means "keep what you have", because the
        // alternative is a sysop who opens the page, saves one unrelated
        // row and silently drops a network's key. The core paid for this
        // exact trap on its own Wi-Fi page. A single space is how one is
        // deliberately removed, which for ap_pass means the network becomes
        // open; settings::set sees the empty string and says so.
        const bool isPw = !strcmp(k, "net_pass") || !strcmp(k, "ap_pass");
        if (isPw && !v[0]) continue;
        if (isPw && v[0] == ' ' && !v[1]) v[0] = 0;
        const char* e = settings::set(k, v);
        if (e) { snprintf(problem, sizeof problem, "%s: %s", k, e); break; }
    }

    if (!problem[0]) {
        for (uint8_t i = 1; i <= settings::kBoards && !problem[0]; ++i) {
            static const char* kParts[] = { "name", "host", "port", "note" };
            for (const char* part : kParts) {
                char k[12], v[80];
                snprintf(k, sizeof k, "b%u_%s", i, part);
                if (httpd_query_key_value(body, k, v, sizeof v) != ESP_OK) continue;
                urlDecode(v);
                const char* e = settings::set(k, v);
                if (e) { snprintf(problem, sizeof problem, "%s: %s", k, e); break; }
            }
        }
    }

    if (!problem[0]) {
        char pw[80] = {0};
        if (httpd_query_key_value(body, "ap_sysop", pw, sizeof pw) == ESP_OK) {
            urlDecode(pw);
            if (pw[0] == ' ' && !pw[1]) {
                settings::passwordSetTo("");          // a single space removes it
                g_sess.token[0] = 0;
            } else if (pw[0]) {
                const char* e = settings::passwordSetTo(pw);
                if (e) snprintf(problem, sizeof problem, "password: %s", e);
                else   g_sess.token[0] = 0;           // sign in again with the new one
            }
        }
        memset(pw, 0, sizeof pw);
    }

    memset(body, 0, sizeof body);   // it held a Wi-Fi key and a password

    if (problem[0]) {
        settings::rollback();
        setupForm(req, problem, nullptr);
        return ESP_OK;
    }
    if (!settings::save()) {
        settings::rollback();
        setupForm(req, "Nothing could be written to this gateway's own storage.",
                  nullptr);
        return ESP_OK;
    }
    settings::commit();
    if (!settings::passwordSet() || !g_sess.token[0]) {
        askPassword(req, nullptr);
        return ESP_OK;
    }
    setupForm(req, nullptr, "Saved.");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
//  The handler table
// ---------------------------------------------------------------------------
// Built field by field from a zeroed struct, never as a positional list.
// httpd_uri_t's tail fields are Kconfig-dependent (the WebSocket handshake
// callbacks sit behind an #if), so a positional list is a field order this
// code does not control; the core paid for exactly that with its plugin
// descriptors, where a field inserted in the middle shifted every one
// after it. A designated list would be as safe and warns about the tail,
// so this is the shape that is both.
httpd_uri_t mkUri(const char* path, httpd_method_t method,
                  esp_err_t (*handler)(httpd_req_t*), bool ws = false) {
    httpd_uri_t u = {};
    u.uri          = path;
    u.method       = method;
    u.handler      = handler;
    u.is_websocket = ws;
    return u;
}

}  // namespace

// -------------------------------------------------------------------------
bool begin() {
    if (g_hd) return true;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size       = kHttpdStack;
    cfg.max_open_sockets = kMaxClients;
    // One handler per asset plus the portal, the setup pair and the socket.
    cfg.max_uri_handlers = kWebAssetCount + 4;
    // A public access point wants the least-recently-used connection
    // closed rather than a new one refused: a phone that wandered off
    // holding a socket should not be what stops the next caller.
    cfg.lru_purge_enable = true;
    // And keepalive on top, which is what notices a phone that walked away
    // mid-session and frees its line rather than holding it for the idle
    // timeout. On a ten-line board that is the difference between ten
    // lines and three.
    cfg.keep_alive_enable   = true;
    cfg.keep_alive_idle     = 30;
    cfg.keep_alive_interval = 10;
    cfg.keep_alive_count    = 3;
    cfg.close_fn            = onSockClose;
    // Core 0 with the radio rather than core 1 with the pump: a 70 KB
    // chunked send of the terminal's assets should not sit in front of a
    // caller's keystrokes on the core that carries them.
    cfg.core_id             = 0;

    esp_err_t e = httpd_start(&g_hd, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd: %s", esp_err_to_name(e));
        g_hd = nullptr;
        return false;
    }

    for (WsCtx& c : g_ws) {
        c.fd = -1;
        c.hd = g_hd;
    }
    for (CloseWork& w : g_closeWork) w.fd = -1;

    const httpd_uri_t portal = mkUri("/", HTTP_GET, portalGet);
    const httpd_uri_t setupG = mkUri("/setup", HTTP_GET, setupGet);
    const httpd_uri_t setupP = mkUri("/setup", HTTP_POST, setupPost);
    const httpd_uri_t ws     = mkUri("/ws", HTTP_GET, wsHandler, true);
    httpd_register_uri_handler(g_hd, &portal);
    httpd_register_uri_handler(g_hd, &setupG);
    httpd_register_uri_handler(g_hd, &setupP);
    httpd_register_uri_handler(g_hd, &ws);
    for (const WebAsset& a : kWebAssets) {
        const httpd_uri_t u = mkUri(a.path, HTTP_GET, assetGet);
        httpd_register_uri_handler(g_hd, &u);
    }
    // One handler for every operating system's probe, because each of them
    // asks for a path this gateway does not serve. See probeAll().
    httpd_register_err_handler(g_hd, HTTPD_404_NOT_FOUND, probeAll);

    ESP_LOGI(TAG, "portal up on http://%s/, %u bytes of gzipped pages, "
                  "%d clients at once",
             ap::addr(), static_cast<unsigned>(payloadBytes()), kMaxClients);
#if GW_WEB_STUB
    ESP_LOGE(TAG, "THIS IMAGE HAS NO BROWSER TERMINAL: it was built with no "
                  "network, so /t will not work. Not a release image.");
#endif
    return true;
}

void stop() {
    if (!g_hd) return;
    httpd_stop(g_hd);
    g_hd = nullptr;
}

bool up() { return g_hd != nullptr; }

uint32_t payloadBytes() { return GW_WEB_GZ_BYTES; }

}  // namespace web

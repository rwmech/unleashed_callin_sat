# µnleashed gateway sat

A front door for a [µnleashed BBS](https://github.com/rwmech/unleashed_BBS) board: a cheap ESP32 that
puts the board in front of a phone, a terminal, or both at once.

**Applies to versions:** gateway sat firmware 0.1.0 (phase 1), against µnleashed BBS firmware 1.2.1
or later. Phase 1 needs **no change to the board at all**.

> **Phase 1, and it is not tested yet.** This is the first phase of the design in the core
> repository's `internal/spec-callin-node-2026-10-05.md`. It is built and code-reviewed; it has not
> been measured on a phone or flashed to a board that anyone calls. Treat every figure here as
> arithmetic until the bench says otherwise.

## What it is

One device, one firmware, three roles that are **settings and not builds**:

| Role | What it offers | Phase 1 |
|---|---|---|
| **access point** | A Wi-Fi network, a portal page that pops up by itself, a list of boards and a terminal in the browser. No app, nothing to install | **yes** |
| **terminal server** | A serial port carrying a real terminal: a VT220, a PC's DB9 through a null modem, later a modem answering a phone line | **yes**, and it works at the same time as the access point |
| **repeater** | Forwarding another gateway's traffic over ESP-NOW, so a box out of the board's range still reaches it | **no.** The setting exists and refuses, naming the phase |

All three compose. The access point being on does not stop the serial port working, which is the
point of building the framework before the roles.

## How it reaches a board

Phase 1 reaches a board **over IP**: a plain telnet connection to the board's own address. Two
shapes, and neither needs pairing, ESP-NOW, or a single line of board code:

- **No router anywhere.** The board joins *the gateway's* own access point as a station. The gateway
  is the board's network and its front door at once. This is the recommended shape with no router,
  because it also settles the Wi-Fi channel by construction.
- **Both on a house router.** The gateway joins the router as a station and runs its access point
  beside it. A phone on either network reaches the board.

Reaching a board **over the link** (ESP-NOW, sealed end to end with AES-128-CCM, no Wi-Fi between
them) is phase 4 and wants the board's CALLIN work in firmware 1.2.3 under it.

## Encryption

**The Wi-Fi has a password by default, so the hop from the phone is encrypted.** WPA2, which on this
framework means CCMP, between the phone and the gateway. The password ships as `unleashed` and is
**published rather than secret** — print it on the sign beside the network's name. What it buys is
not privacy from the caller, it is CCMP on the air, so a passer-by with a laptop reads nothing. And
because that hop is encrypted, it makes no difference to anybody that the portal is plain HTTP, in
the same way it makes no difference on your own router's settings page.

**The second hop, gateway to board, is plain telnet in phase 1**, and what protects it is whichever
Wi-Fi carries it rather than anything this firmware does. On the recommended shape — the board joined
to this gateway's own access point — that is the same WPA2, so both hops are CCMP. On a house router
it is that router's WPA2. On an open network it is nothing. From phase 4 the second hop is the
µnleashed link's own AES-128-CCM over ESP-NOW, sealed end to end and needing no Wi-Fi between them at
all; until then, do not read "sealed" into it.

**Clearing the password gives an open network, which is the exception and not the default.** Then
anyone in radio range can read every keystroke and every screen, a password included, and the portal
says so. HTTPS could not rescue that case either: a page a network pops up by itself cannot carry a
certificate anybody has signed. A gateway in that state tells every caller, in the board's own
voice, and the advice is the usual one: say what you would say out loud, and use a password you use
nowhere else.

A password of one to seven characters is refused with its reason, rather than quietly becoming an
open network. On the pinned framework there is nothing between the two: WPA2-PSK, where the driver
gives CCMP for free, or open. OWE (RFC 8110, Wi-Fi CERTIFIED Enhanced Open), which would encrypt a
network that has no password at all, is not available to us — soft-AP does not support it on
ESP-IDF 5.3.1, which is what `platformio.ini` pins.

## Phase 1's own limits

**A gateway caller is an ordinary telnet caller as far as the board is concerned**, because the
uplink is a socket like any other. The board cannot tell one apart yet, which means the rule that
matters — no staff elevation over an access-point line — arrives with CALLIN in phase 3 and is not
enforceable from here. The terminal server's wired line becomes the one path that is private end to
end once CALLIN seals its second hop.

## Layout

| Folder | What it is |
|---|---|
| `firmware/` | The gateway's firmware, a PlatformIO project for a base ESP32 |
| `firmware/web/` | The portal and the browser terminal, gzipped into the app image at build |
| `host/` | The tests that need no board, for the four units that parse what somebody else sent |

There is no `bbs/` folder yet: phase 1 changes nothing on the board, so there is no plugin to build
into it. The board's half arrives with CALLIN in phase 3.

## Building it

```
git clone https://github.com/rwmech/unleashed_callin_sat
cd unleashed_callin_sat/firmware
pio run -t upload --upload-port COM5
```

The build fetches [xterm.js](https://xtermjs.org/) 5.5.0 and its fit addon from the npm registry,
refuses any tarball whose SHA-256 is not the one `web/xterm.lock` names, and gzips every page into
the app image. Nothing third-party is stored in this repository. With no network and no cache the
build still goes through, with a placeholder terminal and a loud warning; `pio run -e gateway_release`
turns that warning into a compile error, so a release image cannot ship without a terminal.

Measured at build on a bare ESP32-WROOM-32E:

| | Bytes |
|---|---|
| Image | 851,024 of the 4,128,768 app partition (20.6%) |
| The web payload inside it, gzipped | 76,029 |
| Static DRAM | 46,920 of 180,736, so 133,816 free |

Flash is not the constraint and neither is static DRAM. **Internal heap with the access point up is
the open question**, and it needs a board: see phase 2.

## Testing it

Four units have no platform in them, and between them they are every place this firmware parses
something somebody else sent it. They run on a host, so they need no board:

```
cd host && make          # build and run all four
cd host && make break    # prove each guard by removing it
cd host && make asan     # the same under AddressSanitizer and UBSan
```

1,826 checks over the SPSC ring, the telnet client, the DNS wire format and the settings validator.

**`make break` is the part worth understanding.** It compiles each suite once per `GW_TEST_BREAK`
value, each removing exactly one guard from the code under test, and **refuses to pass unless
something catches every one**. A test written after the fix that passes immediately has proved
nothing, and this project's parent has shipped tests that agreed with the bug. A guard can be proved
three ways and the run says which: a check fails, the code hangs, or ASan trips — that last being the
only proof available for a guard whose whole job is to stop an out-of-bounds read, where what gets
read is whatever is next in memory and no verdict need change.

It has already earned it, five times, four of them against my own tests:

- every compression-pointer test was a short packet, so with the pointer guard removed the `0xC0` ran
  off the end and a *different* guard refused it. They proved the packet was refused without proving
  which check did it;
- the truncation sweep sliced a long-lived buffer, so a read past the end was still inside the
  allocation and returned a neighbouring octet that something further down refused;
- the ring's drop clamp was tested by dropping 99 from a ring of 32, and 99 happens to land on a
  multiple of 32, so the mask hid its absence entirely;
- `make break` itself first reported "every guard has a test" having tested none, because it built its
  case list from a shell variable that did not exist;
- and in the code rather than the tests: written as an `enum`, the break switch was invisible to the
  preprocessor, so every `#if` compared 0 against an unknown identifier and **every guard was
  compiled out of the firmware**. `-Werror` caught it in under a minute on an unused parameter.

The encoders are written from the RFCs and share no code with the implementations — RFC 1035's wire
format for DNS, RFC 854/855/856/858/1073's tables for telnet, with the expected bytes spelt out as
literals. The parent project lost two rounds to a telnet test client written beside the server, which
honoured the board's own quirks and let real bugs through while hardware failed; its recorded
conclusion is the rule followed here.

## A framework move now costs something on both sides

Worth reading before anybody bumps `platformio.ini` past `espressif32@6.9.0` (ESP-IDF 5.3.1),
because one half of this is invisible and was found by reading the IDF rather than by anything
failing.

**Moving to IDF 6 would silently break the access-point role.** On 5.3.1, `httpd_uri()` answers a
WebSocket handshake and then calls the URI's handler, which is how `wsHandler`'s `HTTP_GET` block
runs at all — and that block is where the caller's line is taken. IDF 6.1.0 ends that path with
*"If the request is websocket handshake, then do not call the uri->handler"* and returns. Under that
framework the block becomes dead code: no line, no connection line, and every keystroke dropped. The
browser would say "connected" and the terminal would simply never do anything. **No compile error, no
log line, no partial symptom, and the whole role gone.** The portable shape is to claim the line
lazily on the first data frame instead; it is not done that way here because the handshake is also
where the query string lives (which board, and the window size) and a data frame does not carry it.
`web.cpp` says so at the handler.

**And moving to IDF 6 would gain something real, which is why this is a trade rather than a
warning.** OWE (RFC 8110, Wi-Fi CERTIFIED Enhanced Open) gives every client its own keys on a
network with *no password at all* — exactly the open-AP case this firmware has to apologise for. It
is behind `CONFIG_ESP_WIFI_ENABLE_WPA3_OWE_SOFTAP`, and checked against the frameworks installed on
the development machine it is **absent in 5.3.1 and 5.5.3 and present in 6.1.0**; on 5.3.1 soft-AP
does not merely lack it, `esp_wifi_types_generic.h` says the mode is unsupported there. It would also
be partial rather than a fix, because a phone too old for OWE falls back to open in transition mode.

So: a bump buys OWE for the open case and costs the handshake path. Do both pieces of work in the
same change, or neither.

## Open questions for later phases

- **Phase 2 must read the internal heap on a bare WROOM** with the access point up, a phone's worth
  of sockets open and a terminal served, and its lowest-ever figure, not just the figure at rest.
  Until then nobody should promise a WROOM; `gateway_wrover` is the hedge, same money with PSRAM.
- **Whether the portal pops by itself** on a current iPhone, a current Android and a Samsung. The
  `ap_probe` setting switches between a 302 redirect and answering 200 with the page, because some
  recent Android devices are reported to need the second; both are untested here.
- **Phase 3 inherits a changed premise about staff, and one chain worth seeing whole.** Nothing here
  changes; both notes are for whoever builds `callinStaffAllowed`.
  - The specification refuses staff elevation on an access-point line *because the access point is
    open*, and that argument does not hold now the access point is passworded by default. The
    residual hole is that WPA2-PSK lets anyone else holding the same password decrypt your traffic,
    which in a private deployment is nobody. The refusal stays exactly as specified and no staff path
    exists here; the reasoning behind it moved.
  - The default Wi-Fi password is the same word as the board's own *published* default sysop
    password, and in the no-router case this gateway **is** the board's network, so a caller who read
    the sign sits on a local address — which is the one place a board still on its published default
    accepts it. It only bites on a board nobody has set up: the first-boot flow pushes every sysop
    off the default, and a board still on it starts `closed`. The phase 3 refusal closes it properly.

## Licence

GPL-3.0-or-later. Copyright 2026 Robert Mech. See `LICENSE`.

The browser terminal is xterm.js 5.5.0 and `@xterm/addon-fit` 0.10.0, both MIT, fetched at build
time and not redistributed here; `firmware/web/xterm.lock` pins each version and its hash, and a
gateway serves both notices at `/licence`.

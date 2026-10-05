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

**The Wi-Fi has a password by default, so every hop is encrypted**: CCMP from the phone to the
gateway, and the µnleashed link's own AES-128-CCM from the gateway to the board. The password ships
as `unleashed` and is **published rather than secret** — print it on the sign beside the network's
name. What it buys is not privacy from the caller, it is CCMP on the air, so a passer-by with a
laptop reads nothing. Because every hop is encrypted, it makes no difference to anyone that the
portal is plain HTTP.

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
| Image | 845,312 of the 4,128,768 app partition (20.5%) |
| The web payload inside it, gzipped | 76,029 |
| Static DRAM | 45,808 of 180,736, so 134,928 free |

Flash is not the constraint and neither is static DRAM. **Internal heap with the access point up is
the open question**, and it needs a board: see phase 2.

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

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
| **access point** | An open Wi-Fi network, a portal page that pops up by itself, a list of boards and a terminal in the browser. No app, nothing to install | **yes** |
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

Reaching a board **over the link** (ESP-NOW, sealed end to end, no Wi-Fi between them) is phase 4
and wants the board's CALLIN work in firmware 1.2.3 under it.

## The honest part, up front

**The hop from a phone to this box is not encrypted, and cannot be.** It is an open Wi-Fi network
with no password, so anyone nearby with the right equipment can read every keystroke and every
screen, a password included. A captive portal cannot be served over HTTPS without a certificate
nobody has signed, so the choice is plain HTTP with an honest sentence or a full-page security
warning as the front door, and this project says the sentence. The portal says it, and so does the
board's own connection line.

So: say what you would say out loud, and use a password you use nowhere else.

**And in phase 1 a gateway caller is an ordinary telnet caller as far as the board is concerned**,
because the uplink is a socket like any other. The board cannot tell one apart yet, which means the
rule that matters — no staff elevation over an open access point — arrives with CALLIN in phase 3
and is not enforceable here. Until then: do not type a staff password into a gateway on an open
network. The terminal server's wired line is a different matter and is the one line that is private
by construction.

## Layout

| Folder | What it is |
|---|---|
| `firmware/` | The gateway's firmware, a PlatformIO project for a base ESP32 |
| `firmware/web/` | The portal and the browser terminal, gzipped into the app image at build |

There is no `bbs/` folder yet: phase 1 changes nothing on the board, so there is no plugin to build
into it. The board's half arrives with CALLIN in phase 3.

## Licence

GPL-3.0-or-later. Copyright 2026 Robert Mech. See `LICENSE`.

The browser terminal is [xterm.js](https://xtermjs.org/) 5.5.0, MIT, fetched at build time and not
redistributed in this repository; `firmware/web/xterm.lock` pins the version and its hash.

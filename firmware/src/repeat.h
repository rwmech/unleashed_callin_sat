// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/repeat.h
// Module:       The repeater role, which is not in this build
//
// Purpose:      The third role exists here as a setting and a refusal, and
//               that is deliberate rather than an oversight: the framework
//               is shaped for three roles from the first commit, so the
//               day the repeater is built it is a source of sealed frames
//               beside the two that already exist rather than a reason to
//               rearrange anything.
//
//               It is NOT "switched off". It is unbuilt, and the setting
//               says so by naming the phase, because a setting that saves
//               and then does nothing is the "Saved and live" trap this
//               project has paid for more than once.
//
//               What it will be, so nobody re-derives it: a repeater
//               forwards sealed ESP-NOW frames it CANNOT OPEN between a
//               downstream peer and the board or the next hop up. That it
//               cannot read what it carries is forced rather than
//               promised: the ECDH secret and every session key belong to
//               the far peer and the board alone, and every frame is
//               AES-128-CCM with its header as associated data. So a
//               repeater can drop or delay a frame and can do nothing else
//               to it. A gateway that repeats AND carries its own caller
//               is safe for exactly that reason: its own caller is its own
//               session under its own key, and forwarded frames are opaque
//               bytes with a wrapper in front.
//
//               Three things it needs that this phase does not have: the
//               link engine, which is the core's and arrives in phase 3; a
//               hop count in the wrapper, so a chain planted in a loop
//               decrements to zero and drops rather than filling the
//               channel; and three boxes and open ground, which is why it
//               is phase 6 and not a desk job.
//
//               And one thing it can never be, worth writing down because
//               somebody will ask: a repeater cannot bridge to IP. It has
//               no key for what it carries, so it cannot open a forwarded
//               frame and re-originate it as telnet. Only a gateway that
//               TERMINATES a session can do that, and one that terminates
//               a session is carrying its own caller rather than repeating.
//
// See also:     settings.cpp (where `repeat = yes` is refused by name),
//               the spec's §7
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
#pragma once

namespace repeat {

// False in this build, always. Here so that main() reports the three roles
// from one shape rather than special-casing the missing one, and so the day
// it is built there is one place that changes.
constexpr bool available = false;

// Why not, in the words the console and the settings page both use.
constexpr const char* unavailable =
    "repeating is not in this build; it is phase 6";

}  // namespace repeat

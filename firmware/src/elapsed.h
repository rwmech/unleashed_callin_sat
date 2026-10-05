// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/elapsed.h
// Module:       How long ago, and the only way this firmware asks
//
// Purpose:      ONE rule, in ONE place: elapsed time is `since(now, at)`.
//
//               Not `now - at`, which is unsigned and so reads a stamp a
//               few milliseconds in the FUTURE as 49 days. That is not
//               hypothetical here: a stamp can be taken on the Wi-Fi event
//               task, on the HTTP server's task or on a caller's source
//               while the pass that compares it read its `now` a moment
//               earlier.
//
//               And not a signed difference either, which is the obvious
//               fix and is wrong the other way: signed, a genuine gap past
//               24.8 days turns negative and a stamp kept for ever then
//               reads as "no time at all". The core went through both
//               shapes in order, across twelve sites, before arriving at
//               this one: unsigned, with a window of about 65 seconds of
//               "slightly ahead" collapsed to zero.
//
//               It lives in a header of its own because it was three
//               copies in three translation units, which is the drift the
//               core's own plat::since exists to prevent: three places to
//               get right, and nothing to notice when one of them is not.
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
#include <cstdint>

#include "esp_timer.h"

namespace when {

// Milliseconds since boot. One clock, so every stamp is comparable.
inline uint32_t ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// How long ago `at` was, given this pass's `now`.
//
// A stamp up to about 65 seconds AHEAD of now reads 0, which is what makes
// a stamp taken on another task safe. Anything genuinely older reads its
// real age, including an age past 24.8 days.
inline uint32_t since(uint32_t now, uint32_t at) {
    const uint32_t d = now - at;
    return (d > 0xFFFF0000u) ? 0u : d;
}

// The same, reading the clock itself. For a caller with no `now` to hand.
inline uint32_t sinceNow(uint32_t at) { return since(ms(), at); }

}  // namespace when

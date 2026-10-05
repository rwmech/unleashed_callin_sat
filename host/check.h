// ===========================================================================
//  µnleashed gateway sat: the host tests
// ===========================================================================
//
// File:         host/check.h
// Module:       The smallest thing that counts checks
//
// Purpose:      No framework. A check, a count and an exit code, because a
//              test runner is not the interesting part of a test and a
//              dependency here would be one more thing to install before
//              anybody can run them.
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
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace chk {

inline int g_ran = 0;
inline int g_bad = 0;
inline const char* g_group = "";

inline void group(const char* name) {
    g_group = name;
    printf("-- %s\n", name);
}

inline void ok(bool cond, const char* what) {
    ++g_ran;
    if (cond) return;
    ++g_bad;
    printf("   FAIL  %s: %s\n", g_group, what);
}

inline void eq(long got, long want, const char* what) {
    ++g_ran;
    if (got == want) return;
    ++g_bad;
    printf("   FAIL  %s: %s (got %ld, wanted %ld)\n", g_group, what, got, want);
}

inline void bytes(const unsigned char* got, const unsigned char* want, size_t n,
                  const char* what) {
    ++g_ran;
    if (memcmp(got, want, n) == 0) return;
    ++g_bad;
    printf("   FAIL  %s: %s\n        got ", g_group, what);
    for (size_t i = 0; i < n; ++i) printf("%02x ", got[i]);
    printf("\n        want ");
    for (size_t i = 0; i < n; ++i) printf("%02x ", want[i]);
    printf("\n");
}

inline int done(const char* suite) {
    printf("%s: %d checks, %d failed\n", suite, g_ran, g_bad);
    return g_bad ? 1 : 0;
}

}  // namespace chk

// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/version.h
// Module:       The firmware's own version
//
// Purpose:      One place. The board profile carries its own version beside
//               this one, the way the core's board profiles do, and both are
//               shown wherever a version shows: "0.1.0 (WROOM 1.0.0)".
//               ASCII, because the same string reaches a C64 through the
//               board's own screens one day.
//
//               Keep GW_VERSION and PROJECT_VER in ../CMakeLists.txt the
//               same. Bump it with every commit, so no two commits report
//               the same build: a version that does not identify a build is
//               worse than no version, because it is trusted.
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

#define GW_VERSION "0.1.0"

// What this box is called where a person reads it. The device is a
// "gateway sat"; "callin" survives as the repository's name only, the way
// "camsat" does, and "node" is never used for it: a node is a caller line
// everywhere in the µnleashed tree.
#define GW_WHAT "gateway sat"

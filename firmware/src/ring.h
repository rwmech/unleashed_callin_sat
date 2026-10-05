// ===========================================================================
//  µnleashed gateway sat
// ===========================================================================
//
// File:         firmware/src/ring.h
// Module:       One byte ring, one producer and one consumer
//
// Purpose:      A caller's keystrokes arrive on whichever task owns their
//               source (the HTTP server's, or the serial reader's) and leave
//               on the pump task. That is exactly one producer and exactly
//               one consumer, which is the one case a ring needs no lock:
//               the producer only ever moves the head and the consumer only
//               ever moves the tail, and each reads the other's index once.
//
//               Acquire/release rather than relaxed, because the index has
//               to be published AFTER the bytes it describes. Relaxed would
//               work on an ESP32 by accident of its memory model and would
//               be wrong the day this is read on anything else.
//
//               Static storage, sized by the template, because the whole
//               point of phase 1 is to know the worst case rather than to
//               discover it when fifteen phones join at once.
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
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

// N must be a power of two: the mask is what keeps the wrap free of a
// division, and a non-power-of-two N would silently corrupt the indices.
template <size_t N>
class Ring {
    static_assert(N >= 16 && (N & (N - 1)) == 0, "Ring size must be a power of two, 16 or more");

public:
    void clear() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    // The producer's side.
    size_t room() const {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_acquire);
        return N - 1 - ((h - t) & (N - 1));
    }

    // Takes what fits and returns how much that was, so a caller that must
    // not lose bytes can wait and offer the rest rather than assuming.
    size_t push(const uint8_t* p, size_t n) {
        const size_t r = room();
        if (n > r) n = r;
        size_t h = head_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) {
            buf_[(h + i) & (N - 1)] = p[i];
        }
        head_.store(h + n, std::memory_order_release);
        return n;
    }

    // The consumer's side.
    size_t used() const {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_relaxed);
        return (h - t) & (N - 1);
    }

    size_t pop(uint8_t* p, size_t n) {
        const size_t u = used();
        if (n > u) n = u;
        size_t t = tail_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) {
            p[i] = buf_[(t + i) & (N - 1)];
        }
        tail_.store(t + n, std::memory_order_release);
        return n;
    }

    // Looks without taking, so the consumer can hand the bytes to something
    // that may refuse them (a socket whose window is shut) and only then
    // drop what was really taken.
    size_t peek(uint8_t* p, size_t n) const {
        const size_t u = used();
        if (n > u) n = u;
        const size_t t = tail_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) {
            p[i] = buf_[(t + i) & (N - 1)];
        }
        return n;
    }

    void drop(size_t n) {
        const size_t u = used();
        if (n > u) n = u;
        tail_.store(tail_.load(std::memory_order_relaxed) + n, std::memory_order_release);
    }

    static constexpr size_t capacity() { return N - 1; }

private:
    uint8_t buf_[N] = {};
    // Free-running counters masked at every use, so head == tail means
    // empty and there is no ambiguous full state to get wrong.
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

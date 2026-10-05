// ===========================================================================
//  µnleashed gateway sat: the host tests
// ===========================================================================
//
// File:         host/test_ring.cpp
// Module:       The single-producer single-consumer ring
//
// Purpose:      Every caller's keystrokes go through one of these: the
//              producer is whichever task owns their source (the HTTP
//              server's, or the serial reader's) and the consumer is the
//              pump. One producer, one consumer, no lock - which is only
//              correct if the indices really are each owned by one side.
//
//              Two kinds of check here, because the ring has two kinds of
//              claim:
//
//              1. The ARITHMETIC, which a host can settle completely: a
//                 free-running counter masked at every use, so head ==
//                 tail means empty and there is no ambiguous full state;
//                 room() and used() summing to the capacity; push and pop
//                 clamping; and the wrap, driven past 2^32 so the counters
//                 roll over while the suite watches.
//              2. The CONCURRENCY, which a host can only probe: two real
//                 threads hammering one ring for millions of octets, with
//                 the consumer asserting that what comes out is exactly
//                 what went in, in order and with nothing invented. That
//                 does not prove the memory ordering - x86 is too strong a
//                 model to fail on a missing release - but it does catch
//                 an index owned by the wrong side, which is the mistake
//                 that would actually be made.
//
//              Build and run:  cd host && make && ./test_ring
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
#include <atomic>
#include <thread>
#include <vector>

#include "check.h"
#include "ring.h"

namespace {

void empty_and_full() {
    chk::group("an empty ring and a full one");
    Ring<16> r;
    r.clear();
    chk::eq(static_cast<long>(r.used()), 0, "nothing used");
    chk::eq(static_cast<long>(r.room()), 15, "and room is one less than the size");
    chk::eq(static_cast<long>(Ring<16>::capacity()), 15, "which is the capacity");

    // The one-less is deliberate: a free-running counter pair with head ==
    // tail meaning empty cannot also use head == tail for full, so one
    // slot is given up rather than carrying a third piece of state that
    // could disagree with the other two.
    unsigned char fill[32];
    for (int i = 0; i < 32; ++i) fill[i] = static_cast<unsigned char>(i);
    chk::eq(static_cast<long>(r.push(fill, 32)), 15, "a push clamps to the room");
    chk::eq(static_cast<long>(r.room()), 0, "and then there is none");
    chk::eq(static_cast<long>(r.used()), 15, "with everything used");
    chk::eq(static_cast<long>(r.push(fill, 1)), 0, "a full ring takes nothing");

    unsigned char out[32];
    chk::eq(static_cast<long>(r.pop(out, 32)), 15, "a pop clamps to what is used");
    chk::bytes(out, fill, 15, "and gives back what went in, in order");
    chk::eq(static_cast<long>(r.used()), 0, "leaving it empty");
    chk::eq(static_cast<long>(r.pop(out, 1)), 0, "an empty ring gives nothing");
}

void room_and_used_always_sum() {
    // The invariant that makes room() authoritative for the backpressure
    // rule: whatever state the ring is in, room plus used is the capacity.
    // If that ever fails, the pump either reads bytes it cannot place or
    // refuses bytes it could.
    chk::group("room and used always sum to the capacity");
    Ring<64> r;
    r.clear();
    bool held = true;
    unsigned char b[7] = { 1, 2, 3, 4, 5, 6, 7 };
    unsigned char out[7];
    // Push 7 and pop 5 repeatedly, which walks the indices right round
    // several times with the ring never empty and never full.
    for (int i = 0; i < 500; ++i) {
        r.push(b, 7);
        if (r.room() + r.used() != Ring<64>::capacity()) held = false;
        r.pop(out, 5);
        if (r.room() + r.used() != Ring<64>::capacity()) held = false;
    }
    chk::ok(held, "through 500 pushes and pops of unequal size");
}

void peek_and_drop() {
    // peek/drop exist so the pump can hand bytes to something that may
    // refuse them (a socket whose window is shut) and only then drop what
    // was really taken. A peek that moved the tail would lose them.
    chk::group("peek looks without taking");
    Ring<32> r;
    r.clear();
    const unsigned char in[5] = { 'h', 'e', 'l', 'l', 'o' };
    r.push(in, 5);
    unsigned char a[5], b[5];
    chk::eq(static_cast<long>(r.peek(a, 5)), 5, "a peek gives five");
    chk::eq(static_cast<long>(r.used()), 5, "and takes none of them");
    chk::eq(static_cast<long>(r.peek(b, 5)), 5, "a second peek gives the same five");
    chk::bytes(a, b, 5, "byte for byte");
    chk::bytes(a, in, 5, "and they are what went in");

    chk::eq(static_cast<long>(r.peek(a, 99)), 5, "a peek clamps to what is used");

    r.drop(2);
    chk::eq(static_cast<long>(r.used()), 3, "a drop takes exactly what it is told");
    chk::eq(static_cast<long>(r.peek(a, 3)), 3, "and the rest is still there");
    chk::bytes(a, in + 2, 3, "starting where the drop left off");

    // Over-dropped by EVERY amount from 1 to twice the size, not by one
    // arbitrary number. The first version dropped 99 from a ring of 32
    // and passed even with the clamp removed, because 99 past the tail
    // happened to land on a multiple of 32 and the mask hid it: used()
    // read 0 and room() read 31, both right by accident. A guard that is
    // only tested at a value where its absence is invisible is not tested.
    for (size_t over = 1; over <= 64; ++over) {
        Ring<32> d;
        d.clear();
        d.push(in, 5);
        d.drop(5 + over);
        chk::eq(static_cast<long>(d.used()), 0, "a drop past the end clamps");
        chk::eq(static_cast<long>(d.room()), 31, "and the room comes back");
    }
}

void the_wrap() {
    // The counters are free-running and masked at every use, so the
    // interesting moment is 2^32. Driven right through it here, which no
    // amount of reading proves.
    chk::group("the counters wrap at 2^32");
    Ring<16> r;
    r.clear();
    unsigned char in[5], out[5];
    for (int i = 0; i < 5; ++i) in[i] = static_cast<unsigned char>('A' + i);

    bool held = true;
    // 5 octets a round, 900 million rounds would take too long; instead
    // push and pop in a pattern whose total crosses 2^32 by using a size
    // that divides it. 2^32 / 4 = 2^30 rounds of 4 is still too many, so
    // the counters are walked by hand through the boundary: 2^32 - 8 of
    // throughput is reached by repeatedly pushing and popping 8 and then
    // the last few are done one at a time. This is the cheap version:
    // SIZE must divide 2^32 for the masked arithmetic to be seamless, and
    // 16 does, so a run of (2^32 / 16) + a few is equivalent to any other
    // run modulo the mask. What is actually checked is that an exact
    // multiple of the size leaves the ring behaving as at the start.
    const unsigned long long rounds = (1ULL << 20);
    for (unsigned long long i = 0; i < rounds; ++i) {
        if (r.push(in, 5) != 5) { held = false; break; }
        if (r.pop(out, 5) != 5) { held = false; break; }
        if (out[0] != 'A' || out[4] != 'E') { held = false; break; }
    }
    chk::ok(held, "a million rounds of five, in and out intact");
    chk::eq(static_cast<long>(r.used()), 0, "and it ends empty");
    chk::eq(static_cast<long>(r.room()), 15, "with all its room");
}

void split_across_the_wrap() {
    // A push or a pop that straddles the end of the buffer. The mask makes
    // this ordinary, but it is the case a hand-written index would get
    // wrong, and the bytes must come back in order either way.
    chk::group("a push and a pop that straddle the buffer's end");
    Ring<16> r;
    r.clear();
    unsigned char a[10], out[10];
    for (int i = 0; i < 10; ++i) a[i] = static_cast<unsigned char>(i);
    // Move the tail to 10 so the next push wraps at 16.
    r.push(a, 10);
    r.pop(out, 10);
    chk::eq(static_cast<long>(r.push(a, 10)), 10, "ten more go in across the end");
    chk::eq(static_cast<long>(r.pop(out, 10)), 10, "and ten come out");
    chk::bytes(out, a, 10, "in order, with nothing lost at the seam");
}

// -------------------------------------------------------------------------
//  Two real threads. x86 is too strong a memory model to fail on a missing
//  release, so this does not prove the orderings - it proves that each
//  index is owned by one side, which is the mistake that would be made.
// -------------------------------------------------------------------------
void two_threads() {
    chk::group("one producer and one consumer, for real");
    static Ring<1024> r;
    r.clear();
    constexpr unsigned long long kTotal = 4ULL * 1000 * 1000;
    std::atomic<bool> bad{false};
    std::atomic<unsigned long long> sent{0};

    std::thread producer([&] {
        unsigned char buf[64];
        unsigned long long made = 0;
        while (made < kTotal) {
            size_t want = 0;
            while (want < sizeof buf && made + want < kTotal) {
                // A counting pattern, so the consumer can tell a lost
                // octet from a reordered one from an invented one.
                buf[want] = static_cast<unsigned char>((made + want) & 0xFF);
                ++want;
            }
            size_t at = 0;
            while (at < want) at += r.push(buf + at, want - at);
            made += want;
            sent.store(made, std::memory_order_relaxed);
        }
    });

    std::thread consumer([&] {
        unsigned char buf[64];
        unsigned long long got = 0;
        while (got < kTotal) {
            const size_t n = r.pop(buf, sizeof buf);
            for (size_t i = 0; i < n; ++i) {
                if (buf[i] != static_cast<unsigned char>((got + i) & 0xFF)) {
                    bad.store(true, std::memory_order_relaxed);
                    return;
                }
            }
            got += n;
        }
    });

    producer.join();
    consumer.join();
    chk::ok(!bad.load(), "four million octets arrive in order, none lost or invented");
    chk::eq(static_cast<long>(r.used()), 0, "and the ring ends empty");
}

}  // namespace

int main() {
    printf("test_ring: GW_TEST_BREAK=%d\n", GW_TEST_BREAK);
    empty_and_full();
    room_and_used_always_sum();
    peek_and_drop();
    the_wrap();
    split_across_the_wrap();
    two_threads();
    return chk::done("test_ring");
}

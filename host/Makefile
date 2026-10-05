# ===========================================================================
#  µnleashed gateway sat: the host tests
# ===========================================================================
#
# File:         host/Makefile
# Module:       Build and run the tests that need no board
#
# Purpose:      Four units have no platform in them and so can be driven on
#              a host: the SPSC ring, the telnet client, the DNS wire
#              format, and the settings validator. Between them they are
#              every place this firmware parses something somebody else
#              sent it.
#
#              make           build and run them all
#              make break     prove each guard by removing it: the suite is
#                             compiled once per GW_TEST_BREAK value and the
#                             checks that then fail are reported. A test
#                             that passes with its guard removed has proved
#                             nothing, and this project has shipped tests
#                             that agreed with the bug.
#
#                             Three ways a guard can be proved, and the
#                             run says which one did it:
#                               - a check fails, which is the usual case;
#                               - the code HANGS, which is the proof for a
#                                 clamp whose absence leaves a consumer
#                                 waiting for octets that never come;
#                               - or AddressSanitizer trips, which is the
#                                 only proof available for a guard whose
#                                 whole job is to stop an out-of-bounds
#                                 READ: what gets read is whatever is next
#                                 in memory, so no verdict need change.
#              make asan      the same, under AddressSanitizer and UBSan,
#                             which is where an out-of-bounds read in the
#                             DNS parser becomes a crash rather than a pass
#
#              Runs under WSL on the development machine (g++ and make).
#
# Copyright 2026 - Robert Mech
# License:      GNU General Public License v3 or later
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 3 of the License, or (at your
# option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program. If not, see <https://www.gnu.org/licenses/>.
# ===========================================================================
SRC  := ../firmware/src
CXX  ?= g++

# -Wall -Wextra -Werror, because a test suite that tolerates a warning is
# a suite that will tolerate the warning that was the bug.
CXXFLAGS := -std=gnu++17 -I. -I$(SRC) -Wall -Wextra -Werror -g -O1
SAN      := -fsanitize=address,undefined -fno-omit-frame-pointer

# The break builds compile code with one guard removed, which can leave a
# parameter unread or a branch unreachable. -Werror is right for the real
# suite and wrong for deliberately degraded code: without this, removing
# the last use of a parameter failed to COMPILE and was reported as
# "DID NOT COMPILE" rather than as the guard being proved.
BREAKFLAGS := $(filter-out -Werror,$(CXXFLAGS)) -Wno-unused-parameter

TESTS := test_ring test_telnet test_dns test_settings

# Which GW_TEST_BREAK values each suite understands, for `make break`.
BREAK_dns      := 1 2 3 4 5 6
BREAK_ring     := 1 2 3
BREAK_telnet   := 1 2 3 4
BREAK_settings := 1 2 3

.PHONY: all test break asan clean
all: test

test: $(TESTS)
	@fail=0; for t in $(TESTS); do echo; ./$$t || fail=1; done; \
	 echo; if [ $$fail -eq 0 ]; then echo "ALL SUITES PASS"; \
	 else echo "A SUITE FAILED"; exit 1; fi

# -pthread, because the ring's concurrency check runs two real threads:
# one producer and one consumer, which is the only way to find an index
# owned by the wrong side.
test_ring: test_ring.cpp check.h $(SRC)/ring.h
	$(CXX) $(CXXFLAGS) -pthread -o $@ $<

test_telnet: test_telnet.cpp check.h $(SRC)/telnet.h $(SRC)/telnet.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(SRC)/telnet.cpp

test_dns: test_dns.cpp check.h $(SRC)/dnsparse.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_settings: test_settings.cpp check.h $(SRC)/settings_rules.h $(SRC)/board.h
	$(CXX) $(CXXFLAGS) -DGW_BOARD_WROOM32 -o $@ $<

# ---------------------------------------------------------------------------
#  Proving the guards.
#
#  For each unit, compile the suite once per GW_TEST_BREAK value, which
#  removes exactly one check from the code under test, and report which
#  checks then fail. A value that produces NO failures is a guard with no
#  test behind it, and the run says so loudly.
# ---------------------------------------------------------------------------
BREAK_CASES := \
	$(foreach v,$(BREAK_dns),dns:$(v)) \
	$(foreach v,$(BREAK_ring),ring:$(v)) \
	$(foreach v,$(BREAK_telnet),telnet:$(v)) \
	$(foreach v,$(BREAK_settings),settings:$(v))

# The expected number of cases, asserted, because the first version of this
# target built its list with `eval echo \$$BREAK_$$u` - a shell variable of
# that name, which does not exist - so the list was empty, the loop body
# never ran, and it printed "EVERY GUARD HAS A TEST" having tested none.
# Silence reading as success is the exact failure a proof like this exists
# to prevent.
BREAK_EXPECT := $(words $(BREAK_CASES))

break: $(TESTS)
	@echo "== Proving each guard by removing it: $(BREAK_EXPECT) cases =="
	@bad=0; seen=0; \
	for c in $(BREAK_CASES); do \
	  u=$${c%%:*}; v=$${c##*:}; seen=$$((seen+1)); \
	  extra=""; more=""; \
	  [ "$$u" = settings ] && extra="-DGW_BOARD_WROOM32"; \
	  [ "$$u" = telnet ] && more="$(SRC)/telnet.cpp"; \
	  [ "$$u" = ring ] && extra="$$extra -pthread"; \
	  if ! $(CXX) $(BREAKFLAGS) $$extra -DGW_TEST_BREAK=$$v \
	        -o brk_$$u test_$$u.cpp $$more >/dev/null 2>&1; then \
	    echo "  $$u break=$$v: DID NOT COMPILE"; bad=1; continue; \
	  fi; \
	  rc=0; timeout 60 ./brk_$$u > brk.log 2>&1 || rc=$$?; \
	  n=`grep -c '^   FAIL' brk.log || true`; \
	  if [ "$$rc" = 124 ]; then \
	    echo "  $$u break=$$v: it HANGS without the guard, as it must"; \
	    rm -f brk_$$u brk.log; continue; \
	  fi; \
	  if [ "$$n" -gt 0 ]; then \
	    echo "  $$u break=$$v: $$n checks fail, as they must"; \
	    grep '^   FAIL' brk.log | head -2 | sed 's/^/      /'; \
	    rm -f brk_$$u brk.log; continue; \
	  fi; \
	  rm -f brk_$$u brk.log; \
	  if ! $(CXX) $(BREAKFLAGS) $(SAN) $$extra -DGW_TEST_BREAK=$$v \
	        -o brk_$$u test_$$u.cpp $$more >/dev/null 2>&1; then \
	    echo "  $$u break=$$v: NO CHECK CAUGHT IT and ASan would not build"; \
	    bad=1; continue; \
	  fi; \
	  if timeout 60 setarch `uname -m` -R ./brk_$$u > brk.log 2>&1; then \
	    echo "  $$u break=$$v: NO CHECK CAUGHT IT - the guard is untested"; bad=1; \
	  else \
	    echo "  $$u break=$$v: no verdict changed, but ASan trips, as it must"; \
	    grep -m1 -E 'ERROR|runtime error' brk.log | sed 's/^/      /' || true; \
	  fi; \
	  rm -f brk_$$u brk.log; \
	done; \
	echo; \
	if [ "$$seen" -ne "$(BREAK_EXPECT)" ]; then \
	  echo "ran $$seen of $(BREAK_EXPECT) cases: the case list is broken"; exit 1; \
	fi; \
	if [ $$bad -eq 0 ]; then \
	  echo "EVERY GUARD HAS A TEST THAT CATCHES IT ($$seen cases)"; \
	else echo "A GUARD HAS NO TEST"; exit 1; fi

# AddressSanitizer is the point of this target, not a nicety: the DNS suite
# sweeps every single-octet corruption of a real query, and only ASan turns
# a read one past the end into a failure instead of a pass.
#
# The old GCC in WSL sometimes dies at startup under ASan with
# DEADLYSIGNAL, which is ASLR against a newer kernel rather than a bug
# here; `setarch -R` turns ASLR off, as the core's own host notes record.
asan:
	@for t in $(TESTS); do \
	  case $$t in test_settings) extra="-DGW_BOARD_WROOM32";; *) extra="";; esac; \
	  case $$t in test_telnet) more="$(SRC)/telnet.cpp";; *) more="";; esac; \
	  case $$t in test_ring) extra="$$extra -pthread";; esac; \
	  $(CXX) $(CXXFLAGS) $(SAN) $$extra -o san_$$t $$t.cpp $$more || exit 1; \
	  setarch $$(uname -m) -R ./san_$$t || exit 1; rm -f san_$$t; \
	done; \
	echo "ALL SUITES PASS UNDER ASAN AND UBSAN"

clean:
	rm -f $(TESTS) brk_* san_*

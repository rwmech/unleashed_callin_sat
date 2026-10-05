# ===========================================================================
#  µnleashed gateway sat
# ===========================================================================
#
# File:         firmware/tools/fetch_web.py
# Module:       Build / the browser terminal's third-party assets
#
# Purpose:      Fetches the packages web/xterm.lock pins from the npm
#               registry into firmware/.web-cache/ (ignored by git), refuses
#               any tarball whose SHA-256 is not the one the lock names, and
#               then runs mkweb.py to gzip every page into the app image.
#
#               A PlatformIO pre-script (platformio.ini extra_scripts), and
#               runnable alone:  python firmware/tools/fetch_web.py
#
#               A fetch rather than a vendored blob because a 290 KB MIT
#               file in the repository is a licence notice to maintain and a
#               diff nobody reads. The lock's hash is what makes the fetch
#               trustworthy; a mismatch is REFUSED rather than warned about,
#               because a tarball that is not the one the lock names is
#               either a compromised mirror or a deliberate bump and only
#               one of those should pass without somebody typing.
#
#               With no network AND no cache the build still goes through,
#               with a placeholder terminal and a loud #warning, so the
#               portal can be worked on offline. A release build
#               (-DGW_RELEASE) turns that warning into an #error.
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
import hashlib
import io
import os
import sys
import tarfile
import urllib.request

REGISTRY = "https://registry.npmjs.org"
TIMEOUT = 60


def here():
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        # PlatformIO's SCons exec()s this script, which leaves __file__
        # undefined; its working directory is the project root.
        return os.path.join(os.getcwd(), "tools")


def read_lock(path):
    """Blocks of key = value, one block per `package` line. # comments."""
    blocks = []
    cur = None
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            k = k.strip()
            v = v.strip()
            if k == "package":
                cur = {"package": v}
                blocks.append(cur)
            elif cur is not None:
                cur[k] = v
    for b in blocks:
        want = {}
        for pair in b.get("files", "").split(","):
            pair = pair.strip()
            if not pair or ":" not in pair:
                continue
            inside, name = pair.split(":", 1)
            want[inside.strip()] = name.strip()
        b["want"] = want
    return blocks


def fetch_one(cache, blk):
    pkg = blk["package"]
    ver = blk.get("version", "")
    want_hash = blk.get("sha256", "").lower()
    want = blk["want"]
    if not ver or not want:
        sys.stderr.write("fetch_web: %s has no version or no files\n" % pkg)
        return False

    # A stamp per package, holding what is cached. Without it a version
    # bump in the lock would be skipped as "already cached" and the build
    # would quietly keep the old bytes, which is the whole point of a lock.
    short_name = pkg.split("/")[-1]
    stamp = os.path.join(cache, ".stamp-" + short_name)
    want_stamp = "%s %s" % (ver, want_hash)
    have = ""
    if os.path.isfile(stamp):
        with open(stamp, "r", encoding="utf-8") as f:
            have = f.read().strip()
    if have == want_stamp and all(
            os.path.isfile(os.path.join(cache, n)) for n in want.values()):
        print("fetch_web: %s %s already cached" % (pkg, ver))
        return True

    short = pkg.split("/")[-1]
    url = "%s/%s/-/%s-%s.tgz" % (REGISTRY, pkg, short, ver)
    print("fetch_web: fetching %s %s" % (pkg, ver))
    try:
        blob = urllib.request.urlopen(url, timeout=TIMEOUT).read()
    except Exception as e:  # noqa: BLE001 - every failure is the same answer
        sys.stderr.write("fetch_web: cannot fetch %s (%s)\n" % (url, e))
        return False

    got = hashlib.sha256(blob).hexdigest()
    if want_hash and got != want_hash:
        sys.stderr.write(
            "fetch_web: REFUSED. %s %s hashed\n"
            "    %s\n"
            "  and web/xterm.lock wants\n"
            "    %s\n"
            "  If the bump is yours, paste the first line into the lock.\n"
            % (pkg, ver, got, want_hash)
        )
        return False
    if not want_hash:
        print("fetch_web: no hash in the lock; this tarball is sha256 %s" % got)

    os.makedirs(cache, exist_ok=True)
    try:
        with tarfile.open(fileobj=io.BytesIO(blob)) as t:
            for inside, name in want.items():
                m = t.getmember(inside)
                if not m.isfile():
                    raise ValueError("%s is not a file" % inside)
                data = t.extractfile(m).read()
                with open(os.path.join(cache, name), "wb") as f:
                    f.write(data)
                print("fetch_web:   %-14s %7d bytes" % (name, len(data)))
    except Exception as e:  # noqa: BLE001
        sys.stderr.write("fetch_web: cannot unpack %s (%s)\n" % (pkg, e))
        return False
    # The stamp is written HERE and nowhere else, which is what makes the
    # "already cached" shortcut above mean "cached at the version and hash
    # the lock names" rather than "there is a file of about the right
    # name". Without it the shortcut never fired (so every build
    # re-downloaded 290 KB) and, worse, a build with no network found the
    # old files present and shipped them with nothing checking they were
    # the pinned ones.
    with open(stamp, "w", encoding="utf-8") as f:
        f.write(want_stamp + "\n")
    return True


def main():
    fw = os.path.dirname(here())
    cache = os.path.join(fw, ".web-cache")
    blocks = read_lock(os.path.join(fw, "web", "xterm.lock"))
    ok = bool(blocks)
    for blk in blocks:
        if not fetch_one(cache, blk):
            ok = False
    if not ok:
        sys.stderr.write(
            "fetch_web: carrying on with the placeholder terminal. A release\n"
            "           build (-DGW_RELEASE) will refuse to compile.\n"
        )
    # The header is generated whether or not the fetch worked, and `ok` is
    # passed through rather than left for mkweb to guess at. A cache that
    # happens to hold files is not the same thing as a verified fetch: with
    # no network, mkweb would otherwise find the old files, mark the image
    # as carrying a real terminal and let a release build ship whatever
    # version was lying about.
    sys.path.insert(0, here())
    import mkweb  # noqa: E402 - deliberately late, after sys.path

    mkweb.build(fw, verified=ok)


main()

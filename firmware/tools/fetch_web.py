# ===========================================================================
#  µnleashed gateway sat
# ===========================================================================
#
# File:         firmware/tools/fetch_web.py
# Module:       Build / the browser terminal's third-party assets
#
# Purpose:      Fetches xterm.js at the version web/xterm.lock pins from the
#               npm registry into firmware/.web-cache/ (ignored by git),
#               checks the tarball's SHA-256 against the lock, and then runs
#               mkweb.py to gzip every page into the app image.
#
#               A PlatformIO pre-script (platformio.ini extra_scripts), and
#               runnable alone:  python firmware/tools/fetch_web.py
#
#               It is a fetch rather than a vendored blob because a 290 KB
#               MIT file in the repository is a licence notice to maintain
#               and a diff nobody reads. The lock's hash is what makes the
#               fetch trustworthy; a mismatch is refused and prints what it
#               got, so a deliberate bump is one paste.
#
#               With no network AND no cache the build still goes through,
#               with a placeholder terminal page and a loud warning, so that
#               somebody can work on the portal offline. A release env
#               (-DGW_RELEASE) refuses to compile against the placeholder:
#               see webassets/web_assets.h's own #error.
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

# What is taken out of the tarball: the path inside it, and the name it is
# cached under. Nothing else is extracted, so a package that grows a
# postinstall script or a binary cannot bring it in here.
WANT = {
    "package/lib/xterm.js": "xterm.js",
    "package/css/xterm.css": "xterm.css",
    "package/LICENSE": "xterm.LICENSE",
}


def here():
    return os.path.dirname(os.path.abspath(__file__))


def read_lock(path):
    """key = value lines, # comments. Returns a dict."""
    out = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def cached(cache, names):
    return all(os.path.isfile(os.path.join(cache, n)) for n in names)


def fetch(fw):
    lock_path = os.path.join(fw, "web", "xterm.lock")
    lock = read_lock(lock_path)
    pkg = lock.get("package", "@xterm/xterm")
    ver = lock.get("version", "")
    want_hash = lock.get("sha256", "").lower()
    if not ver:
        sys.stderr.write("fetch_web: web/xterm.lock names no version\n")
        return False

    cache = os.path.join(fw, ".web-cache", "%s-%s" % (pkg.split("/")[-1], ver))
    if cached(cache, WANT.values()):
        print("fetch_web: xterm %s already cached" % ver)
        return True

    # registry.npmjs.org/@scope/name/-/name-version.tgz
    short = pkg.split("/")[-1]
    url = "%s/%s/-/%s-%s.tgz" % (REGISTRY, pkg, short, ver)
    print("fetch_web: fetching %s %s" % (pkg, ver))
    try:
        blob = urllib.request.urlopen(url, timeout=TIMEOUT).read()
    except Exception as e:  # noqa: BLE001 - any failure is the same answer
        sys.stderr.write("fetch_web: cannot fetch %s (%s)\n" % (url, e))
        return False

    got = hashlib.sha256(blob).hexdigest()
    if want_hash and got != want_hash:
        # Refuse rather than warn. A tarball that is not the one the lock
        # names is either a compromised mirror or a deliberate bump, and
        # only one of those should pass without somebody typing.
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
            for inside, name in WANT.items():
                m = t.getmember(inside)
                if not m.isfile():
                    raise ValueError("%s is not a file" % inside)
                data = t.extractfile(m).read()
                with open(os.path.join(cache, name), "wb") as f:
                    f.write(data)
                print("fetch_web:   %s  %d bytes" % (name, len(data)))
    except Exception as e:  # noqa: BLE001
        sys.stderr.write("fetch_web: cannot unpack (%s)\n" % e)
        return False
    return True


def main():
    fw = os.path.dirname(here())
    ok = fetch(fw)
    if not ok:
        sys.stderr.write(
            "fetch_web: carrying on with the placeholder terminal. A release\n"
            "           build (-DGW_RELEASE) will refuse to compile.\n"
        )
    # Generate the header whether or not the fetch worked: mkweb.py writes a
    # placeholder when the cache is empty and marks the header as such.
    sys.path.insert(0, here())
    import mkweb  # noqa: E402 - deliberately late, after sys.path

    mkweb.build(fw)


main()

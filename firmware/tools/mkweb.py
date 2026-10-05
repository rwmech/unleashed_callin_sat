# ===========================================================================
#  µnleashed gateway sat
# ===========================================================================
#
# File:         firmware/tools/mkweb.py
# Module:       Build / the web payload that rides in the app image
#
# Purpose:      Gzips every page in firmware/web/, and the xterm.js assets
#               fetch_web.py cached, into firmware/src/webassets/ (ignored by
#               git), and writes the one header that names them.
#
#               The .gz files are embedded by CMake's EMBED_FILES rather than
#               turned into C arrays: 67 KB of gzip as a C source file is
#               400 KB of text that compiles slowly and that nobody can read
#               either way.
#
#               Why precompressed rather than compressed per request: there
#               is no built-in support in esp_http_server and there will not
#               be (IDF issue 18816 asked and was closed "Won't Do"), and
#               gzipping 290 KB on an ESP32 per page view would be absurd.
#               Brotli is not an option at all: browsers advertise `br` only
#               over HTTPS, and a captive portal cannot be HTTPS.
#
#               Runnable alone:  python firmware/tools/mkweb.py
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
import gzip
import os
import sys

# Each asset: the embedded file's base name (which decides its linker
# symbol), the URL it is served at, its media type, and where it comes from.
# "cache" means fetch_web.py put it in .web-cache; "web" means it is ours.
#
# Paths are short on purpose: every byte of a URL is a byte of the portal
# page, and the page is what every phone that joins fetches.
ASSETS = [
    ("xterm_js",      "/x.js",   "application/javascript", "cache", "xterm.js"),
    ("xterm_css",     "/x.css",  "text/css",               "cache", "xterm.css"),
    ("xterm_licence", "/licence", "text/plain",             "cache", "xterm.LICENSE"),
    ("term_html",     "/t",      "text/html",              "web",   "term.html"),
    ("term_js",       "/t.js",   "application/javascript", "web",   "term.js"),
    ("style_css",     "/s.css",  "text/css",               "web",   "style.css"),
]

# What goes in place of a missing xterm, so the portal can be worked on with
# no network. A release build refuses to compile against it.
STUB = {
    "xterm.js": b"window.Terminal=function(){throw new Error("
                b"'this gateway was built with no browser terminal');};\n",
    "xterm.css": b"/* placeholder: no xterm.css at build time */\n",
    "xterm.LICENSE": b"placeholder: xterm.js was not fetched at build time\n",
}

HEADER_TOP = """\
// ===========================================================================
//  µnleashed gateway sat: GENERATED, do not edit.
//
//  firmware/tools/mkweb.py writes this. The .gz files beside it are put in
//  the app image by CMake's EMBED_FILES, and these are the only names that
//  know what each one is served as.
//
//  Copyright 2026 - Robert Mech. SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
#pragma once
#include <cstdint>

"""


def read(path):
    with open(path, "rb") as f:
        return f.read()


def build(fw):
    web = os.path.join(fw, "web")
    out = os.path.join(fw, "src", "webassets")
    os.makedirs(out, exist_ok=True)

    # The newest cache for the version the lock names. fetch_web.py makes
    # exactly one directory per version, so take any that holds xterm.js.
    cache_root = os.path.join(fw, ".web-cache")
    cache = ""
    if os.path.isdir(cache_root):
        for d in sorted(os.listdir(cache_root)):
            p = os.path.join(cache_root, d)
            if os.path.isfile(os.path.join(p, "xterm.js")):
                cache = p
    stubbed = False

    rows = []
    total_raw = 0
    total_gz = 0
    for sym, url, mime, where, name in ASSETS:
        src = os.path.join(cache if where == "cache" else web, name)
        if os.path.isfile(src):
            data = read(src)
        elif name in STUB:
            data = STUB[name]
            stubbed = True
        else:
            sys.stderr.write("mkweb: missing %s\n" % src)
            return False
        # Level 9 and no timestamp, so an unchanged asset produces an
        # unchanged .gz and the build does not relink for nothing.
        gz = gzip.compress(data, compresslevel=9, mtime=0)
        with open(os.path.join(out, sym + ".gz"), "wb") as f:
            f.write(gz)
        rows.append((sym, url, mime, len(data), len(gz)))
        total_raw += len(data)
        total_gz += len(gz)

    with open(os.path.join(out, "web_assets.h"), "w", encoding="utf-8") as f:
        f.write(HEADER_TOP)
        if stubbed:
            f.write(
                "// The browser terminal was NOT fetched when this was built.\n"
                "#define GW_WEB_STUB 1\n"
                "#ifdef GW_RELEASE\n"
                '#error "A release build needs the real browser terminal. '
                'Run tools/fetch_web.py with a network and build again."\n'
                "#endif\n"
                "#warning \"No browser terminal in this image: "
                "tools/fetch_web.py could not fetch xterm.js.\"\n\n"
            )
        else:
            f.write("#define GW_WEB_STUB 0\n\n")

        f.write("struct WebAsset {\n"
                "    const char*    path;     // the URL it is served at\n"
                "    const char*    type;     // its media type\n"
                "    const uint8_t* gz;       // gzip, served with "
                "Content-Encoding: gzip\n"
                "    const uint8_t* gzEnd;\n"
                "    uint32_t       raw;      // what it was before gzip, for the console\n"
                "};\n\n")
        for sym, _u, _m, _r, _g in rows:
            f.write('extern const uint8_t _binary_%s_gz_start[] asm("_binary_%s_gz_start");\n'
                    % (sym, sym))
            f.write('extern const uint8_t _binary_%s_gz_end[] asm("_binary_%s_gz_end");\n'
                    % (sym, sym))
        f.write("\n// constexpr, so it is this translation unit's own copy and "
                "there is no\n// linkage question; only web.cpp includes this.\n")
        f.write("constexpr WebAsset kWebAssets[] = {\n")
        for sym, url, mime, raw, _g in rows:
            f.write('    { "%s", "%s", _binary_%s_gz_start, _binary_%s_gz_end, %d },\n'
                    % (url, mime, sym, sym, raw))
        f.write("};\n")
        f.write("constexpr unsigned kWebAssetCount = %d;\n\n" % len(rows))
        f.write("// What the whole payload costs the app image, measured at build:\n")
        f.write("//   raw %d bytes, gzipped %d bytes\n" % (total_raw, total_gz))
        f.write("#define GW_WEB_GZ_BYTES %d\n" % total_gz)

    # The CMake side needs the same list, and generating it is what stops
    # the two drifting the day somebody adds a page.
    with open(os.path.join(out, "web_assets.cmake"), "w", encoding="utf-8") as f:
        f.write("# GENERATED by firmware/tools/mkweb.py. Do not edit.\n")
        f.write("# Copyright 2026 - Robert Mech. SPDX-License-Identifier: "
                "GPL-3.0-or-later\n")
        f.write("set(GW_WEB_EMBED\n")
        for sym, _u, _m, _r, _g in rows:
            f.write("    webassets/%s.gz\n" % sym)
        f.write(")\n")

    print("mkweb: %d assets, %d bytes raw, %d gzipped%s"
          % (len(rows), total_raw, total_gz, " (PLACEHOLDER terminal)" if stubbed else ""))
    for sym, url, _m, raw, gz in rows:
        print("mkweb:   %-10s %-9s %7d -> %6d" % (url, sym, raw, gz))
    return True


if __name__ == "__main__":
    build(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

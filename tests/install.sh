#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# make install into a scratch root: the files a plugin author and a user
# rely on are there, pkg-config reads the SDK, the installed daemon agrees
# with it, -L lists the plugins, and uninstall leaves nothing behind.
# Usage: sh tests/install.sh <build dir>
B=${1:-build}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "install: FAIL: $*"; exit 1; }

make -s install DESTDIR="$T" >/dev/null || fail "make install"
P="$T/usr/local"
for f in bin/mediamaster bin/mmctl bin/mm-update include/mediamaster/mm_plugin.h lib/pkgconfig/mediamaster.pc; do
	[ -f "$P/$f" ] || fail "missing $f"
done
for p in null raw ffmpeg library; do
	[ ! -e "$P/lib/mediamaster/plugins/$p.so" ] || fail "$p is built in; its .so should not be installed"
done
[ -f "$P/lib/mediamaster/plugins/mpris.so" ] || [ ! -f "$B/plugins/mpris.so" ] || fail "mpris.so not installed"

abi=$(sed -n 's/^#define MM_ABI \([0-9]*\).*/\1/p' include/mm_plugin.h)
# Only the pkg-config calls look in the scratch root; make uninstall below
# must still see the system's libraries.
pc() { PKG_CONFIG_LIBDIR="$P/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR="$T" pkg-config "$@"; }
[ "$(pc --variable=abi mediamaster)" = "$abi" ] || fail "pkg-config abi: $(pc --variable=abi mediamaster)"
[ "$(pc --variable=plugindir mediamaster)" = "$T/usr/local/lib/mediamaster/plugins" ] || fail "pkg-config plugindir: $(pc --variable=plugindir mediamaster)"
pc --cflags mediamaster | grep -q "^-I$T/usr/local/include/mediamaster *$" || fail "pkg-config cflags: $(pc --cflags mediamaster)"
"$P/bin/mediamaster" -V | grep -qx "mediamaster [0-9.]* abi $abi" || fail "-V: $("$P/bin/mediamaster" -V)"

# A plugin built the way a third party would, against the installed SDK.
cat >"$T/hello.c" <<'C'
#include "mm_plugin.h"
static int init(const struct mm_host *h, void **priv) { (void)h; *priv = NULL; return 0; }
static const struct mm_plugin p = { .abi = MM_ABI, .name = "hello", .version = "1.0", .init = init };
MM_PLUGIN_DEFINE(hello) { return &p; }
C
cc -std=c99 -pedantic -Wall -fPIC -shared $(pc --cflags mediamaster) "$T/hello.c" -o "$T/hello.so" ||
	fail "building against the SDK"
"$P/bin/mediamaster" -L -o "host.plugin_dir=$T" -o host.builtins=0 2>/dev/null | grep -q "^hello	1.0	$abi	ok	$T/hello.so" ||
	fail "-L: $("$P/bin/mediamaster" -L -o "host.plugin_dir=$T" -o host.builtins=0 2>&1)"

make -s uninstall DESTDIR="$T" >/dev/null || fail "make uninstall"
left=$(find "$T/usr" -type f 2>/dev/null)
[ -z "$left" ] || fail "left behind: $left"
echo "install: all checks passed"

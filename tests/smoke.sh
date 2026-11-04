#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# End-to-end: real daemon, real mmctl, temp config and socket.
# Usage: tests/smoke.sh <build-dir>
set -eu

B=${1:-build}
T=$(mktemp -d /tmp/mm-smoke.XXXXXX)
PID=
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM

fail() { echo "FAIL smoke: $*" >&2; [ -f "$T/log" ] && sed 's/^/  log| /' "$T/log" >&2; exit 1; }

export MM_SOCK="$T/mm.sock"
MMCTL="$B/mmctl"

start() { # extra -o args
	"$B/mediamaster" -c "$T/config" -o "host.socket=$MM_SOCK" "$@" 2>>"$T/log" &
	PID=$!
	i=0
	until "$MMCTL" version >/dev/null 2>&1; do
		i=$((i + 1)); [ $i -lt 50 ] || fail "daemon did not answer"
		kill -0 "$PID" 2>/dev/null || fail "daemon exited during startup"
		sleep 0.1
	done
}

stop() {
	"$MMCTL" quit | grep -q '^ok bye' || fail "quit not acknowledged"
	wait "$PID" || fail "daemon exit status $?"
	PID=
	[ ! -e "$MM_SOCK" ] || fail "socket left behind"
}

cat >"$T/config" <<EOF
host.plugin_dir = $B/plugins
null.greeting = hello from config
EOF

# --- builtins on: builtin null wins, null.so is skipped as a duplicate
start
"$MMCTL" plugins | grep -q '^null .*active.*builtin' || fail "builtin null not active"
"$MMCTL" null a 'b c' | grep -qx 'ok null: hello from config (ping 1, 2 args)' || fail "null verb / config slice"
if "$MMCTL" nosuch >/dev/null 2>&1; then fail "unknown verb should exit 1"; fi
"$MMCTL" help | grep -q 'null \[args...\]' || fail "help lists plugin verbs"
grep -q "already provided by builtin" "$T/log" || fail "duplicate .so not reported"

# reload picks up a config edit
sed -i 's/hello from config/edited/' "$T/config"
"$MMCTL" reload | grep -q '^ok reload queued' || fail "reload not acknowledged"
sleep 0.2
"$MMCTL" null | grep -q 'edited' || fail "reload did not take effect"

# a second daemon on the same socket must refuse, not steal it
if "$B/mediamaster" -c "$T/config" -o "host.socket=$MM_SOCK" 2>>"$T/log"; then fail "second daemon started"; fi
"$MMCTL" version >/dev/null || fail "first daemon lost its socket"
stop

# --- builtins off: null comes from the shared object
start -o host.builtins=0
"$MMCTL" plugins | grep -q "^null .*active.*null.so" || fail "null.so not loaded from plugin path"
stop

# --- disabled
start -o host.disable=null
"$MMCTL" plugins | grep -q '^null .*disabled' || fail "host.disable ignored"
if "$MMCTL" null >/dev/null 2>&1; then fail "disabled plugin answered"; fi
stop

# --- SIGTERM shuts down cleanly
start
kill -TERM "$PID"
wait "$PID" || fail "SIGTERM exit status $?"
PID=
[ ! -e "$MM_SOCK" ] || fail "socket left behind after SIGTERM"

echo "smoke: all checks passed"

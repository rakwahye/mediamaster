#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# End-to-end: real daemon, real mmctl, null sink, temp config and socket.
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

field() { "$MMCTL" status | sed -n "s/^$1: //p"; }

wait_state() { # state [tries of 0.1 s]
	i=0
	until [ "$(field state)" = "$1" ]; do
		i=$((i + 1)); [ $i -lt "${2:-50}" ] || fail "state never became $1 (is $(field state))"
		sleep 0.1
	done
}

printf 'host.plugin_dir = %s/plugins\nhost.sink = null\n' "$B" >"$T/config"

# --- builtins: null sink and raw source, shared objects skipped as duplicates
start
"$MMCTL" plugins | grep -q '^null .*active.*ifaces=sink.*builtin' || fail "builtin null sink not active"
"$MMCTL" plugins | grep -q '^raw .*active.*ifaces=source.*builtin' || fail "builtin raw source not active"
[ "$(field sink)" = null ] || fail "host.sink not honoured"
if "$MMCTL" nosuch >/dev/null 2>&1; then fail "unknown verb should exit 1"; fi
"$MMCTL" help | grep -q 'play <uri>' || fail "help lists playback verbs"
grep -q "already provided by builtin" "$T/log" || fail "duplicate .so not reported"

# a half-second tone, played to the end through the daemon
"$MMCTL" null reset >/dev/null
"$MMCTL" play sine:440:0.5 | grep -q '^ok playing sine:440:0.5' || fail "play not acknowledged"
[ "$(field state)" = playing ] || fail "not playing"
wait_state stopped 30
loud=$("$MMCTL" null | sed -n 's/.*nonsilent=\([0-9]*\).*/\1/p')
[ "$loud" -ge 23000 ] && [ "$loud" -le 24000 ] || fail "expected ~24000 audible frames, got $loud"
[ "$(field underruns)" = 0 ] || fail "underruns during a short tone"

# controls on an endless tone
"$MMCTL" play sine:440 >/dev/null || fail "endless play"
"$MMCTL" pause | grep -q '^ok paused' || fail "pause"
[ "$(field state)" = paused ] || fail "state after pause"
"$MMCTL" toggle | grep -q '^ok playing' || fail "toggle"
"$MMCTL" vol 30 | grep -qx 'ok 30' || fail "vol set"
"$MMCTL" vol +5 | grep -qx 'ok 35' || fail "vol relative"
if "$MMCTL" seek 10 >/dev/null 2>&1; then fail "seek on a live stream should fail"; fi
if "$MMCTL" play nosuch:x >/dev/null 2>&1; then fail "unknown scheme should fail"; fi
[ "$(field state)" = playing ] || fail "a refused play disturbed the current stream"
"$MMCTL" stop >/dev/null
[ "$(field state)" = stopped ] || fail "stop"

# seek on a finite tone
"$MMCTL" play sine:440:30 >/dev/null || fail "finite play"
"$MMCTL" seek 20 | grep -q '^ok' || fail "seek"
sleep 0.2
pos=$(field position)
[ "${pos%%.*}" -ge 20 ] || fail "position after seek is $pos"
"$MMCTL" stop >/dev/null

# reload is acknowledged and performed
"$MMCTL" reload | grep -q '^ok reload queued' || fail "reload not acknowledged"
sleep 0.2
grep -q "reloading" "$T/log" || fail "reload not performed"

# a second daemon on the same socket must refuse, not steal it
if "$B/mediamaster" -c "$T/config" -o "host.socket=$MM_SOCK" 2>>"$T/log"; then fail "second daemon started"; fi
"$MMCTL" version >/dev/null || fail "first daemon lost its socket"
stop

# --- builtins off: everything from shared objects
start -o host.builtins=0
"$MMCTL" plugins | grep -q "^null .*active.*null.so" || fail "null.so not loaded from plugin path"
"$MMCTL" play sine:440:0.2 >/dev/null || fail "play via shared-object plugins"
wait_state stopped 20
stop

# --- a disabled source
start -o host.disable=raw
"$MMCTL" plugins | grep -q '^raw .*disabled' || fail "host.disable ignored"
if "$MMCTL" play sine:440 >/dev/null 2>&1; then fail "disabled source played"; fi
stop

# --- no usable sink: the daemon refuses to start
if "$B/mediamaster" -c "$T/config" -o "host.socket=$MM_SOCK" -o host.disable=null 2>>"$T/log"; then
	fail "started without a sink"
fi

# --- SIGTERM while playing shuts down cleanly
start
"$MMCTL" play sine:440 >/dev/null
kill -TERM "$PID"
wait "$PID" || fail "SIGTERM exit status $?"
PID=
[ ! -e "$MM_SOCK" ] || fail "socket left behind after SIGTERM"

echo "smoke: all checks passed"

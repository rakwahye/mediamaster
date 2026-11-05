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
"$MMCTL" help | grep -q 'play \[uri\]' || fail "help lists playback verbs"
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

# a real file through ffmpeg, then the same file over http
if command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null; then
	ffmpeg -v error -f lavfi -i sine=f=440:d=1 -ac 2 -metadata title="Tone One" -metadata artist="Osc" \
	       -metadata track=3/12 "$T/tone one.flac" -y || fail "fixture"
	"$MMCTL" null reset >/dev/null
	"$MMCTL" play "$T/tone one.flac" | grep -q '^ok playing' || fail "play a file by path"
	[ "$(field source)" = ffmpeg ] || fail "a bare path is not routed to ffmpeg"
	[ "$(field title)" = "Tone One" ] && [ "$(field artist)" = Osc ] && [ "$(field tracknumber)" = 3 ] ||
		fail "file tags not in status: $("$MMCTL" status | tr '\n' ' ')"
	[ "$(field seekable)" = yes ] || fail "a file should be seekable"
	case "$(field duration)" in 0.99*|1.00*) ;; *) fail "duration $(field duration)";; esac
	wait_state stopped 40
	loud=$("$MMCTL" null | sed -n 's/.*nonsilent=\([0-9]*\).*/\1/p')
	[ "$loud" -ge 47000 ] || fail "expected ~48000 audible frames from the file, got $loud"

	if command -v python3 >/dev/null; then
		port=$(python3 -I -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
		python3 -I -m http.server "$port" --bind 127.0.0.1 --directory "$T" >/dev/null 2>&1 &
		HTTP=$!
		i=0
		until python3 -I -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:$port/')" 2>/dev/null; do
			i=$((i + 1)); [ $i -lt 50 ] || { kill $HTTP; fail "http server did not start"; }
			sleep 0.1
		done
		"$MMCTL" null reset >/dev/null
		"$MMCTL" play "http://127.0.0.1:$port/tone%20one.flac" | grep -q '^ok playing' || { kill $HTTP; fail "play over http"; }
		[ "$(field source)" = ffmpeg ] || { kill $HTTP; fail "http not routed to ffmpeg"; }
		wait_state stopped 40
		kill $HTTP
		loud=$("$MMCTL" null | sed -n 's/.*nonsilent=\([0-9]*\).*/\1/p')
		[ "$loud" -ge 47000 ] || fail "expected ~48000 audible frames over http, got $loud"
	fi
	if "$MMCTL" play "$T/nope.flac" >/dev/null 2>&1; then fail "a missing file should be refused"; fi
fi

# the queue: two tones play back to back, then playback stops
"$MMCTL" queue clear >/dev/null
"$MMCTL" queue add sine:220:0.3 sine:440:1 sine:660:0.3 | grep -qx 'ok added 1-3' || fail "queue add"
"$MMCTL" queue | grep -q '^  3  sine:660:0.3$' || fail "queue list"
"$MMCTL" queue rm 1 >/dev/null || fail "queue rm"
"$MMCTL" null reset >/dev/null
"$MMCTL" play | grep -q '^ok playing sine:440:1' || fail "play starts the queue"
i=0
until [ "$(field next)" = sine:660:0.3 ]; do
	i=$((i + 1)); [ $i -lt 8 ] || fail "the second tone was not preloaded"
	sleep 0.05
done
wait_state stopped 40
"$MMCTL" queue | grep -q '^> 2  sine:660:0.3$' || fail "the queue did not advance to the second tone"
[ "$(field underruns)" = 0 ] || fail "gapless hand-off underran"
loud=$("$MMCTL" null | sed -n 's/.*nonsilent=\([0-9]*\).*/\1/p')
[ "$loud" -ge 61000 ] || fail "expected ~62400 audible frames from two tones, got $loud"
if "$MMCTL" next >/dev/null 2>&1; then fail "next past the end should fail"; fi
"$MMCTL" prev | grep -q '^ok playing 1: sine:440:1' || fail "prev"
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

#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The PipeWire sink and its server coming and going: a private PipeWire,
# WirePlumber and virtual sink in their own runtime dir and session bus.
# First the daemon starts before the server: it falls back to null, plays
# there, and moves to PipeWire by itself once the server is up. Then the
# server is killed and started again twice: position must hold while it
# is away and move again once it is back, and quitting while it is away
# must not hang.
#
# Not part of `make test`: a private WirePlumber may reach for the real
# sound card. Run it with `make test-pipewire` (SAN= as usual).
# Usage: tests/pipewire.sh <build-dir>
set -eu

B=${1:-build}
fail() { echo "pipewire: FAIL: $*" >&2; [ -f "$T/mm.log" ] && tail -n 20 "$T/mm.log" >&2; exit 1; }
for c in pipewire wireplumber pw-cli dbus-run-session; do
	command -v $c >/dev/null || { echo "pipewire: skipped (no $c)"; exit 0; }
done
[ -f "$B/plugins/pipewire.so" ] || { echo "pipewire: skipped (built without PipeWire)"; exit 0; }

T=$(mktemp -d)
export XDG_RUNTIME_DIR="$T" MM_SOCK="$T/mm.sock"
chmod 700 "$T"
DPID=
cleanup() {
	[ -n "$DPID" ] && kill "$DPID" 2>/dev/null
	down
	rm -rf "$T"
}
up() {
	setsid dbus-run-session -- sh -c 'pipewire & sleep 1; wireplumber & sleep 1
	  pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=mm-test-sink
	    media.class=Audio/Sink object.linger=true audio.position=[FL FR] }" >/dev/null; wait' \
		>>"$T/pw.log" 2>&1 &
	echo $! >"$T/pw.pid"
	sleep 3
}
down() {
	[ -f "$T/pw.pid" ] || return 0
	pkill -9 -s "$(cat "$T/pw.pid")" 2>/dev/null || true
	rm -f "$T/pw.pid"
	sleep 1
}
trap cleanup EXIT INT TERM

pos() { "$B/mmctl" status | sed -n 's/^position: \([0-9]*\).*/\1/p'; }
moving() { a=$(pos); sleep 1.5; b=$(pos); [ -n "$a" ] && [ -n "$b" ] && [ "$b" -gt "$a" ]; }

sink() { "$B/mmctl" status | sed -n 's/^sink: //p'; }

# --- the server comes up after the daemon
HOME="$T" "$B/mediamaster" -c /dev/null -o "host.plugin_dir=$B/plugins" -o "host.sink=pipewire, null" \
	-o host.sink_retry_s=1 -o "host.socket=$MM_SOCK" 2>"$T/mm.log" &
DPID=$!
i=0; until [ -S "$MM_SOCK" ]; do i=$((i + 1)); [ $i -lt 50 ] || fail "daemon did not start"; sleep 0.1; done
[ "$(sink)" = null ] || fail "no server at start: expected the null fallback, got $(sink)"
"$B/mmctl" play sine:440:600 >/dev/null || fail "play"
sleep 1
moving || fail "not playing on the fallback"
sleep 2
[ "$(sink)" = null ] || fail "left the fallback with no server"
[ "$(grep -c 'no PipeWire output' "$T/mm.log")" -le 1 ] || fail "every retry warns"
up
i=0; until [ "$(sink)" = pipewire ]; do i=$((i + 1)); [ $i -lt 30 ] || fail "did not move to PipeWire once it was up"; sleep 0.2; done
grep -q 'sink: pipewire (was null)' "$T/mm.log" || fail "the move is not in the log"
moving || fail "not playing into PipeWire after the move"

for round in 1 2; do
	down
	held=$(pos); sleep 2
	[ "$(pos)" = "$held" ] || fail "round $round: position moved with no server ($held, $(pos))"
	up
	i=0; until moving; do i=$((i + 1)); [ $i -lt 8 ] || fail "round $round: playback did not come back"; done
done
[ "$(grep -c 'PipeWire output back' "$T/mm.log")" -ge 2 ] || fail "no 'back' in the log"

down
sleep 1
"$B/mmctl" quit >/dev/null
i=0; while kill -0 "$DPID" 2>/dev/null; do i=$((i + 1)); [ $i -lt 50 ] || fail "quit while away hung"; sleep 0.1; done
wait "$DPID" || fail "daemon exit $?"
DPID=
grep -q 'ThreadSanitizer\|AddressSanitizer' "$T/mm.log" && fail "sanitizer report"
echo "pipewire: all checks passed"

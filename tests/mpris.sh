#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# MPRIS against real playerctl on a private session bus.
# Usage: tests/mpris.sh <build-dir>   (re-executes itself under dbus-run-session)
set -eu

B=${1:-build}
if [ ! -e "$B/plugins/mpris.so" ]; then echo "mpris: SKIP (built without sd-bus)"; exit 0; fi
for t in playerctl dbus-run-session dbus-monitor stdbuf; do
	command -v $t >/dev/null || { echo "mpris: SKIP ($t not installed)"; exit 0; }
done
if [ -z "${MM_MPRIS_INNER:-}" ]; then
	st=$(mktemp)
	{ r=0; MM_MPRIS_INNER=1 dbus-run-session -- sh "$0" "$@" 2>&1 || r=$?; echo $r >"$st"; } | grep -v '^dbus-daemon\['
	rc=$(cat "$st"); rm -f "$st"
	exit "$rc"
fi

T=$(mktemp -d /tmp/mm-mpris.XXXXXX)
PID=
MON=
cleanup() {
	[ -n "$MON" ] && kill "$MON" 2>/dev/null
	[ -n "$PID" ] && kill "$PID" 2>/dev/null
	rm -rf "$T"
}
trap cleanup EXIT INT TERM
fail() { echo "FAIL mpris: $*" >&2; sed 's/^/  log| /' "$T/log" >&2; exit 1; }

export MM_SOCK="$T/mm.sock"
M="$B/mmctl"
P="playerctl -p mediamaster"

"$B/mediamaster" -c /dev/null -o "host.socket=$MM_SOCK" -o "host.plugin_dir=$B/plugins" \
	-o host.sink=null -o host.builtins=1 -o library.index=off 2>"$T/log" &
PID=$!
i=0
until playerctl -l 2>/dev/null | grep -qx mediamaster; do
	i=$((i + 1)); [ $i -lt 50 ] || fail "mediamaster never appeared on the bus"
	sleep 0.1
done

stdbuf -oL dbus-monitor "type='signal',path='/org/mpris/MediaPlayer2'" >"$T/signals" 2>/dev/null &
MON=$!
sleep 0.2

[ "$($P status)" = Stopped ] || fail "initial status"
$P play 2>/dev/null || true # nothing queued: must not break anything
[ "$($P status)" = Stopped ] || fail "play on an empty queue"

"$M" queue add sine:440:20 sine:660:20 >/dev/null
$P play
sleep 0.2
[ "$($P status)" = Playing ] || fail "play"
[ "$($P metadata xesam:url)" = sine:440:20 ] || fail "metadata url"
[ "$($P metadata mpris:length)" = 20000000 ] || fail "metadata length"
$P metadata mpris:trackid | grep -q "^'*/org/mediamaster/track/[0-9]*'*$" || fail "trackid"

$P volume 0.25
sleep 0.1
[ "$("$M" vol)" = "ok 25" ] || fail "setting Volume reaches the player"
"$M" vol 80 >/dev/null
sleep 0.2
[ "$($P volume)" = 0.800000 ] || fail "player volume reads back over MPRIS"

$P position 5
sleep 0.1
case "$($P position)" in 5.*) ;; *) fail "SetPosition: $($P position)";; esac
$P position 3-
sleep 0.1
case "$($P position)" in 2.*) ;; *) fail "relative Seek: $($P position)";; esac

$P play-pause; sleep 0.1
[ "$($P status)" = Paused ] || fail "play-pause pauses"
$P play-pause; sleep 0.1
[ "$($P status)" = Playing ] || fail "play-pause resumes"

$P next; sleep 0.2
[ "$($P metadata xesam:url)" = sine:660:20 ] || fail "next"
$P previous; sleep 0.2
[ "$($P metadata xesam:url)" = sine:440:20 ] || fail "previous"

$P stop; sleep 0.1
[ "$($P status)" = Stopped ] || fail "stop"
$P play-pause; sleep 0.2
[ "$($P status)" = Playing ] || fail "play-pause from stopped starts playback"

$P open "sine:880:30"; sleep 0.2
[ "$($P metadata xesam:url)" = sine:880:30 ] || fail "OpenUri"
if $P open "nosuch:thing" 2>/dev/null; then fail "OpenUri of an unplayable URI should fail"; fi

# Signals: a gapless track change emits Metadata; a seek emits Seeked.
"$M" queue clear >/dev/null
"$M" queue add sine:300:0.4 sine:500:0.4 >/dev/null
"$M" queue play 1 >/dev/null
i=0
until [ "$($P status)" = Stopped ]; do
	i=$((i + 1)); [ $i -lt 50 ] || fail "two short tones should have finished"
	sleep 0.1
done
grep -q 'string "sine:500:0.4"' "$T/signals" || fail "no PropertiesChanged with the second tone's metadata"
"$M" play sine:440:30 >/dev/null
sleep 0.2
$P position 10
i=0
until grep -A1 'member=Seeked' "$T/signals" | grep -q 'int64 10'; do
	i=$((i + 1)); [ $i -lt 40 ] || fail "no Seeked signal at 10 s"
	sleep 0.1
done

# Tags from the source: title, artists, album, track, cover.
if command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null; then
	ffmpeg -v error -f lavfi -i sine=f=440:d=5 -ac 2 -metadata title="Tone One" -metadata artist="Osc" \
	       -metadata album="Tests" -metadata track=3/12 "$T/tagged.flac" -y || fail "fixture"
	"$M" play "$T/tagged.flac" >/dev/null || fail "play a tagged file"
	sleep 0.3
	[ "$($P metadata xesam:title)" = "Tone One" ] || fail "title: $($P metadata xesam:title)"
	[ "$($P metadata xesam:artist)" = Osc ] || fail "artist: $($P metadata xesam:artist)"
	[ "$($P metadata xesam:album)" = Tests ] || fail "album"
	[ "$($P metadata xesam:trackNumber)" = 3 ] || fail "track number"
fi
"$M" play sine:440:30 >/dev/null
sleep 0.2
[ "$($P metadata xesam:title)" = "sine:440:30" ] || fail "untagged falls back to the name: $($P metadata xesam:title)"

$P stop
"$M" plugins | grep -q '^mpris .*active' || fail "mpris not active"
"$M" quit >/dev/null
wait "$PID" || fail "daemon exit status"
PID=
echo "mpris: all checks passed"

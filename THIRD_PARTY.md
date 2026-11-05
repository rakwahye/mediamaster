<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->
# Third-party code and services

Every external component mediamaster links, vendors, spawns or calls, with its
license and where it entered the tree. Update this file in the same commit that
adds a dependency. `make check-licenses` enforces an SPDX line on our own files.

Project license: LGPL-2.1-or-later (see LICENSE).

## Linked

| Component | License | Since | Used by |
|---|---|---|---|
| glibc (libc, libdl, libpthread, libm) | LGPL-2.1-or-later | initial | host, plugins |
| libpipewire-0.3 | MIT | ring/sink commit | `pipewire.so` (optional; built when headers are present) |
| sd-bus from libsystemd, or basu (its standalone extraction) | LGPL-2.1-or-later (libsystemd), LGPL-2.1 (basu) | mpris commit | `mpris.so` (optional). Chosen over libdbus, whose AFL-2.1/GPL-2.0+ dual licence is awkward to combine with LGPL code |

## Vendored

None yet.

## Spawned

Run as separate processes, never linked or redistributed. Their license
does not combine with ours; the user's installed build applies.

| Program | License | Since | Used by |
|---|---|---|---|
| ffmpeg, ffprobe | LGPL-2.1-or-later, or GPL-2.0-or-later when built with `--enable-gpl` (most distribution builds) | ffmpeg source commit | `ffmpeg` source plugin |

## Test tools

Used by the test suite only; not linked or shipped: playerctl,
dbus-run-session, dbus-monitor, ffmpeg (fixtures), python3 (http.server).

## Read for design, not copied

| Project | License | What we took |
|---|---|---|
| rakwahye/projectm-daemon | LGPL-2.1-or-later | IPC wire format (NUL-framed argv) — reimplemented |
| MPRIS D-Bus Interface Specification (freedesktop.org) | specification | implemented from the spec text |

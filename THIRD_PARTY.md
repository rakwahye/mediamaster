<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->
# Third-party code and services

Every external component mediamaster links, vendors, spawns or calls, with its
license and where it entered the tree. Update this file in the same commit that
adds a dependency. `make check-licenses` enforces an SPDX line on our own files.

Project license: LGPL-2.1-or-later (see LICENSE).

## Linked

| Component | License | Since | Used by |
|---|---|---|---|
| glibc (libc, libdl, libpthread) | LGPL-2.1-or-later | initial | host |

## Vendored

None yet.

## Spawned

None yet.

## Read for design, not copied

| Project | License | What we took |
|---|---|---|
| rakwahye/projectm-daemon | LGPL-2.1-or-later | IPC wire format (NUL-framed argv) — reimplemented |

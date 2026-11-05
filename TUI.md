# mediamaster — TUI plan v0.1

`mmtui`: a Notcurses front end for the mediamaster daemon. One screen, a handful of
keys, and bling where bling costs nothing. It is a client, exactly like `mmctl`: it
talks the IPC socket and knows nothing a plugin knows. KISS first, then incremental
glow, each step one commit.

Status 2026-11-05: plan only. Nothing built.

---

## 1. Where it lives

Own repo, `rakwahye/mediamaster-tui`, on the `mediamaster-spotify` pattern: a
`mediamaster` submodule pinned to a commit (the daemon for `make test`, nothing
else — a client needs no ABI header), its own `Makefile`, `THIRD_PARTY.md`,
`tests/`. Binary `mmtui`, prefix `tui_`. `make install` drops it next to `mmctl`.

Why not `src/mmtui/` in the main repo: the main repo has no external
dependencies beyond pkg-config'd system libs that are optional at build time;
Notcurses is a hard dependency of the TUI and would become an optional one of
the host tree. Out of tree keeps the host clean and the TUI pinned to a host
revision it was tested against.

```
mediamaster-tui/
  Makefile  LICENSE  THIRD_PARTY.md  README.md
  mediamaster/            submodule, pinned
  src/
    main.c                args, loop, tick
    ipc.[ch]              connect, frame, reply; typed readers for status/queue/items
    model.[ch]            what the screen shows: status struct, queue, item list, message
    ui.[ch]               planes, layout, redraw
    theme.[ch]            palette, gradients, glyphs
    keys.[ch]             key → command
  tests/
    check.h               copied from mediamaster/tests (same macros)
    ipc_test.c            parse fixtures: status, queue, browse lines
    fakes/mmd.sh          a `socat` daemon that answers from fixture files
    smoke.sh              start the pinned daemon with host.sink=null, drive mmtui headless
```

Build: `-std=c11` (it is host-side tooling, like `mmctl`), same `WARN` set as the
host Makefile, `SAN=asan|tsan` kept. `pkg-config notcurses-core` only — the full
`notcurses` (ffmpeg/OIIO image backend) is optional and only for cover art (§6).

License: LGPL-2.1-or-later like the host. Notcurses is Apache-2.0 (+MIT bits),
dynamically linked; compatible with LGPL-2.1-**or-later** via the LGPLv3 option.
One row in `THIRD_PARTY.md`.

---

## 2. Shape

```
 mmtui ──unix sock──► daemon            one connection per command, like mmctl
   │
   ├── tick (250 ms): `status` → model → redraw if changed
   ├── on demand:     `queue list`, `search <q>`, `browse <uri>`
   └── keys:          `toggle` `next` `prev` `seek ±5` `vol ±5` `stop` `play <uri>` `queue …`
```

No threads in v0.1: `notcurses_get()` with a timeout *is* the tick. A command
that may block on the network (`search`, `browse` over Spotify) runs on one worker
thread from commit 7 on, posting its lines back through a pipe the main loop
polls — same shape as the mpris plugin's eventfd hand-off, so there is one way
things cross threads.

Everything the screen shows comes from the daemon's text replies. No local
state that the daemon does not have, except cursor positions and the input line.

### 2.1 IPC (what the daemon speaks today)

| Verb | Reply | Parsed into |
|---|---|---|
| `status` | `key: value\n` lines: state, item, uri, source, tags (`title` `artist` `album` `art` …), position (s, float), duration, seekable, next, volume, sink, buffered, underruns | `struct tui_status` |
| `queue list` | `> n  uri` / `  n  uri`, or `queue is empty` | `struct tui_list` |
| `search <w>` / `browse <uri>` | `kind\turi\ttitle\tartist\talbum\tm:ss\n`, `-` for empty | `struct tui_list` |
| others | `ok …` / `err …` | message line |

A reply the parser does not understand becomes one message-line error and the
screen keeps the last good model. Never crash on daemon output.

---

## 3. Screen

One layout, three planes stacked, nothing floating until the help overlay:

```
┌ mediamaster ──────────────────────────────── vol ▮▮▮▮▮▮▮▮▯▯ 80 ┐
│  ▶  Pink Floyd — Time                                          │
│     The Dark Side of the Moon · spotify                        │
│  1:23 ━━━━━━━━━━━━━━━━━━━●───────────────────────────── 6:53   │
├ queue 3/12 ────────────────────────────────────────────────────┤
│    2  Breathe                                   Pink Floyd     │
│  > 3  Time                                      Pink Floyd     │
│    4  The Great Gig in the Sky                  Pink Floyd     │
│                                                                │
├────────────────────────────────────────────────────────────────┤
│ /                                                              │
└ ␣ toggle  n/p  ←/→ seek  +/- vol  / search  ⏎ play  ? keys ───┘
```

- **now** (4 rows, top): state glyph, title, artist, album · source, progress.
- **list** (the rest): one of queue / search results / browse page; a title bar
  says which. `Tab` cycles. Cursor, `Enter` plays, `a` queues, `d` removes (queue).
- **line** (1 row): input when `/` or `:` is open, else the last message.
- **keys** (frame bottom): the four or five that matter; `?` for the rest.

Resize: planes are recreated from the terminal size on `NCKEY_RESIZE`; nothing
is remembered about the old size.

### 3.1 Theme — "powerlines"

From the reference image: black sky, electric cyan and magenta light lines,
violet between them, a sunset orange and an acid green at the edges.

| Role | Hex | Used for |
|---|---|---|
| bg | `#05060e` | everything |
| cyan | `#19d3ff` | progress done, cursor row, title text |
| magenta | `#ff2bd6` | frame, state glyph when playing, volume |
| violet | `#8a3cff` | gradient midpoint, dim rows, frame when stopped |
| orange | `#ff7a1a` | errors, paused glyph |
| green | `#c6ff3a` | `ok` flashes, "live" badge on radio |
| dim | `#3a3f5c` | progress remaining, inactive text |
| fg | `#e6f0ff` | body text |

Bling that is free in Notcurses, in order of adding:
frame line as a magenta→cyan gradient (`ncplane_gradient2x1`), progress bar in
`━●─` with the done part stained cyan→magenta, volume as `▮` cells, title
"glow" by a dim violet duplicate one cell down/right, drawn first. A `--theme
plain` flag sets the eight colours to the terminal defaults, for 16-colour and
screen readers; `--theme <file>` reads `role = #hex` lines.

Everything is cells and colour. No images, no sixel, until §6.

---

## 4. Keys

```
space  toggle       n p    next/prev       ← →   seek ∓5 s   ⇧← ⇧→  ∓30 s
+ -    vol ±5       s      stop            0-9   nothing (yet)
j k ↓ ↑  cursor     ⏎      play cursor     a     queue cursor   d   remove (queue)
/      search       :      raw mmctl line  b     browse cursor list   ⌫ back
Tab    queue / results / browse            ?     help           q   quit
```

`:` sends whatever is typed as an IPC frame (words split on spaces) and shows
the reply — every daemon verb, present and future, works from the TUI on day
one without a binding.

---

## 5. Commits

One focus each, in the mediamaster voice: a title that says what changed, a
body that says why. Each ends green under `make test`, `SAN=asan`, `SAN=tsan`.

| # | Title line | What lands | Exit test |
|---|---|---|---|
| 0 | TUI: a client that asks the daemon its version | repo, Makefile, LICENSE, THIRD_PARTY.md, submodule pin, `src/ipc.c` (copy of `mmctl`'s connect/send/recv, made a library), `mmtui --version` prints the daemon's | `smoke.sh` starts the pinned daemon, `mmtui --version` matches `mmctl version` |
| 1 | TUI: status, queue and item lines read into structs | `ipc.c` typed readers, `model.h` | `ipc_test` on fixtures incl. `-` fields, a tab in a title (host turns it to a space), `queue is empty`, truncated `...` |
| 2 | TUI: a Notcurses screen that shows the daemon's state | core init, the three planes, `status` on a 250 ms tick, title/artist/album/state, `q` quits, resize | `smoke.sh` runs `mmtui` under `script(1)` with `TERM=xterm-256color`, checks it exits 0 on `q` and that the last frame holds the fixture title |
| 3 | TUI: progress and volume | position/duration bar, volume cells, `space n p ← → + - s` | smoke: send `space`, `status` says paused |
| 4 | TUI: the powerlines theme | `theme.c`, gradients, glyphs, `--theme plain`, `--theme <file>` | smoke under `plain` draws no escapes beyond 16 colours (`script` output grep) |
| 5 | TUI: the queue, with a cursor | list plane, `j k ⏎ a d`, follows the `>` row | smoke: `⏎` on row 2 → `status` item 2 |
| 6 | TUI: search | `/` input (own 1-line editor: insert, backspace, ←→, history ↑↓), results into the list plane, `⏎` plays, `a` queues | smoke with the ffmpeg source over a temp `ffmpeg.music_dir` of two generated files (raw has no catalog) |
| 7 | TUI: slow commands off the main loop | one worker thread, pipe back, "…" spinner in the line while waiting | tsan clean; smoke with `search` against a stalled fake |
| 8 | TUI: browse | `b` on a list item, breadcrumb in the list title, `⌫` back | smoke over a temp folder of two subfolders |
| 9 | TUI: a help overlay and a raw command line | `?`, `:` | smoke: `:version` shows the version in the line |
| 10 | TUI: config | `~/.config/mediamaster/tui`: `theme`, `tick_ms`, `seek_s`, `vol_step`; `MM_SOCK` honoured like mmctl | config test |
| 11 | README, `make install`, THIRD_PARTY current | — | fresh clone builds on a second machine |

Then, cross-repo, each a small daemon commit followed by a TUI commit:

| # | Daemon (mediamaster) | TUI |
|---|---|---|
| 12 | `queue list` prints `n\turi\ttitle\tartist\talbum\tm:ss` like browse (URIs only is what a TUI cannot show) — or a `queue list -l` flag if the plain form must stay | queue rows show titles |
| 13 | `watch [mask]`: keeps the connection open and writes one line per event (`state`, `track`, `volume`, `seek`, `queue`, `meta`) until the client closes — the events bus already exists, this is `events_subscribe` from the IPC thread | tick becomes event-driven; polling stays as the fallback when `watch` is `err unknown verb` (older daemon) |
| 14 | — | §6 bling |

---

## 6. Later bling (not in the first twelve)

- **Cover art** when `status` has `art:` — `ncvisual_from_file` into a plane
  beside `now`, needs the full `notcurses` build; `file://` straight, `http(s)://`
  fetched by the worker to `$XDG_CACHE_HOME/mediamaster/tui/art/`. Kitty/sixel
  where the terminal has them, else `NCBLIT_2x2` blocks. Optional at build
  (`pkg-config notcurses` present) and at run (`art = off`).
- **Level meter / wave strip** under the progress bar. Needs PCM from the
  daemon: a `tap` verb that streams peak pairs over the socket at 20 Hz (not
  samples). That is a host change on the tap ring the plan already foresees for
  finddl; park until that ring exists.
- **Lyrics pane** when the spotify plugin exposes `color-lyrics` through a verb.
- **Mouse**: click a row, click the bar to seek, wheel for volume. Notcurses
  gives it for free; one commit.

---

## 7. Defaults chosen (change by saying so)

- Own repo `mediamaster-tui`, binary `mmtui`, prefix `tui_`.
- Poll at 250 ms until `watch` exists; a tick that returns an unchanged
  `status` does not redraw.
- `notcurses-core` only for the first twelve commits.
- No threads until commit 7; one worker after it.
- Theme named "powerlines"; `plain` is the only other built-in.
- Keys follow mpv/cmus muscle memory where they do not clash.

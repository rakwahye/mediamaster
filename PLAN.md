# Media Master — plan v0.2

`mediamaster` daemon + `mmctl` client. C99 idioms, Linux. One play button, one search,
one queue, one volume across local files, Spotify, radio, YouTube and whatever else gets
strapped in. Conventional plugins (`dlopen`'d `.so`, strict C99 ABI header), a small host
that owns rings, events, IPC, config and policy — and nothing else.

Supersedes v0.1 (which borrowed projectm-daemon's linker-section registry; dropped).

Decisions so far: flat capabilities, no tree · Spotify = **Route A** (librespot-c linked,
Spotify's *internal* APIs, no Web API, no developer client ID, no phone) · decode =
spawned ffmpeg for now · host builds `-std=c11` only for `<stdatomic.h>`, everything
else and the whole ABI stays C99.

---

## 1. Shape

```
 mmctl ──unix sock──► ipc thread ──mailbox──► engine thread (state, queue, volume, resolver)
                                                   │
          plugins (.so or static)                  │ events bus (host-owned, SPSC per subscriber)
 ┌────────────────────────────────┐   PCM     ┌────▼────┐   PCM    ┌─────────────────────┐
 │ source: ffmpeg · spotify · raw │──reader──►│play ring│─callback►│ sink: pipewire · null│
 │ catalog/search: spotify · lib  │  thread   │lossless │          └──────────┬──────────┘
 │        · ytdlp · radio         │           └─────────┘                     │ copy
 │ identify: acoustid             │                                   ┌───────▼───────┐
 │ enrich:   musicbrainz          │                                   │   tap ring    │
 │ acquire:  ytdlp · record       │                                   │   lossy       │
 │ store:    library              │                                   └───────┬───────┘
 │ observer: icy · mpris · pw-mon │                                           ▼
 │ orchestrator: finddl           │◄─────────── events ─────────── finddl worker
 └────────────────────────────────┘
```

Host = kernel. Local library and the ffmpeg decoder are plugins too; nothing is privileged.

---

## 2. Plugin model

### 2.1 Interfaces (flat, composable) and attributes (policy)

A plugin implements any subset. Nothing is classified by "where it lives"; that is an
attribute the host uses for policy (offline switch, rate budget, key storage).

| Interface | Contract | v1 providers |
|---|---|---|
| `source` | `open(uri) → PCM into play ring`; flags `SEEKABLE·FINITE·LIVE·PRELOAD` | ffmpeg, spotify, raw |
| `sink` | drains play ring, mirrors to tap ring, applies host softvol | pipewire, null |
| `catalog` | `list(uri, cursor) → page of items`. **Search is a catalog node**: `<scheme>:search:<q>` | spotify, library, ytdlp, radio |
| `identify` | `PCM → track_meta + score` | acoustid |
| `enrich` | `track_meta → better track_meta` | musicbrainz |
| `observer` | emits `TRACK_CHANGED` for audio the host didn't originate | icy (in ffmpeg source), mpris, pw-monitor |
| `acquire` | `item → local file` | ytdlp, record (tap → file) |
| `store` | `file + meta → import, tag, index` | library |
| `orchestrator` | subscribes to events, drives the above through the host | finddl |

Attributes: `MM_ATTR_NETWORK`, `MM_ATTR_SPAWNS`, `MM_ATTR_NEEDS_LOGIN`, `MM_ATTR_RATE_LIMITED`,
`MM_ATTR_UNOFFICIAL_API`.

### 2.2 ABI (strict C99 header `mm_plugin.h`)

```c
#define MM_ABI 1
struct mm_plugin {
	uint32_t abi, attrs;
	const char *name, *version;
	const char *schemes;                 /* "spotify" · "file,http,https" · "yt" */
	int  (*init)(const struct mm_host *h, void **priv);
	void (*fini)(void *priv);
	const struct mm_source_ops   *source;      /* NULL = not provided */
	const struct mm_sink_ops     *sink;
	const struct mm_catalog_ops  *catalog;
	const struct mm_identify_ops *identify;
	const struct mm_enrich_ops   *enrich;
	const struct mm_observer_ops *observer;
	const struct mm_acquire_ops  *acquire;
	const struct mm_store_ops    *store;
	const struct mm_orch_ops     *orchestrator;
	uint32_t (*caps)(void *priv);        /* live capability mask; may shrink (see §5.4) */
};
const struct mm_plugin *mm_plugin_entry(void);   /* the one exported symbol */
```

`struct mm_host` (passed to `init`): `log`, `config_get`, `ring_write/wait`, `event_post`,
`event_subscribe`, `spawn` (argv, never shell), `rate_bucket` (token bucket per plugin),
`cache_dir`, `register_verb` (plugin adds IPC commands), `resolve` (ask the host to open a URI).

Loading: `$XDG_CONFIG_HOME/mediamaster/plugins/*.so`, `/usr/lib/mediamaster/plugins/*.so`,
plus a static builtin table (same struct) so tests and a no-dlopen build work. ABI mismatch
= refuse with a log line, never crash.

### 2.3 One struct for metadata, URIs everywhere

`struct mm_track_meta { title, artist, album, duration_ms, uri, ids{spotify, mbid, acoustid, isrc}, source_name }`.
Every item a catalog returns is a URI the resolver can `open`. Schemes: `file:` `http(s):`
`spotify:track:` `spotify:playlist:` `spotify:search:` `lib:` `yt:` `yt:search:` `radio:` `mb:recording:`.

---

## 3. Host

### 3.1 Rings (`ring.[ch]`)

SPSC, free-running 64-bit head/tail atomics, `slot = idx % cap`, one writer per counter.
Semantics chosen by the owner:

| Ring | Producer | Consumer | full | empty |
|---|---|---|---|---|
| play (~1 s) | source reader | sink callback | producer **waits** (eventfd) | silence + underrun counter |
| tap (~2 s) | sink callback | finddl worker | producer **drops** + overrun counter | consumer waits |

Format: f32 interleaved, 48 kHz, 2 ch. Conversions at the edges (ffmpeg on the way in,
f32→s16 before chromaprint). **Track-boundary markers** (frame index + meta handle) ride a
tiny side queue so the sink fires `TRACK_STARTED` sample-accurately — this is what makes
gapless work across *different* sources.

### 3.2 Engine

Owns state machine, queue, volume, resolver. IPC posts to a mailbox; engine drains per tick.
Host-owned for every source so they feel identical: **pause, volume, queue, next/prev,
shuffle/repeat**. Only `seek`/`duration` can be absent (`caps` lacks `SEEKABLE`/`FINITE`):
`seek` on radio answers `err not seekable`, nothing else changes.

Gapless: when the current item passes `prefetch_s` from its end, engine resolves and opens
the next queue item into the same ring behind a boundary marker.

Resolution ladder (config, cheapest/most-trusted first):
```
resolve.order  = library, spotify, radio, ytdlp
identify.order = observer, acoustid
acquire.order  = ytdlp, record
```

### 3.3 IPC and `mmctl`

Unix socket, one frame per connection: `<verb> argc=N\n` then N NUL-terminated words,
optional residual body. Replies `ok …` / `err …`; `status json` multi-line. `help` is built
live from host verbs + `register_verb`.

```
play <uri|query>   pause | resume | toggle | stop | next | prev   seek <±s|abs>   vol <0-100>
status [json]      queue add <uri> | list | rm <n> | clear | shuffle | repeat
search <q>         fan-out over every catalog's search root; numbered results
browse <uri>       list a catalog node (library, playlists, spotify:playlist:…)
dl <n|uri>         acquire + store
finddl on|off|now  identify + acquire what is playing
login spotify      run the OAuth bootstrap (§5.2)
plugins            loaded plugins, interfaces, live caps, latched areas
reload | help | quit
```

`play <query>` with no scheme = search, play first hit — the "one play button."

### 3.4 Subprocesses (`proc.[ch]`)

`posix_spawnp` with argv arrays, `CLOEXEC`, SIGTERM→SIGKILL, reaper thread. **Never a shell
string**: queries are built from metadata remote services return. Binaries are config keys
so tests substitute fakes in `tests/fakes/`.

---

## 4. Sources

**ffmpeg** — `ffmpeg -nostdin -loglevel error [-ss S] -i URI -vn -f f32le -ar 48000 -ac 2 -`,
reader thread → play ring. Seek = respawn with `-ss`. `ffprobe -of json` for duration/tags.
ICY `StreamTitle` parsed from `-icy 1` metadata output → observer events for radio.
Handles file/http/HLS/radio in one path. Linking libav* later = a second source plugin.

**raw** — test source: plays an f32 file or a generated sine. **spotify** — §5.

Spotify Connect receiver: **dropped**. The satellite source below is phone-independent.

---

## 5. Spotify plugin — Route A

### 5.1 Basis: librespot-c (MIT, ejurgensen, used by OwnTone, active Oct 2026)

Read `connection.c` directly. It already does: apresolve (accesspoint/spclient/dealer),
**login5 → bearer `http_accesstoken` + `http_clienttoken`**, spclient `metadata/4/track`,
`extended-metadata/v0` (protobuf-c), `storage-resolve`, decrypt-to-fd. Public API:
`librespotc_init/login_token/login_stored_cred/credentials_get/open/write/seek/close/metadata_get`.
`librespotc_open("spotify:track:…")` returns an **fd of decrypted Ogg Vorbis** → straight into
the ffmpeg decode path above. Spotify becomes "a file that arrives over a socket." Its
`sp_sysinfo` left zeroed makes it present as Spotify's own desktop client.

Deps it brings: libevent (BSD-3), libgcrypt (LGPL-2.1+), libcurl, json-c (MIT), protobuf-c (BSD-2).

### 5.2 OAuth bootstrap (C, ~150 lines)

Authorization-code flow against the desktop client ID (what librespot-oauth and ncspot's
streaming session use): open browser (or print URL for headless), 127.0.0.1 redirect
listener, exchange code via libcurl → access token → `librespotc_login_token` → persist the
`stored_cred` blob from `librespotc_credentials_get` (0600) so later logins skip the browser.
**Verify in spike:** whether `login_token` needs the username supplied or login5 returns it.

### 5.3 Internal APIs — what the ncspot thread led to

The failure mode in ncspot #1866/#1867 (Sep 2026) was `api.spotify.com`: a shared Web API
client ID rate-limited per application, search capped at 10, dev-mode IDs Premium-only with
5 users and no generated playlists. The fix adopted by MichaelPachec0's ncspot fork (#10–#14),
spotatui, riff and go-librespot: use the session's own login5 token against the internal
client APIs on **spclient**. No Web API, no developer app. We do the same from C, adding to
librespot-c:

| Need | spclient endpoint | Encoding | Source for shape |
|---|---|---|---|
| expose token | `librespotc_spclient_token_get(host, bearer, client_token)` — small patch, upstream it | — | librespot-c internals |
| search | `GET /context-resolve/v1/spotify:search:<q+>` → context pages of track URIs | JSON | librespot 0.8 `spclient.rs::get_context`, riff #17 |
| track/album/artist | `GET /metadata/4/{track,album,artist}/<hex>`; batch via `POST /extended-metadata/v0/extended-metadata` | protobuf | librespot-c has both builders |
| playlists | `GET /playlist/v2/user/<me>/rootlist`, `GET /playlist/v2/playlist/<id>` | protobuf `playlist4_external.proto` | librespot protos, MichaelPachec0 #13 |
| library | collection v2 paging (liked songs, saved albums), 300/page + delta sync token | protobuf `collection2v2.proto` | MichaelPachec0 #11, go-librespot — verify path in spike |
| radio (later) | `/radio-apollo/v3/…`, `/context-resolve/v1/autoplay` | JSON | librespot 0.8 |
| lyrics (later) | `/color-lyrics/v2/track/<id>` | JSON | spotatui |

Not available anywhere: hub/home/"made for you", browse categories, generated playlists.
Our home is our own library and queue, so nothing is lost.

`.proto` files are vendored from `librespot/protocol/proto` (MIT), compiled with `protoc-c`
at build; each carries its upstream header and a line in `THIRD_PARTY.md`.

### 5.4 Areas and latches

Each endpoint family is an **area** (`search`, `metadata`, `playlists`, `library`, `radio`)
with its own health. A 4xx that isn't 429, or a parse failure, **latches the area off** for
the session, the plugin's `caps()` drops it, host degrades that one feature and `plugins`
shows it. 429 → back off via `rate_bucket`. Nothing falls back to the Web API, nothing takes
the daemon down. Live tests exist but run only when a credentials blob is cached (like
MichaelPachec0's harness).

### 5.5 Caveats to put in README

Premium only. Unofficial client API; Spotify can change it without notice (ncspot #1732,
Nov 2025) — the area latches are the mitigation. librespot's own README says using it is
probably against Spotify's terms; user's call. Credentials blob is a long-lived secret.

---

## 6. finddl (orchestrator) + identify/acquire/store

Worker thread, never touches the audio path except reading the tap ring.

1. Trigger: `TRACK_STARTED` with unknown meta, an observer event, or `finddl now`.
2. If the event already carries artist/title (Spotify meta, ICY title) → skip to 5.
3. Tap window `acoustid.window_s` (60, min 30); overrun → restart.
4. `identify.order`: acoustid — libchromaprint `start(48000,2) → feed(s16) → finish →
   get_fingerprint`; `POST api.acoustid.org/v2/lookup` `client=<key>&duration=&fingerprint=&meta=recordings+releasegroups`;
   ≤ 3 req/s; accept if `score ≥ 0.85` **and** duration within ±`max_delta_s` when known.
5. `enrich`: musicbrainz `GET /ws/2/recording/<mbid>?inc=artists+releases&fmt=json`, ≤ 1 req/s,
   real `User-Agent: mediamaster/<ver> (<contact from config>)`.
6. Dedupe against `library.tsv` (acoustid, mbid, isrc, path).
7. `acquire.order`: ytdlp — `yt-dlp --dump-single-json --flat-playlist "ytsearch5:<artist> - <title>"`,
   rank by |duration−want| then title similarity, reject if none within `ytdlp.max_delta_s`;
   `yt-dlp -f bestaudio -x --audio-format <cfg> -o "<dir>/%(title)s.%(ext)s" <url>`.
   Fallback `record`: write the tap to file (radio).
8. `store`: library — `ffmpeg -i in -c copy -metadata …` with the **identified** tags
   (`MUSICBRAINZ_TRACKID`, `ACOUSTID_ID`), move to `library.dir/Artist/Album/Title.ext`,
   index. `finddl on` runs 1–8 on every track start.

HTTP: plugins link libcurl directly (already a hard dependency via librespot-c). JSON:
json-c where librespot-c already pulls it, `jsmn.h` (MIT, vendored) where a plugin wants
zero deps. Host offers `rate_bucket`, not HTTP.

---

## 7. Licenses and sources (verified 2026-11-04)

Project: **LGPL-2.1-or-later**. `THIRD_PARTY.md` lists every row with URL + license +
first-used commit. `make check-licenses` fails the build on any `src/**/*.[ch]` or
vendored file lacking an SPDX line.

| Component | Use | License | Notes |
|---|---|---|---|
| librespot-c | linked (spotify.so) | MIT (LICENSE read) | github.com/ejurgensen/librespot-c · plus libevent BSD-3, libgcrypt LGPL-2.1+, json-c MIT, protobuf-c BSD-2, libcurl |
| librespot `protocol/proto/*.proto` | vendored, `protoc-c` | MIT | github.com/librespot-org/librespot · headers kept |
| librespot 0.8 `spclient.rs` | **read for endpoint shapes only** | MIT | not copied (Rust) |
| ncspot, MichaelPachec0/ncspot | read for design (worker, areas/latches) | BSD-2-Clause | not copied |
| spotatui | read for endpoint list | MIT | not copied |
| go-librespot | referenced by librespot-c comments | **not verified** | do not copy |
| libpipewire-0.3 | linked (sink, pw-monitor) | MIT | — |
| libchromaprint | linked (acoustid.so) | MIT core + LGPL-2.1 resampler ⇒ LGPL-2.1 whole; `fpcalc` GPL-2+ (don't ship) | binary distro: check FFT lib — FFTW is GPL, kissfft BSD-3 |
| libcurl | linked | curl license | — |
| jsmn | vendored header | MIT | github.com/zserge/jsmn |
| ffmpeg / ffprobe | spawned | LGPL-2.1+ / GPL-2+ if `--enable-gpl` (most distros) | spawning sidesteps; linking libav* would make a GPL build's combination GPL |
| yt-dlp | spawned | Unlicense (repo); binaries bundle GPLv3+ PyInstaller | not redistributed; YouTube terms + local law are the user's — README says so |
| AcoustID web service | HTTP | free non-commercial, **≤ 3 req/s**, app key (`client=`), test keys expire | acoustid.org/webservice · lookup only |
| MusicBrainz API | HTTP | no key; **≤ 1 req/s**; meaningful User-Agent or 503 | musicbrainz.org/doc/MusicBrainz_API/Rate_Limiting |
| Spotify internal APIs | HTTP via login5 token | unofficial; Premium required | see §5.5 |

Do not lift code from GPL-only players (mpd, moc, cmus) into this tree; reading is fine.

---

## 8. Layout

```
mediamaster/
  include/mm_plugin.h          strict C99 ABI (the only header plugins see)
  src/host/   main.c cli.c config.c ipc.c engine.c queue.c resolver.c events.c ring.c
              proc.c loader.c meta.c log.c
  src/mmctl/  mmctl.c
  plugins/    ffmpeg/ raw/ pipewire/ null/ spotify/ library/ acoustid/ musicbrainz/
              ytdlp/ radio/ record/ icy/ mpris/ pwmon/ finddl/      (one dir each, own tests)
  third_party/ jsmn.h  proto/*.proto (vendored, headers intact)
  tests/      check.h  fakes/{ffmpeg,ffprobe,yt-dlp}  *_test.c  smoke-null.sh  sanitize-smoke.sh
  Makefile  config.mk  THIRD_PARTY.md  README.md
```

Build: `-std=c99 -Wall -Wextra -pedantic` for plugins and ABI; host `-std=c11` for atomics.
`SAN=asan|tsan`. Plugins build as `.so` and as static objects for the test binaries.

---

## 9. Phases

Each ends with tests green under `SAN=asan` and `SAN=tsan`, and `THIRD_PARTY.md` current.

| # | Deliverable | Exit test |
|---|---|---|
| 0 | `mm_plugin.h`, loader (dlopen + static), config, cli, ipc, `mmctl`, `help/quit/reload/plugins`; `null` plugin | `test-loader` (ABI mismatch refused), `test-ipc`, `test-config`; `mmctl plugins` lists null |
| 1 | `ring.[ch]` both modes + boundary markers, `null` + `pipewire` sinks, `raw` source; `play raw.f32` audible | `test-ring` (wrap, 64-bit counter, wait/wake, drop count, marker ordering); `smoke-null.sh` sample-count check |
| 2 | `proc`, `ffmpeg` source, engine, queue, resolver, `play/pause/seek/vol/status/next/prev` | `test-proc`, `test-engine` (fake ffmpeg emits known pattern), `test-queue`, `test-resolver` (scheme → plugin) |
| 3a | **Spotify spike**: librespot-c `open` → fd → ffmpeg → ring; byte-seek lands within 1 s?; which username does `login_token` need? | manual; decides A stays |
| 3b | `spotify` plugin: OAuth bootstrap, token exposure patch, areas+latches, search via context-resolve, metadata, rootlist/playlists, library | `test-spotify-areas` (latch state machine, offline), `test-spotify-parse` (fixtures); live tests gated on cached creds |
| 4 | tap ring, `acoustid`, `musicbrainz`, `finddl now` | `test-acoustid` (score/duration gate, 3 req/s bucket, fixture parse), `test-tap` (overrun restart) |
| 5 | `ytdlp` catalog+acquire, `record`, `library` store, `finddl on/off`, `dl`, `search` fan-out | `test-ytdlp` (dump-json ranking), `test-library` (dedupe), `test-search-merge`; e2e smoke with fakes |
| 6 | `icy`/`mpris`/`pwmon` observers, `radio` catalog, README, systemd user unit, `status json` | fresh-clone build on a second machine |

---

## 10. Defaults chosen (change by saying so)

- Names: `mediamaster`, `mmctl`, prefix `mm_`.
- Tap default `play` (what we output); `pwmon` (system sink monitor) is an optional observer.
- Downloads: yt-dlp `bestaudio` extracted to **opus** by default, `library.dir/Artist/Album/Title.ext`.
- No Spotify Connect receiver.
- Pathfinder GraphQL search skipped (rotating persisted-query hashes); `context-resolve` is enough.

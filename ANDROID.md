# mediamaster on Android — research notes

*Branch `android-research`, 2026-11-07. Notes, not code: nothing here is built. Where this
disagrees with what later gets built, the code wins.*

The question: can mediamaster — the daemon, the TUI, the plugins, `mm-update` — reach a phone,
and could a version of it earn a few dollars in an app store without becoming a finicky app?

## 1. Three facts that shape everything

**Google Play forbids downloaded code.** Play's Device and Network Abuse policy prohibits apps
(or SDKs in them) from downloading executable code — dex, JAR, `.so`, native code — from any
source other than Google Play; an app may not update itself by any means but Play's. The only
exception is code run by an interpreter or VM (JavaScript in a WebView, Python in an embedded
interpreter). A plugin manager that fetches or builds `.so` files is therefore not a technique
problem on Play but a policy violation. Kodi lives with this: its add-ons are Python, hence
interpreted, hence allowed; its native core ships in the APK.

**Android blocks running downloaded binaries anyway.** Apps targeting API 29 (Android 10) or
later cannot `exec()` — or `dlopen()` — files in their own writable data directory (a W^X rule
enforced by SELinux: `untrusted_app_29` has no `execute` on `app_data_file`). Binaries that
ship inside the APK and land in the native-library directory (`/data/app/.../lib/<arch>/`,
context `apk_data_file`) may run. Termux targets SDK 28 to keep its model and so left the Play
Store at v0.101 (2020); it lives on F-Droid and GitHub. A Play-store Termux fork exists that
executes through the system linker (`system_linker_exec`, `termux-exec`), which works but
sits under a policy that does not want it.

**Sideloading gets a gate in 2026–27, not a wall.** Google's developer verification: open to all
developers since March 2026; an "Android Developer Verifier" service on devices from April; an
"advanced flow" (friction, warnings) for installing apps from unverified developers from
August; enforcement in Brazil, Indonesia, Singapore and Thailand from 30 September 2026; global
in 2027. Hobbyist "limited distribution" accounts exist; installs over ADB stay exempt (for
now). F-Droid is working through the same thing. A user can still "download the installer
package", but the developer registers, and the user clicks through warnings unless they do.

## 2. Two Androids, two products

### The open one: Termux-style, F-Droid or a GitHub APK

The app bundles a prefix the way Termux does (its bootstrap zip: toolchain, git, make,
pkg-config, ffmpeg) and targets SDK 28. The *whole* model then works unchanged: `mm-update`,
plugins by address, the Spotify plugin fetched and built on the phone, the daemon exactly as
on a laptop. Termux's own packages already cover most of what we link: libevent, libgcrypt,
curl, json-c, ffmpeg; notcurses is packaged (with a shaky build record in Termux's
rebuild tracker, Dec 2025); protobuf-c unconfirmed. `$PREFIX` is writable, so `mm-update`'s
no-sudo branch already fits.

What is missing is a **sink**: no PipeWire on Android. A small sink plugin writing to Termux's
PulseAudio (`pacat`) or to AAudio (a C API in the NDK) is a day's work when the day comes.
The host Makefile must also skip `pipewire.so` and `mpris.so` cleanly when their libraries are
absent (to check).

What to embed: Termux's `terminal-emulator` / `terminal-view` libraries from `termux-app`
(used by Termux:Float, NeoTerm, Andronix), or the NLnet-funded `termux-core` library that is
meant to make a Termux rootfs embeddable in other apps. Caveat from that work: Termux packages
hard-code `/data/data/com.termux/files`; a different package name needs its own rebuild of the
bootstrap, or `termux-core` finished.

Audience: people who have Termux already. Free, and should be: F-Droid is free-only, and this
is where the project's character lives. Nothing in this product needs to be "agnostic" by
policy — it is agnostic by construction.

### The store one: a different product

A Play app may run only what shipped in its APK. So: the daemon plus a **fixed** set of stock
plugins (library, mix, net radio, perhaps open services — Bandcamp, Jamendo, Internet Archive,
Audius), shipped as native libraries in the APK and exec'd from the native-library directory;
no Termux, no `mm-update`, no compiler, no plugin-by-address. Agnostic in architecture, fixed
in catalogue. Paid.

Two things cannot be in it. The Spotify plugin: Play policy would not allow fetching it, and
a reverse-engineered Spotify client in a store listing under your own name is a takedown
magnet (the user's account is Premium, and Spotify's own terms say reverse-engineered clients
violate them; librespot's README says as much). And a terminal UI: a notcurses TUI in a
terminal view on a touchscreen is the finicky thing a store customer uninstalls. The
looking-glass design is what makes the store product tractable at all: a native UI (Compose)
is just another client of the same text protocol over the same unix socket; the daemon does
not care who is looking.

LGPL-2.1 is fine for a paid app: offer the source and the ability to relink. The
daemon-plus-`.so` shape gives relinking for free.

### Not iOS

iOS has no Termux, no exec of downloaded code, no cross-app unix sockets, no daemons that
outlive their app. The daemon would have to live *inside* the one app, plugins-as-apps do not
exist there (app groups only within one developer), and Apple's agreement forbids downloading
executable code as well (§3.3.2). macOS is fine. A TUI client over ssh to a daemon elsewhere
works everywhere, iOS included — that is already in use.

## 3. The "clean main app" model: plugins as separate processes and separate apps

The idea worth the most: keep the mediamaster app clean — no third-party service code, and
no mechanism specific to any service — and let **other developers** publish the Spotify (or
Bandcamp, or Jamendo) plugin as **their own app**, which the user installs from wherever and
which talks to mediamaster over a pipe. Nobody can point at mediamaster and say "take this
down": it ships a generic, documented extension protocol, like MPRIS over D-Bus, like Kodi's
add-on API, like Tasker's plugin intents.

### How it would work

Today a plugin is a `.so` the daemon `dlopen`s, speaking `mm_plugin.h`. The out-of-process
form is the **same ABI serialised over a stream**:

- A plugin process connects to the daemon's existing socket (the one `mmctl` and the TUI
  use, found by the same `mm_client_socket` rule) and announces itself:
  `plugin <name> <version> <abi> <schemes> <attrs>`. The daemon answers and the connection
  switches to a framed request/response protocol. One socket, one discovery rule, no new
  paths.
- The daemon's side is a builtin `remote` plugin: for each connection it creates a proxy
  `struct mm_plugin` whose ops forward over the wire (`open`, `read`, `seek`, `close`,
  `interrupt`; `list`, `radio`, `match`) and hands it to the loader like any other. The rest
  of the daemon cannot tell the difference. Host services go the other way on the same
  connection: `log`, `config_get`, `stream_tags`/`stream_update`, `register_verb` (verb calls
  forwarded back), `command`, `subscribe`.
- Audio rides the same stream: 48 kHz stereo f32 is 384 KB/s, nothing for a unix socket, and
  the reader thread already buffers a second ahead. If it ever matters, an fd for a second
  data connection can be passed with `SCM_RIGHTS` (Android's `LocalSocket` supports it too).
- The wire protocol is versioned by the **same** `MM_ABI` major.minor and mirrors the ops
  table field for field, so the two forms cannot drift apart: an op added to the `.so` ABI as
  a minor is a message added to the wire as the same minor.

### The adapter: every `.so` becomes a process for free

One small program in the SDK, `mm-plugin-host <plugin.so>`: it `dlopen`s any existing plugin,
connects to the daemon, and speaks the wire protocol on the plugin's behalf. No plugin
changes. On Linux the daemon can spawn it for `.so` files it is told to keep out of process
(`host.isolate = spotify`), optionally under bwrap/Landlock/seccomp — which is the real
containment layer the trust model (§4) cannot give in-process. The Spotify plugin becomes a
separate process without touching its source.

On Android, a plugin app is exactly this adapter plus the plugin's `.so` in its own APK:
its own process, its own UID, its own `INTERNET` permission (mediamaster needs none for
Spotify then), its own developer, its own store listing or sideload. It connects to the
mediamaster app's socket (abstract-namespace `LocalSocket` works across apps; a filesystem
socket does not, since app data directories are private).

### Permission on Android

An abstract socket has no file permissions: anyone who knows the name can connect. The daemon
has `SO_PEERCRED` and so the peer's UID; the mediamaster app's Java side maps UID → package
name through `PackageManager` and asks the user once: "Allow *Spotify for mediamaster* to
connect?" — then tells the daemon the UID is allowed (a verb; the same mechanism as
`host.allow.<plugin>` in §4). This is how Tasker plugins and Android Auto media apps are
gated: consent per app, remembered.

### What it buys

- The liability firewall the model exists for: mediamaster publishes a protocol; who speaks
  it is not mediamaster's doing. Kodi has lived in the Play Store on this exact posture for a
  decade while third parties ship add-ons the Kodi Foundation publicly disowns.
- Plugins in any language: a Rust plugin could use librespot proper; a Python one could wrap
  yt-dlp; a Go one go-librespot. "Interconnect all the best tech" stops meaning "rewrite it in
  C99".
- Sandboxing for plugins from strangers, on the desktop too.
- Crash isolation: a plugin that dies takes its stream, not the daemon.

### What it costs

- A wire protocol (mirror of `mm_plugin.h`), the `remote` proxy in the host, and the adapter
  program: roughly 1500 lines of C, plus tests with a fake plugin process. One minor ABI bump
  (a new verb for the handshake). The in-process `.so` path stays as it is — mix, library and
  the sinks have no reason to leave the process.
- Latency on `open`: a round trip plus the plugin's own work; negligible against network
  streams, and the gapless preload already hides it.
- On Android, two apps to keep in step; the ABI fit rule (`MM_ABI_FITS`) already answers
  "do these two work together", and the handshake carries both numbers.

### Where it sits in the order

After the Spotify login flow (in progress) and mix step 2 (radio from a seed), before or
alongside the plugin-trust layers below, since the adapter is what makes layer 5 real.

## 4. Trust for plugins by address (the open product and the desktop)

This is the AUR problem: a stranger's Makefile, then their code in the daemon's process. No
mechanism makes that *safe*; the aim is trust that is explicit, pinned and visible.

1. **Pinned signers, ssh-style (trust on first use).** `mm-update add <url>` records the repo
   and the SSH key that signed the commit it first fetched (git signs commits with SSH keys;
   `git verify-commit` against an `allowed_signers` file in `~/.config/mediamaster/`). Later
   updates refuse an unsigned HEAD or a different key — "the signer changed", like a host key.
   No GPG, no web of trust.
2. **A manifest.** `PLUGIN` in the repo root: name, ABI major, what it needs
   (`needs = network, spawn`), what it installs. No manifest, no build. Shown and confirmed on
   first add.
3. **Attributes enforced at load.** `MM_ATTR_NETWORK` and `MM_ATTR_SPAWNS` exist in the ABI;
   `host.allow.<plugin> = network, spawn` and a loader that refuses a plugin claiming more than
   allowed, `-L` showing the claims. In-process code can lie about its attributes: this is a
   seatbelt and a consent screen, not a sandbox.
4. **A curated index.** `rakwahye/mediamaster-plugins`: known repos, their signer keys, a line
   on each. `mm-update add spotify` resolves by name; any address still works through layer 1.
   Three tiers: stock (in the host), known (in the index), anywhere (pinned on first use).
5. **Containment: out-of-process plugins** (§3), sandboxed. The path if strangers' plugins
   become normal.

Layers 1, 2 and 4 are desktop features worth having regardless, a day or two; 3 is an
afternoon. None of it applies to the store product, where there is nothing to fetch.

## 5. Lining up now, at no cost

- Paths from `$HOME` and XDG only, never hard-coded (Termux's own hard-coded-path problem).
  Already so.
- Sinks are plugins; `pacat`/AAudio is a small file when needed. Already so.
- The ABI's ops tables carry their size and the fit rule is one macro: the wire protocol can
  mirror them without inventing a second versioning scheme. Already so.
- To check: the host Makefile skipping `pipewire`/`mpris` where their libraries are absent.
- Keep the TUI a pure client of the text protocol (it is) so a native UI can be one too.

## Sources

- Google Play, Device and Network Abuse policy — downloading executable code, self-update:
  https://support.google.com/googleplay/android-developer/answer/9888379 and the policy
  summary at https://support.google.com/googleplay/android-developer/answer/16559646
- Termux wiki, "Termux and Android 10" — W^X, exec from the app data directory, SDK 28:
  https://github.com/termux/termux-packages/wiki/Termux-and-Android-10
- Android 10 behaviour changes — execute permission:
  https://developer.android.com/about/versions/10/behavior-changes-10#execute-permission
- Termux issues #1072 and #2155 (W^X), termux-exec `system_linker_exec`; Termux NLnet NGI
  Mobifree grant post (termux-core library, hard-coded paths):
  https://termux.dev/en/posts/general/2024/11/11/termux-selected-for-nlnet-ngi-mobifree-grant.html
- Android developer verification timeline (Android Authority, 18 Jun 2026; 9to5Google;
  Google's announcements): verification open to all March 2026, Verifier on devices April,
  advanced sideloading flow August, enforcement in BR/ID/SG/TH 30 Sep 2026, global 2027;
  ADB exempt; limited-distribution accounts for hobbyists.
- Termux packages rebuild tracker (notcurses listed, Dec 2025):
  https://github.com/termux/termux-packages/issues/23492
- Apple Developer Program License Agreement §3.3.2 (no downloaded executable code).
- Kodi add-on model (Python add-ons; Kodi Foundation's stance on third-party add-ons) as the
  precedent for a clean core with third-party extensions in app stores.

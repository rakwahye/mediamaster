# mediamaster

An IPC-controlled media daemon in C (audio first). One play button, one search, one queue
across local files, Spotify, radio, YouTube and anything a plugin brings in.

**Status: phase 0** — plugin ABI, loader, config, IPC, `mmctl`. No audio yet.
See [docs/PLAN.md](docs/PLAN.md) for the design and phases.

## Build

Linux, a C11 compiler, nothing else for now.

```sh
make            # build/mediamaster, build/mmctl, build/plugins/*.so
make test       # unit tests + end-to-end smoke
make SAN=asan test
make SAN=tsan test
```

## Run

```sh
mmctl launch            # start detached; logs to ~/.local/state/mediamaster/
mmctl                   # live command list from the daemon
mmctl plugins
mmctl quit
```

Foreground with debug logging: `mediamaster -d`.
`SIGHUP` or `mmctl reload` re-reads the config.

## Config

`~/.config/mediamaster/config`, flat `owner.key = value`:

```ini
host.socket     = /run/user/1000/mediamaster.sock   # default: $XDG_RUNTIME_DIR
host.plugin_dir = ~/.config/mediamaster/plugins:/usr/local/lib/mediamaster/plugins
host.builtins   = 1          # link-time plugins load first and win name clashes
host.disable    = null       # comma list; listed but never initialised
host.log_level  = info       # error | warn | info | debug
null.greeting   = hello      # every plugin reads its own <name>.* slice
```

`-o key=value` overrides a key and survives reloads.

## Plugins

A plugin is a `.so` exporting `mm_plugin_entry()`, written against
[`include/mm_plugin.h`](include/mm_plugin.h) only (strict C99). It fills
the interface tables it implements and leaves the rest NULL. See
[`plugins/null/null.c`](plugins/null/null.c) for the smallest working one.

## License

LGPL-2.1-or-later. Third-party components: [THIRD_PARTY.md](THIRD_PARTY.md).

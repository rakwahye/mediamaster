# SPDX-License-Identifier: LGPL-2.1-or-later
#
#   make                 daemon, mmctl, plugins          -> build/
#   make test            unit tests + smoke
#   make SAN=asan test   same under AddressSanitizer     -> build/asan/
#   make SAN=tsan test   same under ThreadSanitizer      -> build/tsan/
#   make check-licenses  every source file carries an SPDX line
#   make install PREFIX=/usr/local

CC         ?= cc
PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
PLUGIN_DIR ?= $(PREFIX)/lib/mediamaster/plugins
VERSION    := 0.1.0

SAN ?=
ifeq ($(SAN),)
  SAN_FLAGS :=
  OPT       := -O2 -g
  BUILD     := build
else ifeq ($(SAN),asan)
  SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
  OPT       := -O1 -g
  BUILD     := build/asan
else ifeq ($(SAN),tsan)
  SAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer
  OPT       := -O1 -g
  BUILD     := build/tsan
else
  $(error SAN must be empty, asan or tsan)
endif

WARN := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2

# Host: C11 only for <stdatomic.h>; everything else is plain C99 style.
HOST_CFLAGS := -std=c11 $(WARN) $(OPT) $(SAN_FLAGS) -D_GNU_SOURCE \
               -Iinclude -Isrc/host \
               -DMM_PLUGIN_DIR=\"$(PLUGIN_DIR)\" -DMM_VERSION=\"$(VERSION)\"
HOST_LIBS   := -ldl -lpthread -lm $(SAN_FLAGS)

# Plugins: strict C99 against the ABI header only.
PLUG_CFLAGS := -std=c99 -pedantic $(WARN) $(OPT) $(SAN_FLAGS) -D_POSIX_C_SOURCE=200809L \
               -Iinclude -fPIC -fvisibility=hidden
PLUG_LDFLAGS := -shared $(SAN_FLAGS)

# Built in (no dependencies) and also built as .so.
PLUGINS_STATIC := null raw ffmpeg library
# Shared objects; pipewire only when its headers are installed.
PLUGINS := null raw ffmpeg library
HAVE_PIPEWIRE := $(shell pkg-config --exists libpipewire-0.3 && echo 1)
ifeq ($(HAVE_PIPEWIRE),1)
  PLUGINS += pipewire
endif

# mpris on sd-bus: libsystemd, or basu where systemd is absent.
SDBUS_PC := $(shell pkg-config --exists libsystemd && echo libsystemd || (pkg-config --exists basu && echo basu))
ifneq ($(SDBUS_PC),)
  PLUGINS += mpris
endif

# Per-plugin extras.
LIBS_raw := -lm
CFLAGS_ffmpeg := -D_GNU_SOURCE
CFLAGS_pipewire := $(shell pkg-config --cflags libpipewire-0.3 2>/dev/null) -std=gnu99 -Wno-pedantic -Wno-missing-prototypes
LIBS_pipewire := $(shell pkg-config --libs libpipewire-0.3 2>/dev/null)
CFLAGS_mpris := $(shell pkg-config --cflags $(SDBUS_PC) 2>/dev/null) $(if $(filter basu,$(SDBUS_PC)),-DMM_HAVE_BASU) \
                -std=gnu99 -D_GNU_SOURCE -Wno-pedantic
LIBS_mpris := $(shell pkg-config --libs $(SDBUS_PC) 2>/dev/null)

HOST_SRC := main.c log.c config.c paths.c ipc.c loader.c builtins.c ring.c player.c queue.c events.c \
            catalog.c uri.c radio.c watch.c levels.c
HOST_OBJ := $(addprefix $(BUILD)/host/,$(HOST_SRC:.c=.o)) \
            $(foreach p,$(PLUGINS_STATIC),$(BUILD)/static/$(p).o)
MMCTL_OBJ := $(BUILD)/obj-mmctl/mmctl.o $(BUILD)/host/paths.o
PLUGIN_SO := $(foreach p,$(PLUGINS),$(BUILD)/plugins/$(p).so)

HDRS := $(wildcard include/*.h src/host/*.h)

.SECONDEXPANSION:
.PHONY: all test unit smoke check-licenses install clean
all: $(BUILD)/mediamaster $(BUILD)/mmctl $(PLUGIN_SO)

$(BUILD)/host/%.o: src/host/%.c $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD)/obj-mmctl/%.o: src/mmctl/%.c $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

# A plugin may be several files: built and partially linked into one object.
$(BUILD)/static/%.o: $$(wildcard plugins/$$*/*.c plugins/$$*/*.h) include/mm_plugin.h
	@mkdir -p $(@D)
	$(CC) $(PLUG_CFLAGS) $(CFLAGS_$*) -DMM_PLUGIN_STATIC -r -nostdlib $(filter %.c,$^) -o $@

$(BUILD)/plugins/%.so: $$(wildcard plugins/$$*/*.c plugins/$$*/*.h) include/mm_plugin.h
	@mkdir -p $(@D)
	$(CC) $(PLUG_CFLAGS) $(CFLAGS_$*) $(filter %.c,$^) $(PLUG_LDFLAGS) $(LIBS_$*) -lpthread -o $@

$(BUILD)/mediamaster: $(HOST_OBJ)
	$(CC) $^ $(HOST_LIBS) -o $@

$(BUILD)/mmctl: $(MMCTL_OBJ)
	$(CC) $^ $(SAN_FLAGS) -o $@

# ------------------------------------------------------------------ tests

TEST_CFLAGS := $(HOST_CFLAGS) -Itests
T := $(BUILD)/tests

# Everything a test needs once the loader is involved.
PLAYER_DEPS := src/host/player.c src/host/ring.c src/host/loader.c src/host/ipc.c src/host/config.c src/host/log.c \
               src/host/queue.c src/host/events.c src/host/catalog.c src/host/uri.c src/host/radio.c

$(T)/test-config: tests/config_test.c src/host/config.c src/host/log.c $(HDRS) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) $(filter %.c,$^) $(HOST_LIBS) -o $@

$(T)/test-ipc: tests/ipc_test.c src/host/ipc.c src/host/log.c $(HDRS) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) $(filter %.c,$^) $(HOST_LIBS) -o $@

$(T)/test-loader: tests/loader_test.c $(PLAYER_DEPS) $(HDRS) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) $(filter %.c,$^) $(HOST_LIBS) -o $@

$(T)/test-ring: tests/ring_test.c src/host/ring.c $(HDRS) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) $(filter %.c,$^) $(HOST_LIBS) -o $@

$(T)/test-player: tests/player_test.c $(PLAYER_DEPS) $(BUILD)/static/raw.o $(HDRS) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) $(filter %.c %.o,$^) $(HOST_LIBS) -o $@

$(T)/test-ffmpeg: tests/ffmpeg_test.c $(PLAYER_DEPS) $(BUILD)/static/ffmpeg.o $(HDRS) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) $(filter %.c %.o,$^) $(HOST_LIBS) -o $@

$(T)/test-queue: tests/queue_test.c $(PLAYER_DEPS) $(BUILD)/static/raw.o $(HDRS) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) $(filter %.c %.o,$^) $(HOST_LIBS) -o $@

$(T)/test-events: tests/events_test.c $(PLAYER_DEPS) $(BUILD)/static/raw.o $(HDRS) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) $(filter %.c %.o,$^) $(HOST_LIBS) -o $@

$(T)/test-levels: tests/levels_test.c src/host/levels.c $(HDRS) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) $(filter %.c,$^) $(HOST_LIBS) -o $@

$(T)/test-libidx: tests/libidx_test.c plugins/library/index.c plugins/library/probe.c $(wildcard plugins/library/*.h) $(HDRS) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) -Iplugins/library $(filter %.c,$^) -lpthread -o $@

TESTS := $(T)/test-levels $(T)/test-libidx $(T)/test-events $(T)/test-queue $(T)/test-config $(T)/test-ipc $(T)/test-loader $(T)/test-ring $(T)/test-player $(T)/test-ffmpeg

unit: $(TESTS) $(PLUGIN_SO)
	@$(T)/test-config
	@$(T)/test-ipc
	@$(T)/test-loader $(BUILD)/plugins
	@$(T)/test-ring
	@$(T)/test-player
	@$(T)/test-queue
	@$(T)/test-events
	@$(T)/test-levels
	@$(T)/test-ffmpeg
	@$(T)/test-libidx

smoke: all
	@sh tests/smoke.sh $(BUILD)
	@sh tests/mpris.sh $(BUILD)

# Sequential: smoke times real playback and must not compete with unit tests.
test: unit
	@$(MAKE) --no-print-directory smoke

check-licenses:
	@missing=$$(grep -L 'SPDX-License-Identifier' $$(git ls-files -co --exclude-standard \
		'*.c' '*.h' '*.sh' Makefile) 2>/dev/null); \
	if [ -n "$$missing" ]; then echo "missing SPDX line:"; echo "$$missing"; exit 1; fi; \
	echo "check-licenses: ok"

install: all
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(PLUGIN_DIR)
	install -m 755 $(BUILD)/mediamaster $(BUILD)/mmctl $(DESTDIR)$(BINDIR)
	install -m 644 $(PLUGIN_SO) $(DESTDIR)$(PLUGIN_DIR)

clean:
	rm -rf build

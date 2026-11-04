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
HOST_LIBS   := -ldl -lpthread $(SAN_FLAGS)

# Plugins: strict C99 against the ABI header only.
PLUG_CFLAGS := -std=c99 -pedantic $(WARN) $(OPT) $(SAN_FLAGS) -D_POSIX_C_SOURCE=200809L \
               -Iinclude -fPIC -fvisibility=hidden
PLUG_LDFLAGS := -shared $(SAN_FLAGS)

PLUGINS := null

HOST_SRC := main.c log.c config.c paths.c ipc.c loader.c builtins.c
HOST_OBJ := $(addprefix $(BUILD)/host/,$(HOST_SRC:.c=.o)) \
            $(foreach p,$(PLUGINS),$(BUILD)/static/$(p).o)
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

$(BUILD)/static/%.o: $$(wildcard plugins/$$*/*.c) include/mm_plugin.h
	@mkdir -p $(@D)
	$(CC) $(PLUG_CFLAGS) -DMM_PLUGIN_STATIC -c $(filter %.c,$^) -o $@

$(BUILD)/plugins/%.so: $$(wildcard plugins/$$*/*.c) include/mm_plugin.h
	@mkdir -p $(@D)
	$(CC) $(PLUG_CFLAGS) $(filter %.c,$^) $(PLUG_LDFLAGS) -o $@

$(BUILD)/mediamaster: $(HOST_OBJ)
	$(CC) $^ $(HOST_LIBS) -o $@

$(BUILD)/mmctl: $(MMCTL_OBJ)
	$(CC) $^ $(SAN_FLAGS) -o $@

# ------------------------------------------------------------------ tests

TEST_CFLAGS := $(HOST_CFLAGS) -Itests
T := $(BUILD)/tests

$(T)/test-config: tests/config_test.c src/host/config.c src/host/log.c $(HDRS) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) $(filter %.c,$^) $(HOST_LIBS) -o $@

$(T)/test-ipc: tests/ipc_test.c src/host/ipc.c src/host/log.c $(HDRS) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) $(filter %.c,$^) $(HOST_LIBS) -o $@

$(T)/test-loader: tests/loader_test.c src/host/loader.c src/host/ipc.c src/host/config.c src/host/log.c $(HDRS) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) $(filter %.c,$^) $(HOST_LIBS) -o $@

TESTS := $(T)/test-config $(T)/test-ipc $(T)/test-loader

unit: $(TESTS) $(PLUGIN_SO)
	@$(T)/test-config
	@$(T)/test-ipc
	@$(T)/test-loader $(BUILD)/plugins

smoke: all
	@sh tests/smoke.sh $(BUILD)

test: unit smoke

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

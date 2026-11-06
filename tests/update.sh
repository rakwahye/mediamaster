#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# mm-update on fake repositories under a scratch HOME: lists, pulls,
# builds, installs in order, stops at the first failure, reports status,
# adds by path and by URL.
set -u
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "update: FAIL: $*"; exit 1; }
export HOME="$T/home" XDG_CONFIG_HOME="$T/home/.config" XDG_DATA_HOME="$T/home/.local/share" MM_SUDO=
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t
mkdir -p "$HOME"
U=$(cd "$(dirname "$0")/.." && pwd)/tools/mm-update

# A fake project: make records that it built, make install that it installed.
mkrepo() { # name
	mkdir -p "$T/$1" && cd "$T/$1" && git init -q &&
	printf 'all:\n\t@echo built >>$(HOME)/log-%s\ninstall:\n\t@echo installed >>$(HOME)/log-%s\n' "$1" "$1" >Makefile &&
	git add Makefile && git commit -qm init && cd "$T"
}
mkrepo one && mkrepo two || fail "fixtures"

mkdir -p "$XDG_CONFIG_HOME/mediamaster"
printf '# the list\n%s/one\n  ~/../two   # a comment\n' "$T" >"$XDG_CONFIG_HOME/mediamaster/repos"
out=$("$U" 2>&1) || fail "update: $out"
[ "$(cat "$HOME/log-one")" = "built
installed" ] && [ "$(cat "$HOME/log-two")" = "built
installed" ] || fail "both built and installed: $out"
echo "$out" | grep -q "no upstream branch: not pulled" || fail "a repo without a remote is not pulled: $out"
echo "$out" | grep -q "2 up to date" || fail "summary: $out"

# A clone with an upstream is pulled: a commit lands upstream, update sees it.
git clone -q "$T/one" "$T/one-clone" || fail "clone"
cd "$T/one" && echo x >f && git add f && git commit -qm more && cd "$T"
printf '%s/one-clone\n' "$T" >"$XDG_CONFIG_HOME/mediamaster/repos"
"$U" >/dev/null 2>&1 || fail "update of a clone"
[ -f "$T/one-clone/f" ] || fail "the clone was not pulled"

# The first failure stops the run, later repos untouched.
mkdir -p "$T/bad" && cd "$T/bad" && git init -q && printf 'all:\n\t@exit 3\n' >Makefile && git add Makefile && git commit -qm init && cd "$T"
printf '%s/bad\n%s/two\n' "$T" "$T" >"$XDG_CONFIG_HOME/mediamaster/repos"
rm -f "$HOME/log-two"
"$U" >/dev/null 2>&1 && fail "a failing make should fail the update"
[ ! -f "$HOME/log-two" ] || fail "repos after a failure must not be built"

# status: a line per repo with its commit; a dirty tree is said.
echo y >"$T/two/Makefile.new"
printf '%s/one\n%s/two\n' "$T" "$T" >"$XDG_CONFIG_HOME/mediamaster/repos"
st=$("$U" status 2>&1) || fail "status: $st"
echo "$st" | grep -q "$T/one" && echo "$st" | grep -q "more" || fail "status shows repos and commits: $st"
echo "$st" | grep -q "local changes" || fail "status shows a dirty tree: $st"

# add: by path (once), by URL (cloned under the data dir).
"$U" add "$T/one" | grep -q "is listed" || fail "add of a listed path"
"$U" add "$T/bad" | grep -q "^added" || fail "add by path"
"$U" add "file://$T/one" | grep -q "^added" || fail "add by URL"
[ -d "$XDG_DATA_HOME/mediamaster/src/one/.git" ] || fail "URL was not cloned"
grep -qx "$XDG_DATA_HOME/mediamaster/src/one" "$XDG_CONFIG_HOME/mediamaster/repos" || fail "clone not listed"
"$U" bogus >/dev/null 2>&1 && fail "unknown command accepted"
"$U" help | grep -q "mm-update status" || fail "help"
echo "update: all checks passed"

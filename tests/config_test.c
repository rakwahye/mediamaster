/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "config.h"
#include "check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char s_path[128];

static int write_file(const char *text)
{
	FILE *f = fopen(s_path, "w");
	if (!f) return -1;
	fputs(text, f);
	fclose(f);
	return 0;
}

static int parsing(void)
{
	CHECK(write_file(
		"# full-line comment\n"
		"   \n"
		"host.socket = /tmp/x.sock\n"
		"null.greeting=hi there   # trailing comment\n"
		"null.quoted = \"  keep # this  \"\n"
		"null.hash=a#b\n"
		"not a pair\n"
		"bad key = 1\n"
		"null.dup = first\n"
		"null.dup = second\n"
		"host.n = 0x10\n"
		"host.flag = yes\n") == 0, "the fixture is written");
	CHECK(config_load(s_path) == 0, "a readable file loads");
	CHECK(!strcmp(config_get("host.socket"), "/tmp/x.sock"), "spaces around '=' are trimmed");
	CHECK(!strcmp(config_get("null.greeting"), "hi there"), "an inline comment after whitespace is dropped, inner spaces kept");
	CHECK(!strcmp(config_get("null.quoted"), "  keep # this  "), "a quoted value is verbatim");
	CHECK(!strcmp(config_get("null.hash"), "a#b"), "a '#' inside a word is not a comment");
	CHECK(config_get("not") == NULL && config_get("bad") == NULL, "malformed lines are skipped");
	CHECK(!strcmp(config_get("null.dup"), "second"), "the later line wins");
	CHECK(config_get_long("host.n", 0) == 16, "numbers accept a 0x prefix");
	CHECK(config_get_bool("host.flag", 0) == 1, "yes reads as true");
	CHECK(config_get_long("host.missing", 7) == 7, "a missing key falls back to the default");
	CHECK(config_get_long("null.greeting", 3) == 3, "a non-number falls back to the default");
	return 0;
}

static int overrides_and_reload(void)
{
	const char *before = config_get("null.greeting");
	CHECK(config_override("null.greeting=from cli") == 0, "a key=value override is accepted");
	CHECK(config_override("garbage") != 0, "an override without '=' is refused");
	CHECK(!strcmp(before, "hi there"), "a string handed out earlier is still intact");

	CHECK(write_file("null.greeting = edited\nhost.socket = /tmp/y.sock\n") == 0, "the fixture is rewritten");
	CHECK(config_reload() == 0, "reload re-reads the same path");
	CHECK(!strcmp(config_get("host.socket"), "/tmp/y.sock"), "reload picks up edits");
	CHECK(!strcmp(config_get("null.greeting"), "from cli"), "an override beats the file and survives reload");
	CHECK(config_get("null.quoted") == NULL, "keys removed from the file are gone after reload");
	CHECK(!strcmp(before, "hi there"), "an old generation's strings outlive the reload");
	return 0;
}

static int missing_file(void)
{
	unlink(s_path);
	CHECK(config_load(s_path) == 0, "a missing file is not an error");
	CHECK(config_get("host.socket") == NULL, "a missing file means no file keys");
	CHECK(!strcmp(config_get("null.greeting"), "from cli"), "overrides still apply without a file");
	return 0;
}

int main(void)
{
	snprintf(s_path, sizeof(s_path), "/tmp/mm-config-test.%d", (int)getpid());
	RUN(parsing);
	RUN(overrides_and_reload);
	RUN(missing_file);
	config_free();
	CHECK_PASS("test-config");
}

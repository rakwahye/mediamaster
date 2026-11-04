/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "ipc.h"
#include "check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static char s_sock[100];

/* What the probe verb last saw. Written on the IPC thread, read here only
 * after the reply arrived, which orders the two. */
static int  g_argc;
static char g_argv[8][128];
static char g_body[64];
static size_t g_body_len;
static int  g_hits;

static int probe(void *priv, struct mm_verb_ctx *c)
{
	(void)priv;
	g_hits++;
	g_argc = c->argc;
	for (int i = 0; i < c->argc && i < 8; i++) snprintf(g_argv[i], sizeof(g_argv[i]), "%s", c->argv[i]);
	g_body_len = c->body_len;
	memcpy(g_body, c->body, c->body_len < sizeof(g_body) ? c->body_len : sizeof(g_body));
	snprintf(c->reply, c->reply_len, "probed %d", c->argc);
	return MM_OK;
}

static int fails(void *priv, struct mm_verb_ctx *c)
{
	(void)priv;
	snprintf(c->reply, c->reply_len, "nope\n");
	return MM_ERR;
}

static int raw(void *priv, struct mm_verb_ctx *c)
{
	(void)priv;
	snprintf(c->reply, c->reply_len, "line one\nline two\n");
	return MM_REPLY_RAW;
}

/* Send bytes as one write, read the whole reply. */
static int exchange(const void *msg, size_t len, char *reply, size_t cap)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	struct sockaddr_un a;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", s_sock);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); return -1; }
	if (len && send(fd, msg, len, MSG_NOSIGNAL) != (ssize_t)len) { close(fd); return -1; }
	shutdown(fd, SHUT_WR);
	size_t n = 0;
	ssize_t r;
	while (n + 1 < cap && (r = recv(fd, reply + n, cap - 1 - n, 0)) > 0) n += (size_t)r;
	reply[n] = '\0';
	close(fd);
	return 0;
}

static int framed(int argc, const char *const *argv, const void *body, size_t body_len,
                  char *reply, size_t cap)
{
	char buf[4096];
	size_t off = (size_t)snprintf(buf, sizeof(buf), "%s argc=%d\n", argv[0], argc);
	for (int i = 0; i < argc; i++) {
		size_t l = strlen(argv[i]) + 1;
		memcpy(buf + off, argv[i], l);
		off += l;
	}
	if (body_len) memcpy(buf + off, body, body_len);
	off += body_len;
	return exchange(buf, off, reply, cap);
}

static int registry(void)
{
	CHECK(ipc_register("t", "probe", "[x]\tprobe", probe, NULL) == 0, "a fresh verb registers");
	CHECK(ipc_register("u", "probe", "", probe, NULL) != 0, "a taken verb is refused");
	CHECK(ipc_register("t", "help", "", probe, NULL) != 0, "help is reserved");
	CHECK(ipc_register("t", "two words", "", probe, NULL) != 0, "a verb with whitespace is refused");
	CHECK(ipc_register("t", "fails", "", fails, NULL) == 0, "a second verb for the same owner registers");
	CHECK(ipc_register("t2", "raw", "", raw, NULL) == 0, "another owner registers");
	CHECK(ipc_has_verb("probe") && ipc_has_verb("raw"), "registered verbs are visible");
	return 0;
}

static int wire(void)
{
	char reply[4096];

	const char *sp[] = { "probe", "-f", "DejaVu Sans" };
	CHECK(framed(3, sp, NULL, 0, reply, sizeof(reply)) == 0, "a framed command is delivered");
	CHECK(!strcmp(reply, "ok probed 3\n"), "an MM_OK reply is framed as 'ok ...\\n'");
	CHECK(g_argc == 3 && !strcmp(g_argv[2], "DejaVu Sans"), "a word with a space stays one word");

	const char *nl[] = { "probe", "a\nb", "c" };
	CHECK(framed(3, nl, NULL, 0, reply, sizeof(reply)) == 0, "a word with a newline is delivered");
	CHECK(g_argc == 3 && !strcmp(g_argv[1], "a\nb") && !strcmp(g_argv[2], "c"), "an embedded newline neither splits nor swallows");

	const char *bd[] = { "probe", "x" };
	CHECK(framed(2, bd, "\x01\x00\x02", 3, reply, sizeof(reply)) == 0, "a frame with a binary body is delivered");
	CHECK(g_argc == 2 && g_body_len == 3 && !memcmp(g_body, "\x01\x00\x02", 3), "body bytes, NUL included, arrive after the last word");

	CHECK(exchange("probe  hello\tworld\r\n", 20, reply, sizeof(reply)) == 0, "a bare header is accepted");
	CHECK(g_argc == 3 && !strcmp(g_argv[1], "hello") && !strcmp(g_argv[2], "world"), "a bare header splits on whitespace and drops CR");

	const char *em[] = { "probe", "" };
	CHECK(framed(2, em, NULL, 0, reply, sizeof(reply)) == 0 && g_argc == 2 && g_argv[1][0] == '\0', "an empty word is a word");

	int hits = g_hits;
	const char *ff[] = { "fails" };
	CHECK(framed(1, ff, NULL, 0, reply, sizeof(reply)) == 0 && !strcmp(reply, "err nope\n"), "MM_ERR frames as 'err ...' with one trailing newline");
	const char *rw[] = { "raw" };
	CHECK(framed(1, rw, NULL, 0, reply, sizeof(reply)) == 0 && !strcmp(reply, "line one\nline two\n"), "a raw reply is sent verbatim");
	const char *un[] = { "nosuch" };
	CHECK(framed(1, un, NULL, 0, reply, sizeof(reply)) == 0 && !strcmp(reply, "err unknown verb: nosuch\n"), "an unknown verb is an error");
	CHECK(g_hits == hits, "other verbs never reach the probe");

	CHECK(exchange("probe argc=2\nonly-one\0", 22, reply, sizeof(reply)) == 0 && !strncmp(reply, "err bad frame", 13), "a frame short of its words is rejected");
	CHECK(exchange("probe argc=1\nother\0", 19, reply, sizeof(reply)) == 0 && !strncmp(reply, "err bad frame", 13), "a header that disagrees with arg0 is rejected");
	CHECK(exchange("probe argc=999\n", 15, reply, sizeof(reply)) == 0 && !strncmp(reply, "err bad frame", 13), "an absurd argc is rejected");
	CHECK(exchange("no newline at all", 17, reply, sizeof(reply)) == 0 && !strncmp(reply, "err bad frame", 13), "a header without a newline is rejected");

	static char big[IPC_FRAME_MAX + 16];
	memset(big, 'a', sizeof(big));
	CHECK(exchange(big, sizeof(big), reply, sizeof(reply)) == 0 && !strncmp(reply, "err bad frame", 13), "an oversized header is rejected");

	const char *hp[] = { "help" };
	CHECK(framed(1, hp, NULL, 0, reply, sizeof(reply)) == 0, "help answers");
	CHECK(strstr(reply, "probe [x]") && strstr(reply, "raw") && strstr(reply, "help"), "help lists every verb with its usage");
	CHECK(hits == g_hits, "help does not dispatch to verbs");
	return 0;
}

static int lifecycle(void)
{
	CHECK(ipc_start(s_sock) != 0, "a second listener on a live socket is refused");
	ipc_stop();
	CHECK(access(s_sock, F_OK) != 0, "stop removes the socket file");
	ipc_stop();

	ipc_unregister_owner("t");
	CHECK(!ipc_has_verb("probe") && !ipc_has_verb("fails"), "unregistering an owner drops all its verbs");
	CHECK(ipc_has_verb("raw"), "other owners keep theirs");

	/* Stale socket file from a crash. */
	FILE *f = fopen(s_sock, "w");
	if (f) fclose(f);
	CHECK(ipc_start(s_sock) == 0, "a stale socket file is replaced");
	ipc_stop();
	ipc_unregister_owner("t2");
	return 0;
}

int main(void)
{
	snprintf(s_sock, sizeof(s_sock), "/tmp/mm-ipc-test.%d.sock", (int)getpid());
	unlink(s_sock);
	RUN(registry);
	CHECK(ipc_start(s_sock) == 0, "the listener starts");
	RUN(wire);
	RUN(lifecycle);
	CHECK_PASS("test-ipc");
}

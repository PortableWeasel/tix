/*
 * tix-web: serves the repository in the current directory (docs/architecture.md section 13).
 * Usually started through `tix web`. Loopback only in M6.
 */
#include "app.h"
#include "tix/index_sqlite.h"
#include "tix/store_libgit.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char usage[] =
	"usage: tix web [--listen 127.0.0.1:7480] [--read-only] [--no-open]\n"
	"  --listen ADDR:PORT  127.0.0.1, [::1] or localhost; port 0 picks a free one\n"
	"                      (default 7480, or the next free port up to 7499)\n"
	"  --read-only         serve pages but accept no changes\n"
	"  --no-open           don't start a browser\n";

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static int64_t env_now(void *ctx)
{
	(void)ctx;
	return (int64_t)time(NULL);
}

static void env_random(void *ctx, uint8_t *out, size_t n)
{
	(void)ctx;
	size_t got = 0;
	while (got < n) {
		ssize_t r = getrandom(out + got, n - got, 0);
		if (r < 0) {
			perror("tix web: getrandom");
			abort(); /* never hand out a predictable token or nonce */
		}
		got += (size_t)r;
	}
}

static tix_date today(void *ctx)
{
	(void)ctx;
	const char *fixed = getenv("TIX_TODAY");
	tix_date d;
	if (fixed && tix_date_parse(fixed, &d))
		return d;
	time_t now = time(NULL);
	struct tm tm;
	char buf[16];
	if (!localtime_r(&now, &tm) || !strftime(buf, sizeof(buf), "%Y-%m-%d", &tm) || !tix_date_parse(buf, &d))
		return tix_date_from_unix((int64_t)now);
	return d;
}

typedef struct runner {
	web_app *app;
	char host[64];
	bool open_browser;
	bool listening;
} runner;

static void open_url(const char *url)
{
	if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY"))
		return;
	pid_t p = fork();
	if (p == 0) {
		/* a double fork, so the browser isn't our child */
		if (fork() == 0) {
			execlp("xdg-open", "xdg-open", url, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	if (p > 0)
		waitpid(p, NULL, 0);
}

static void on_listen(void *vr, int port)
{
	runner *r = vr;
	r->listening = true;
	web_app_set_listen(r->app, r->host, port);
	char url[256];
	snprintf(url, sizeof(url), "http://%s/login?t=%s", r->app->hosts[0], r->app->token);
	printf("tix web: %s\n", url);
	printf("tix web: open that URL in a browser (it logs you in); Ctrl-C stops the server\n");
	fflush(stdout);
	if (r->open_browser)
		open_url(url);
}

/* "127.0.0.1:7480", "[::1]:7480", "localhost:7480" -> host for binding and port. */
static bool parse_listen(const char *s, char *host, size_t hn, int *port)
{
	const char *colon = strrchr(s, ':');
	if (!colon || !colon[1])
		return false;
	char *end;
	long p = strtol(colon + 1, &end, 10);
	if (*end || p < 0 || p > 65535)
		return false;
	size_t n = (size_t)(colon - s);
	char h[64];
	if (n >= sizeof(h))
		return false;
	memcpy(h, s, n);
	h[n] = '\0';
	if (!strcmp(h, "localhost") || !strcmp(h, "127.0.0.1"))
		snprintf(host, hn, "127.0.0.1");
	else if (!strcmp(h, "[::1]"))
		snprintf(host, hn, "::1");
	else
		return false;
	*port = (int)p;
	return true;
}

int main(int argc, char **argv)
{
	char host[64] = "127.0.0.1";
	int port = 7480;
	bool fixed_port = false, read_only = false, open_browser = true;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--listen") && i + 1 < argc) {
			if (!parse_listen(argv[++i], host, sizeof(host), &port)) {
				fprintf(stderr, "tix web: --listen takes 127.0.0.1:PORT, [::1]:PORT or localhost:PORT; "
						"other addresses wait for the hosted mode (docs/architecture.md 13.8)\n");
				return 2;
			}
			fixed_port = true;
		} else if (!strcmp(argv[i], "--read-only")) {
			read_only = true;
		} else if (!strcmp(argv[i], "--no-open")) {
			open_browser = false;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			fputs(usage, stdout);
			return 0;
		} else {
			fprintf(stderr, "tix web: unexpected '%s'\n%s", argv[i], usage);
			return 2;
		}
	}

	tix_error err = TIX_ERROR_INIT;
	tix_store *store = NULL;
	tix_index *index = NULL;
	tix_repo *repo = NULL;
	if (tix_store_libgit_open(&store, &err)) {
		fprintf(stderr, "tix web: %s\n", err.msg);
		return 1;
	}
	tix_buf path = TIX_BUF_INIT;
	const char *off = getenv("TIX_NO_INDEX");
	if ((off && *off && strcmp(off, "0")) || tix_store_libgit_local_path(store, "index.db", &path, NULL) ||
	    tix_index_sqlite_open(path.data, &index, NULL))
		index = NULL;
	tix_buf_free(&path);
	tix_repo_config cfg = {.store = store, .env = {env_now, env_random, NULL}, .index = index};
	tix_buf vb = TIX_BUF_INIT;
	if (!tix_store_libgit_config_get(store, "tix.verify", &vb, NULL)) {
		bool vok;
		cfg.verify = tix_verify_policy_parse(vb.data, &vok); /* signatures on sync (§14.3) */
		if (!vok)
			fprintf(stderr, "tix web: ignoring tix.verify '%s' (off, warn or require)\n", vb.data);
	}
	tix_buf_free(&vb);
	tix_buf idb = TIX_BUF_INIT;
	if (!tix_store_libgit_config_get(store, "tix.identity", &idb, NULL) &&
	    !tix_oid_from_hex(&cfg.identity, idb.data, idb.len))
		memset(&cfg.identity, 0, sizeof(cfg.identity));
	tix_buf_free(&idb);
	if (tix_repo_open(&repo, &cfg, &err)) {
		memset(&cfg.identity, 0, sizeof(cfg.identity));
		fprintf(stderr, "tix web: configured tix.identity unusable (%s); changes will fail until one is set\n", err.msg);
		err = (tix_error)TIX_ERROR_INIT;
		if (tix_repo_open(&repo, &cfg, &err)) {
			fprintf(stderr, "tix web: %s\n", err.msg);
			return 1;
		}
	}

	static char remote[128] = "origin";
	tix_buf rb = TIX_BUF_INIT;
	if (!tix_store_libgit_config_get(store, "tix.remote", &rb, NULL) && rb.len && rb.len < sizeof(remote))
		snprintf(remote, sizeof(remote), "%s", rb.data);
	tix_buf_free(&rb);
	static web_app app; /* large; not on the stack */
	web_app_config acfg = {.repo = repo, .random = env_random, .today = today, .read_only = read_only,
			       .remote = remote};
	web_app_init(&app, &acfg);
	runner r = {.app = &app, .open_browser = open_browser};
	snprintf(r.host, sizeof(r.host), "%s", host);

	struct sigaction sa = {0};
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	int rc = 1;
	for (int attempt = 0; attempt < (fixed_port ? 1 : 20); attempt++) {
		char url[96];
		bool v6 = strchr(host, ':') != NULL;
		snprintf(url, sizeof(url), v6 ? "http://[%s]:%d" : "http://%s:%d", host, port + attempt);
		web_server_opts o = {.listen = url, .handler = web_app_handle, .app = &app, .on_listen = on_listen,
				     .listen_ctx = &r, .always_headers = app.always_headers,
				     .tick = web_app_tick, .tick_ms = 1000};
		err = (tix_error)TIX_ERROR_INIT;
		if (web_serve(&o, &stop, &err)) {
			rc = 0;
			break;
		}
	}
	if (!r.listening)
		fprintf(stderr, "tix web: %s\n", err.msg);
	tix_repo_close(repo);
	tix_index_destroy(index);
	tix_store_destroy(store);
	return rc;
}

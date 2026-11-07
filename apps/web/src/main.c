/*
 * tix-web: serves the repository in the current directory (docs/architecture.md section 13).
 * Usually started through `tix web`. Loopback only in M6.
 */
#include "app.h"
#include "tix/index_sqlite.h"
#include "tix/store_libgit.h"

#include <arpa/inet.h>
#include <errno.h>
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
	"  --no-open           don't start a browser\n"
	"  --pull-every 60s    pull from the remote (tix.remote, else origin) on this interval, so\n"
	"                      pushes from other clones show up (and in live pages) without a sync\n"
	"  --hook-secret-file F  accept POST /hooks/forgejo signed with the secret in F (a Forgejo\n"
	"                      push webhook, content type application/json), and pull at once\n"
	"hosted mode, behind a reverse proxy that signs people in (docs/architecture.md 13.8):\n"
	"  --trust-proxy HEADER  take the login from this request header (e.g. X-authentik-username)\n"
	"  --public-url URL      the proxy's address (https://tix.example.org): the only Host/Origin\n"
	"  --proxy-from ADDR     the proxy's address as seen here (repeatable; default 127.0.0.1, ::1)\n"
	"  --users FILE          lines \"LOGIN IDENTITY-ID\": who may write, as whom (others read)\n"
	"  --logout-url URL      the proxy's sign-out address, shown as a link\n"
	"  (--listen then takes any address, e.g. 0.0.0.0:7480 for a proxy in a container)\n";

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
	if (r->app->cfg.hosted) {
		printf("tix web: hosted on %s port %d for %s (logins from %s, %zu may write)\n", r->host, port,
		       r->app->cfg.hosted->origin, r->app->cfg.hosted->header, r->app->cfg.hosted->nusers);
		fflush(stdout);
		return;
	}
	char url[256];
	snprintf(url, sizeof(url), "http://%s/login?t=%s", r->app->hosts[0], r->app->token);
	printf("tix web: %s\n", url);
	printf("tix web: open that URL in a browser (it logs you in); Ctrl-C stops the server\n");
	fflush(stdout);
	if (r->open_browser)
		open_url(url);
}

/* "127.0.0.1:7480", "[::1]:7480", "localhost:7480" -> host for binding and port. In hosted
 * mode any literal address ("0.0.0.0:7480", "[fd00::5]:7480"). */
static bool parse_listen(const char *s, char *host, size_t hn, int *port, bool any)
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
	if (!strcmp(h, "localhost") || !strcmp(h, "127.0.0.1")) {
		snprintf(host, hn, "127.0.0.1");
	} else if (!strcmp(h, "[::1]")) {
		snprintf(host, hn, "::1");
	} else if (any) {
		unsigned char buf[16];
		bool v6 = h[0] == '[' && n > 2 && h[n - 1] == ']';
		if (v6) {
			h[n - 1] = '\0';
			memmove(h, h + 1, n - 1);
		}
		if (inet_pton(v6 ? AF_INET6 : AF_INET, h, buf) != 1)
			return false;
		snprintf(host, hn, "%s", h);
	} else {
		return false;
	}
	*port = (int)p;
	return true;
}

typedef TIX_VEC(web_user) user_vec;

/* The users file: "LOGIN IDENTITY" per line, '#' comments. RETURNS false (message printed). */
static bool load_users(tix_repo *repo, const char *path, user_vec *out)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "tix web: --users %s: %s\n", path, strerror(errno));
		return false;
	}
	char line[512];
	int no = 0;
	bool ok = true;
	while (ok && fgets(line, sizeof(line), f)) {
		no++;
		char *s = line + strspn(line, " \t");
		if (!*s || *s == '#' || *s == '\n')
			continue;
		char login[WEB_MAX_USER + 1], id[TIX_OID_MAX_HEX + 2];
		web_user u;
		tix_error e = TIX_ERROR_INIT;
		if (sscanf(s, "%128s %65s", login, id) != 2) {
			fprintf(stderr, "tix web: %s:%d: want \"LOGIN IDENTITY-ID\"\n", path, no);
			ok = false;
		} else if (tix_entity_resolve(repo, TIX_ENTITY_IDENTITY, id, &u.identity, &e)) {
			fprintf(stderr, "tix web: %s:%d: %s\n", path, no, e.msg);
			ok = false;
		} else {
			u.login = tix_xstrdup(login);
			TIX_VEC_PUSH(out, u);
		}
	}
	fclose(f);
	return ok;
}

int main(int argc, char **argv)
{
	char host[64] = "127.0.0.1";
	int port = 7480;
	bool fixed_port = false, read_only = false, open_browser = true;
	const char *listen = NULL, *users_file = NULL, *hook_file = NULL;
	int pull_every = 0;
	static web_hosted hosted;
	static const char *from[16];
	size_t nfrom = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--listen") && i + 1 < argc) {
			listen = argv[++i];
			fixed_port = true;
		} else if (!strcmp(argv[i], "--trust-proxy") && i + 1 < argc) {
			hosted.header = argv[++i];
		} else if (!strcmp(argv[i], "--public-url") && i + 1 < argc) {
			hosted.origin = argv[++i];
		} else if (!strcmp(argv[i], "--proxy-from") && i + 1 < argc) {
			if (nfrom == sizeof(from) / sizeof(*from)) {
				fprintf(stderr, "tix web: at most 16 --proxy-from\n");
				return 2;
			}
			from[nfrom++] = argv[++i];
		} else if (!strcmp(argv[i], "--users") && i + 1 < argc) {
			users_file = argv[++i];
		} else if (!strcmp(argv[i], "--logout-url") && i + 1 < argc) {
			hosted.logout_url = argv[++i];
		} else if (!strcmp(argv[i], "--read-only")) {
			read_only = true;
		} else if (!strcmp(argv[i], "--no-open")) {
			open_browser = false;
		} else if (!strcmp(argv[i], "--pull-every") && i + 1 < argc) {
			char *end;
			long v = strtol(argv[++i], &end, 10);
			if (*end == 'm')
				v *= 60, end++;
			else if (*end == 's')
				end++;
			if (*end || v < 10 || v > 86400) {
				fprintf(stderr, "tix web: --pull-every takes 10s to 24h, like 60s or 5m\n");
				return 2;
			}
			pull_every = (int)v;
		} else if (!strcmp(argv[i], "--hook-secret-file") && i + 1 < argc) {
			hook_file = argv[++i];
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			fputs(usage, stdout);
			return 0;
		} else {
			fprintf(stderr, "tix web: unexpected '%s'\n%s", argv[i], usage);
			return 2;
		}
	}

	bool is_hosted = hosted.header != NULL;
	if (!is_hosted && (hosted.origin || nfrom || users_file || hosted.logout_url)) {
		fprintf(stderr, "tix web: --public-url, --proxy-from, --users and --logout-url need --trust-proxy\n");
		return 2;
	}
	if (is_hosted) {
		size_t hl = strlen(hosted.header);
		if (!hl || hl > 64 || strspn(hosted.header, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != hl) {
			fprintf(stderr, "tix web: --trust-proxy takes a header name\n");
			return 2;
		}
		if (!hosted.origin || (strncmp(hosted.origin, "https://", 8) && strncmp(hosted.origin, "http://127.0.0.1", 16) &&
				       strncmp(hosted.origin, "http://localhost", 16)) ||
		    strpbrk(hosted.origin + 8, "/?#\"<> ")) {
			fprintf(stderr, "tix web: hosted mode needs --public-url https://HOST[:PORT] (no path)\n");
			return 2;
		}
		if (!nfrom) {
			from[nfrom++] = "127.0.0.1";
			from[nfrom++] = "::1";
		}
		hosted.from = from;
		hosted.nfrom = nfrom;
		open_browser = false;
	}
	if (listen && !parse_listen(listen, host, sizeof(host), &port, is_hosted)) {
		fprintf(stderr, is_hosted ? "tix web: --listen takes ADDRESS:PORT\n"
					  : "tix web: --listen takes 127.0.0.1:PORT, [::1]:PORT or localhost:PORT; "
					    "other addresses need the hosted mode (--trust-proxy, docs/architecture.md 13.8)\n");
		return 2;
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
	user_vec users = {0};
	if (is_hosted && users_file && !load_users(repo, users_file, &users)) {
		tix_repo_close(repo);
		tix_index_destroy(index);
		tix_store_destroy(store);
		return 2;
	}
	hosted.users = users.v;
	hosted.nusers = users.n;
	static uint8_t secret[256];
	size_t secret_len = 0;
	if (hook_file) {
		FILE *f = fopen(hook_file, "r");
		if (f) {
			secret_len = fread(secret, 1, sizeof(secret), f);
			fclose(f);
		}
		while (secret_len && (secret[secret_len - 1] == '\n' || secret[secret_len - 1] == '\r'))
			secret_len--;
		if (!f || secret_len < 16 || secret_len == sizeof(secret)) {
			fprintf(stderr, "tix web: --hook-secret-file needs a readable file holding 16 to 255 bytes\n");
			tix_repo_close(repo);
			tix_index_destroy(index);
			tix_store_destroy(store);
			return 2;
		}
	}
	static web_app app; /* large; not on the stack */
	web_app_config acfg = {.repo = repo, .random = env_random, .today = today, .now = env_now, .read_only = read_only,
			       .remote = remote, .hosted = is_hosted ? &hosted : NULL, .pull_every = pull_every,
			       .hook_secret = hook_file ? secret : NULL, .hook_secret_len = secret_len};
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
				     .tick = web_app_tick, .tick_ms = 1000, .tick_always = pull_every > 0,
				     .extra_header = hosted.header};
		err = (tix_error)TIX_ERROR_INIT;
		if (web_serve(&o, &stop, &err)) {
			rc = 0;
			break;
		}
	}
	if (!r.listening)
		fprintf(stderr, "tix web: %s\n", err.msg);
	for (size_t i = 0; i < users.n; i++)
		free((char *)users.v[i].login);
	free(users.v);
	web_app_free(&app);
	tix_repo_close(repo);
	tix_index_destroy(index);
	tix_store_destroy(store);
	return rc;
}

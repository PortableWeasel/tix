/*
 * End to end over real HTTP: tix-web on port 0 in a throwaway git repository (store_libgit,
 * SQLite cache), driven like a browser without JavaScript; the CLI writes next to it; then a
 * soak of a few thousand requests. Under ASan, tix-web must exit cleanly (no leaks) on SIGTERM.
 * argv[1] = tix, argv[2] = tix-web.
 */
#include "tix/testkit.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int port;
static char cookie[160], csrf[80], origin[64], hostport[64];
static const char *extra_hdr = ""; /* hosted mode: "X-User: ada\r\n" */

static int run(char *const argv[], char *out, size_t cap)
{
	int fd[2];
	if (pipe(fd))
		return -1;
	pid_t p = fork();
	if (p == 0) {
		dup2(fd[1], 1);
		dup2(fd[1], 2);
		close(fd[0]);
		execv(argv[0], argv);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(fd[1]);
	size_t n = 0;
	ssize_t r;
	while (out && n + 1 < cap && (r = read(fd[0], out + n, cap - 1 - n)) > 0)
		n += (size_t)r;
	if (out)
		out[n] = '\0';
	close(fd[0]);
	int st;
	waitpid(p, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* One request on its own connection. RETURNS the status; the whole reply is in out. */
static int http(const char *method, const char *path, const char *body, char *out, size_t cap)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a))) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	char req[8192];
	int n = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n%s", method, path, hostport,
			 extra_hdr);
	if (cookie[0])
		n += snprintf(req + n, sizeof(req) - (size_t)n, "Cookie: %s\r\n", cookie);
	if (body)
		n += snprintf(req + n, sizeof(req) - (size_t)n,
			      "Origin: %s\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n",
			      origin, strlen(body));
	n += snprintf(req + n, sizeof(req) - (size_t)n, "\r\n%s", body ? body : "");
	if (n >= (int)sizeof(req)) {
		close(fd);
		return -1;
	}
	if (write(fd, req, (size_t)n) != n) {
		close(fd);
		return -1;
	}
	size_t got = 0;
	ssize_t r;
	struct pollfd pf = {fd, POLLIN, 0};
	while (got + 1 < cap && poll(&pf, 1, 5000) > 0 && (r = read(fd, out + got, cap - 1 - got)) > 0)
		got += (size_t)r;
	out[got] = '\0';
	close(fd);
	int status = 0;
	sscanf(out, "HTTP/1.1 %d", &status);
	return status;
}

static void grab(const char *hay, const char *start, const char *stop, char *out, size_t cap)
{
	const char *p = strstr(hay, start);
	out[0] = '\0';
	if (!p)
		return;
	p += strlen(start);
	size_t n = strcspn(p, stop);
	snprintf(out, cap, "%.*s", (int)(n < cap ? n : cap - 1), p);
}

#define CONTAINS(hay, needle)                                                                     \
	do {                                                                                      \
		if (!strstr((hay), (needle)))                                                     \
			tk_fail(__FILE__, __LINE__, "expected '%s' in:\n%.2000s", (needle), (hay)); \
	} while (0)

int main(int argc, char **argv)
{
	TIX_REQUIRE(argc == 3);
	char tix[1024], web[1024];
	TK_CHECK(realpath(argv[1], tix) && realpath(argv[2], web));
	char dir[] = "/tmp/tix-web-smoke-XXXXXX";
	TK_CHECK(mkdtemp(dir) != NULL);
	setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1);
	setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
	setenv("TIX_TODAY", "2026-11-09", 1);
	TK_CHECK(chdir(dir) == 0);
	char out[1 << 17];
	char *init[] = {"git", "init", "-q", ".", NULL};
	char *name[] = {"git", "config", "user.name", "Ada", NULL};
	char *mail[] = {"git", "config", "user.email", "ada@example.org", NULL};
	char *idnew[] = {tix, "id", "new", NULL};
	TK_CHECK(run(init, NULL, 0) == 0 && run(name, NULL, 0) == 0 && run(mail, NULL, 0) == 0);
	TK_CHECK(run(idnew, out, sizeof(out)) == 0);

	/* start the server; read the login URL it prints */
	int pfd[2];
	TK_CHECK(pipe(pfd) == 0);
	pid_t srv = fork();
	if (srv == 0) {
		dup2(pfd[1], 1);
		close(pfd[0]);
		execl(web, "tix-web", "--listen", "127.0.0.1:0", "--no-open", (char *)NULL);
		_exit(127);
	}
	close(pfd[1]);
	char line[512] = "";
	FILE *f = fdopen(pfd[0], "r");
	TK_CHECK(f && fgets(line, sizeof(line), f));
	char token[80] = "";
	TK_CHECK(sscanf(line, "tix web: http://127.0.0.1:%d/login?t=%70s", &port, token) == 2);
	snprintf(hostport, sizeof(hostport), "127.0.0.1:%d", port);
	snprintf(origin, sizeof(origin), "http://127.0.0.1:%d", port);

	char path[256];
	TK_CHECK(http("GET", "/tickets", NULL, out, sizeof(out)) == 401);
	snprintf(path, sizeof(path), "/login?t=%s", token);
	TK_CHECK(http("GET", path, NULL, out, sizeof(out)) == 303);
	char sid[100];
	grab(out, "Set-Cookie: tix_session=", ";", sid, sizeof(sid));
	snprintf(cookie, sizeof(cookie), "tix_session=%s", sid);
	TK_CHECK(http("GET", "/tickets", NULL, out, sizeof(out)) == 200);
	grab(out, "name=\"csrf\" value=\"", "\"", csrf, sizeof(csrf));
	TK_CHECK(strlen(csrf) == 64);

	/* create through the web */
	char body[1024];
	snprintf(body, sizeof(body), "csrf=%s&title=From+the+web&o.title=&duration=3&o.duration=&labels=web&o.labels=", csrf);
	TK_CHECK(http("POST", "/new", body, out, sizeof(out)) == 303);
	char loc[200];
	grab(out, "Location: ", "\r", loc, sizeof(loc));
	TK_CHECK(!strncmp(loc, "/t/", 3) && strlen(loc) == 3 + 40);
	TK_CHECK(http("GET", loc, NULL, out, sizeof(out)) == 200);
	CONTAINS(out, "<h1>From the web</h1>");

	/* the CLI writes while the server runs: the next page sees it (refresh) */
	char *cli_new[] = {tix, "new", "-t", "From the CLI", NULL};
	TK_CHECK(run(cli_new, out, sizeof(out)) == 0);
	TK_CHECK(http("GET", "/tickets", NULL, out, sizeof(out)) == 200);
	CONTAINS(out, "From the CLI");
	CONTAINS(out, "From the web");

	/* edit through the web; the CLI sees it */
	char head[64];
	TK_CHECK(http("GET", loc, NULL, out, sizeof(out)) == 200);
	grab(out, "name=\"head\" value=\"", "\"", head, sizeof(head));
	snprintf(body, sizeof(body), "csrf=%s&head=%s&status=in-progress&o.status=open", csrf, head);
	TK_CHECK(http("POST", loc, body, out, sizeof(out)) == 303);
	char *show[] = {tix, "show", loc + 3, NULL};
	TK_CHECK(run(show, out, sizeof(out)) == 0);
	CONTAINS(out, "in-progress");
	snprintf(body, sizeof(body), "csrf=%s&body=Hello+from+HTTP", csrf);
	snprintf(path, sizeof(path), "%s/comment", loc);
	TK_CHECK(http("POST", path, body, out, sizeof(out)) == 303);
	TK_CHECK(run(show, out, sizeof(out)) == 0);
	CONTAINS(out, "Hello from HTTP");

	/* live refresh: an open event stream hears about a CLI write within a couple of ticks */
	{
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		TK_CHECK(fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
		char req[512], ev[4096] = "";
		int n = snprintf(req, sizeof(req), "GET /events HTTP/1.1\r\nHost: %s\r\nCookie: %s\r\n\r\n", hostport,
				 cookie);
		TK_CHECK(write(fd, req, (size_t)n) == n);
		size_t got = 0;
		struct pollfd pf = {fd, POLLIN, 0};
		while (!strstr(ev, "\n\n") && poll(&pf, 1, 3000) > 0) {
			ssize_t r = read(fd, ev + got, sizeof(ev) - 1 - got);
			if (r <= 0)
				break;
			got += (size_t)r;
			ev[got] = '\0';
			if (strstr(ev, "data: ") && strstr(strstr(ev, "data: "), "\n\n"))
				break;
		}
		CONTAINS(ev, "text/event-stream");
		char first[20] = "";
		grab(ev, "data: ", "\n", first, sizeof(first));
		TK_CHECK(strlen(first) == 16);
		char *cli_more[] = {tix, "new", "-t", "Seen live", NULL};
		TK_CHECK(run(cli_more, out, sizeof(out)) == 0);
		got = 0;
		ev[0] = '\0';
		while (!(strstr(ev, "event: stamp\ndata: ") && strstr(strstr(ev, "event: stamp\ndata: "), "\n\n")) &&
		       poll(&pf, 1, 4000) > 0) {
			ssize_t r = read(fd, ev + got, sizeof(ev) - 1 - got);
			if (r <= 0)
				break;
			got += (size_t)r;
			ev[got] = '\0';
		}
		CONTAINS(ev, "event: tickets\ndata: {\"changed\":[{\"id\":\""); /* which ticket moved, first */
		CONTAINS(ev, "\"title\":\"Seen live\"");
		char second[20] = "";
		grab(ev, "event: stamp\ndata: ", "\n", second, sizeof(second));
		TK_CHECK(strlen(second) == 16 && strcmp(first, second));
		close(fd);
	}

	/* a forged form from elsewhere is refused */
	char saved[64];
	snprintf(saved, sizeof(saved), "%s", origin);
	snprintf(origin, sizeof(origin), "http://evil.example");
	TK_CHECK(http("POST", "/new", body, out, sizeof(out)) == 403);
	snprintf(origin, sizeof(origin), "%s", saved);

	/* soak: every page, many times, plus hostile requests (leaks show at exit under ASan) */
	static const char *const pages[] = {"/tickets", "/wbs", "/plan", "/load", "/res", "/cal", "/id",
					    "/export?format=json", "/tickets?q=web", "/no/such", "/t/zzz"};
	int bad = 0;
	for (int i = 0; i < 3000; i++) {
		const char *pg = i % 50 == 49 ? loc : pages[i % (sizeof(pages) / sizeof(*pages))];
		int st = http("GET", pg, NULL, out, sizeof(out));
		if (st != 200 && st != 404)
			bad++;
		if (i % 500 == 0) {
			snprintf(body, sizeof(body), "csrf=%s&title=Soak+%d&o.title=", csrf, i);
			if (http("POST", "/new", body, out, sizeof(out)) != 303)
				bad++;
		}
	}
	TK_CHECK(bad == 0);

	int status;
	kill(srv, SIGTERM);
	TK_CHECK(waitpid(srv, &status, 0) == srv);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		tk_fail(__FILE__, __LINE__, "tix-web exited with %d (ASan/LSan reports above?)",
			WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	fclose(f);

	/* hosted mode (§13.8): behind a proxy on 127.0.0.1 that sends X-User; ada may write */
	{
		char *idls[] = {tix, "id", "ls", NULL};
		TK_CHECK(run(idls, out, sizeof(out)) == 0);
		char ada[16] = "";
		grab(out, "* ", " ", ada, sizeof(ada)); /* "* <short id>  name <email>": the current identity */
		FILE *uf = fopen("users", "w");
		TK_CHECK(uf != NULL);
		if (uf) {
			fprintf(uf, "# login identity\nada %s\n", ada);
			fclose(uf);
		}
		int hp[2];
		TK_CHECK(pipe(hp) == 0);
		pid_t hs = fork();
		if (hs == 0) {
			dup2(hp[1], 1);
			close(hp[0]);
			execl(web, "tix-web", "--listen", "127.0.0.1:0", "--trust-proxy", "X-User", "--public-url",
			      "https://tix.test", "--users", "users", (char *)NULL);
			_exit(127);
		}
		close(hp[1]);
		FILE *hf = fdopen(hp[0], "r");
		char hl[512] = "";
		TK_CHECK(hf && fgets(hl, sizeof(hl), hf));
		TK_CHECK(sscanf(hl, "tix web: hosted on 127.0.0.1 port %d", &port) == 1);
		CONTAINS(hl, "1 may write");
		snprintf(hostport, sizeof(hostport), "tix.test");
		snprintf(origin, sizeof(origin), "https://tix.test");
		cookie[0] = '\0';
		TK_CHECK(http("GET", "/tickets", NULL, out, sizeof(out)) == 401);
		extra_hdr = "X-User: ada\r\n";
		TK_CHECK(http("GET", "/tickets", NULL, out, sizeof(out)) == 200);
		CONTAINS(out, "<span class=\"user\">ada</span>");
		grab(out, "name=\"csrf\" value=\"", "\"", csrf, sizeof(csrf));
		snprintf(body, sizeof(body), "csrf=%s&title=Hosted+write&o.title=", csrf);
		TK_CHECK(http("POST", "/new", body, out, sizeof(out)) == 303);
		grab(out, "Location: ", "\r", loc, sizeof(loc));
		char *hshow[] = {tix, "show", loc + 3, NULL};
		TK_CHECK(run(hshow, out, sizeof(out)) == 0);
		CONTAINS(out, "Hosted write");
		extra_hdr = "X-User: mallory\r\n";
		TK_CHECK(http("GET", "/tickets", NULL, out, sizeof(out)) == 200);
		grab(out, "name=\"csrf\" value=\"", "\"", csrf, sizeof(csrf));
		snprintf(body, sizeof(body), "csrf=%s&title=Nope&o.title=", csrf);
		TK_CHECK(http("POST", "/new", body, out, sizeof(out)) == 403);
		extra_hdr = "";
		kill(hs, SIGTERM);
		TK_CHECK(waitpid(hs, &status, 0) == hs && WIFEXITED(status) && WEXITSTATUS(status) == 0);
		fclose(hf);
	}
	char *rm[] = {"rm", "-rf", dir, NULL};
	run(rm, NULL, 0);
	return tk_finish();
}

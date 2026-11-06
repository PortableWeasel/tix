/*
 * The Mongoose adapter over real sockets: a child process runs web_serve on 127.0.0.1
 * port 0 with an echo handler; the parent speaks raw HTTP to it, including hostile input.
 */
#include "tix/testkit.h"
#include "web_http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int port_pipe[2];
static volatile sig_atomic_t stop_flag;

static void on_term(int sig)
{
	(void)sig;
	stop_flag = 1;
}

static void echo(void *app, const web_request *req, web_response *res)
{
	(void)app;
	web_header(res, "Content-Type", "text/plain; charset=utf-8");
	tix_buf_addf(&res->body, "%s %s\n", req->method, req->path);
	for (size_t i = 0; i < req->query.n; i++)
		tix_buf_addf(&res->body, "q:%s=%s\n", req->query.v[i].name, req->query.v[i].value);
	for (size_t i = 0; i < req->form.n; i++)
		tix_buf_addf(&res->body, "f:%s=%s\n", req->form.v[i].name, req->form.v[i].value);
	if (req->host)
		tix_buf_addf(&res->body, "host:%s\n", req->host);
	if (!strcmp(req->path, "/teapot"))
		res->status = 404;
}

static void on_listen(void *app, int port)
{
	(void)app;
	if (write(port_pipe[1], &port, sizeof(port)) != sizeof(port))
		_exit(3);
}

static pid_t start_server(int *port)
{
	TK_CHECK(pipe(port_pipe) == 0);
	pid_t p = fork();
	if (p == 0) {
		signal(SIGTERM, on_term);
		signal(SIGPIPE, SIG_IGN);
		web_server_opts o = {.listen = "http://127.0.0.1:0", .handler = echo, .on_listen = on_listen,
				     .always_headers = "X-Content-Type-Options: nosniff\r\n", .idle_ms = 400,
				     .max_connections = 8};
		tix_error e = TIX_ERROR_INIT;
		_exit(web_serve(&o, &stop_flag, &e) ? 0 : 2);
	}
	TK_CHECK(read(port_pipe[0], port, sizeof(*port)) == sizeof(*port) && *port > 0);
	close(port_pipe[0]);
	close(port_pipe[1]);
	return p;
}

static int dial(int port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	TK_CHECK(fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
	return fd;
}

static void say(int fd, const char *s, size_t n)
{
	while (n) {
		ssize_t w = write(fd, s, n);
		if (w <= 0)
			return; /* the server may close early on purpose */
		s += w;
		n -= (size_t)w;
	}
}

/* Reads until the peer closes, `want` bytes arrive, or ms pass. */
static size_t hear(int fd, char *out, size_t cap, int ms, size_t want)
{
	size_t n = 0;
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (n + 1 < cap) {
		struct timespec t1;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		int left = ms - (int)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
		struct pollfd p = {fd, POLLIN, 0};
		if (left <= 0 || poll(&p, 1, left) <= 0)
			break;
		ssize_t r = read(fd, out + n, cap - 1 - n);
		if (r <= 0)
			break;
		n += (size_t)r;
		if (want && n >= want)
			break;
	}
	out[n] = '\0';
	return n;
}

static bool closed(int fd, int ms)
{
	char b[64];
	struct pollfd p = {fd, POLLIN, 0};
	if (poll(&p, 1, ms) <= 0)
		return false;
	return read(fd, b, sizeof(b)) == 0;
}

/* One request on a fresh connection; RETURNS the whole reply in out. */
static void exchange(int port, const char *req, char *out, size_t cap)
{
	int fd = dial(port);
	say(fd, req, strlen(req));
	hear(fd, out, cap, 2000, 0);
	close(fd);
}

#define CONTAINS(hay, needle)                                                                   \
	do {                                                                                    \
		if (!strstr((hay), (needle)))                                                   \
			tk_fail(__FILE__, __LINE__, "expected '%s' in:\n%s", (needle), (hay)); \
	} while (0)

int main(void)
{
	int port;
	pid_t srv = start_server(&port);
	char out[65536];

	exchange(port, "GET /echo?q=hi&x=%E2%9C%93 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", out,
		 sizeof(out));
	CONTAINS(out, "HTTP/1.1 200 OK\r\n");
	CONTAINS(out, "X-Content-Type-Options: nosniff\r\n");
	CONTAINS(out, "GET /echo\nq:q=hi\nq:x=\xe2\x9c\x93\nhost:127.0.0.1\n");

	const char *post = "POST /t/1 HTTP/1.1\r\nHost: h\r\nContent-Type: application/x-www-form-urlencoded\r\n"
			   "Content-Length: 27\r\nConnection: close\r\n\r\ntitle=A+%26+B&body=x%0D%0Ay";
	exchange(port, post, out, sizeof(out));
	CONTAINS(out, "f:title=A & B\nf:body=x\ny\n");

	exchange(port, "GET /teapot HTTP/1.1\r\nConnection: close\r\n\r\n", out, sizeof(out));
	CONTAINS(out, "HTTP/1.1 404 Not Found\r\n");

	/* HEAD: headers with the length, no body */
	exchange(port, "HEAD /echo HTTP/1.1\r\nConnection: close\r\n\r\n", out, sizeof(out));
	CONTAINS(out, "Content-Length: 11\r\n");
	TK_CHECK(!strstr(out, "HEAD /echo\n"));

	/* keep-alive: two requests on one connection, both answered */
	int fd = dial(port);
	const char *two = "GET /one HTTP/1.1\r\nHost: h\r\n\r\nGET /two HTTP/1.1\r\nHost: h\r\n\r\n";
	say(fd, two, strlen(two));
	hear(fd, out, sizeof(out), 1000, 0);
	CONTAINS(out, "GET /one\n");
	CONTAINS(out, "GET /two\n");
	close(fd);

	/* refused before the body is read: too large, chunked, bad length */
	exchange(port, "POST /x HTTP/1.1\r\nContent-Type: application/x-www-form-urlencoded\r\n"
		       "Content-Length: 99999999\r\n\r\n", out, sizeof(out));
	CONTAINS(out, "HTTP/1.1 413 ");
	exchange(port, "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\na=1\r\n0\r\n\r\n", out, sizeof(out));
	CONTAINS(out, "HTTP/1.1 501 ");
	exchange(port, "POST /x HTTP/1.1\r\nContent-Length: -1\r\n\r\n", out, sizeof(out));
	TK_CHECK(!*out || strstr(out, "HTTP/1.1 400 ")); /* Mongoose's parser drops it, or we answer 400 */
	exchange(port, "POST /x HTTP/1.1\r\nContent-Length: 12x\r\n\r\n", out, sizeof(out));
	TK_CHECK(!*out || strstr(out, "HTTP/1.1 400 "));
	exchange(port, "POST /x HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}", out, sizeof(out));
	CONTAINS(out, "HTTP/1.1 415 ");
	exchange(port, "GET /x?a=%zz HTTP/1.1\r\n\r\n", out, sizeof(out));
	CONTAINS(out, "HTTP/1.1 400 ");

	/* garbage, a NUL in the request line, many headers: answered or dropped, never fatal */
	exchange(port, "\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03\r\n\r\n", out, sizeof(out));
	TK_CHECK(!strstr(out, "200 OK"));
	char nul[] = "GET /a\0b HTTP/1.1\r\n\r\n";
	fd = dial(port);
	say(fd, nul, sizeof(nul) - 1);
	hear(fd, out, sizeof(out), 1000, 0);
	TK_CHECK(!strstr(out, "200 OK"));
	close(fd);
	tix_buf many = TIX_BUF_INIT;
	tix_buf_adds(&many, "GET /many HTTP/1.1\r\n");
	for (int i = 0; i < 200; i++)
		tix_buf_addf(&many, "X-H%d: %d\r\n", i, i);
	tix_buf_adds(&many, "Connection: close\r\n\r\n");
	exchange(port, many.data, out, sizeof(out));
	tix_buf_free(&many);

	/* idle connections are closed (400 ms in this test) */
	fd = dial(port);
	say(fd, "GET /slow", 9);
	TK_CHECK(closed(fd, 3000));
	close(fd);

	/* connection limit (8 here): the ninth gets 503, the first eight still work */
	int fds[9];
	for (int i = 0; i < 9; i++) {
		fds[i] = dial(port);
		usleep(20000); /* let the server accept them in order */
	}
	for (int i = 0; i < 8; i++)
		say(fds[i], "GET /k HTTP/1.1\r\nConnection: close\r\n\r\n", 39);
	hear(fds[8], out, sizeof(out), 1500, 0);
	CONTAINS(out, "HTTP/1.1 503 ");
	for (int i = 0; i < 8; i++) {
		hear(fds[i], out, sizeof(out), 2000, 0);
		CONTAINS(out, "200 OK");
	}
	for (int i = 0; i < 9; i++)
		close(fds[i]);

	/* still alive after all that */
	exchange(port, "GET /alive HTTP/1.1\r\nConnection: close\r\n\r\n", out, sizeof(out));
	CONTAINS(out, "GET /alive\n");

	int status;
	kill(srv, SIGTERM);
	TK_CHECK(waitpid(srv, &status, 0) == srv && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	return tk_finish();
}

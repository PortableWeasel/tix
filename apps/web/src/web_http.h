/*
 * The web app's view of HTTP (docs/architecture.md section 13.1). Handlers see a decoded
 * request and fill a response. Nothing here names the HTTP library: only http_mongoose.c
 * does, so handlers are plain functions testable without sockets.
 */
#ifndef TIX_WEB_HTTP_H
#define TIX_WEB_HTTP_H

#include "tix/base.h"
#include "tix/base_buf.h"

#include <signal.h>

/* Limits (section 13.6). Mongoose itself caps headers at 30 and a request at 256 KiB. */
#define WEB_MAX_BODY (256 * 1024 - 8 * 1024) /* request line + headers fit in the rest */
#define WEB_MAX_FIELDS 200                    /* query + form fields, each */
#define WEB_MAX_NAME 64                       /* field and cookie names */
#define WEB_MAX_CONNECTIONS 64
#define WEB_IDLE_MS 30000

typedef struct web_kv {
	char *name, *value; /* NUL-terminated, valid UTF-8, no NUL inside; owned by the request */
} web_kv;

typedef struct web_kvs {
	web_kv *v;
	size_t n;
} web_kvs;

typedef struct web_request {
	char *method;       /* "GET", "POST", ... */
	char *path;         /* raw path, no query; always starts with '/' */
	web_kvs query;      /* decoded application/x-www-form-urlencoded */
	web_kvs form;       /* decoded body, for POSTs with that content type; else empty */
	web_kvs cookies;
	char *host, *origin, *referer, *content_type; /* NULL when absent */
	bool form_body;     /* the body was a form (urlencoded); false for no body */
} web_request;

/* Raw request parts, as the HTTP library hands them over (not NUL-terminated). */
typedef struct web_raw {
	const char *method, *uri, *query, *body;
	size_t method_len, uri_len, query_len, body_len;
	const char *(*header)(void *ctx, const char *name, size_t *len); /* NULL when absent */
	void *ctx;
} web_raw;

/* Decodes raw into req. RETURNS false with a short reason in why (for a 400) on malformed
 * or over-limit input: bad %-escapes, invalid UTF-8, NUL bytes, too many fields, a name
 * that is too long or not [A-Za-z0-9._-], a body that isn't a form. Line endings in values
 * become LF. ENSURES req is always safe to web_request_free. */
bool web_request_decode(const web_raw *raw, web_request *req, char *why, size_t why_len);
void web_request_free(web_request *req);

/* First value of a field, or NULL. web_kvs_all collects every value (repeated fields). */
const char *web_kvs_get(const web_kvs *kv, const char *name);
size_t web_kvs_all(const web_kvs *kv, const char *name, const char **out, size_t max);

/* Decodes one application/x-www-form-urlencoded string (exposed for tests and fuzzing). */
bool web_urldecode_form(const char *s, size_t len, web_kvs *out, char *why, size_t why_len);
void web_kvs_free(web_kvs *kv);

typedef struct web_response {
	int status;      /* 200 unless set */
	tix_buf headers; /* "Name: value\r\n" lines */
	tix_buf body;
} web_response;

#define WEB_RESPONSE_INIT {200, TIX_BUF_INIT, TIX_BUF_INIT}

/* REQUIRES name and value contain no CR, LF or NUL (response headers are ours). */
void web_header(web_response *res, const char *name, const char *value);
void web_response_free(web_response *res);

typedef void (*web_handler)(void *app, const web_request *req, web_response *res);

typedef struct web_server_opts {
	const char *listen;   /* "http://127.0.0.1:7480"; port 0 picks a free one */
	web_handler handler;
	void *app;
	/* Called once listening, with the bound port. */
	void (*on_listen)(void *listen_ctx, int port);
	void *listen_ctx;
	/* Called before each poll; returning false stops the loop. */
	bool (*keep_going)(void *app);
	/* Extra headers on every response, including errors the server makes itself. */
	const char *always_headers;
	int idle_ms;         /* 0: WEB_IDLE_MS */
	int max_connections; /* 0: WEB_MAX_CONNECTIONS */
} web_server_opts;

/* Runs the server on the calling thread until keep_going returns false or *stop is set
 * (signal handlers may set it). Every handler call happens on this thread. RETURNS
 * false (with a message in err) when the address can't be bound. */
bool web_serve(const web_server_opts *o, volatile sig_atomic_t *stop, tix_error *err);

#endif

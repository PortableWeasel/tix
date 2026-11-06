/*
 * The only file that includes mongoose.h (docs/architecture.md section 13.1, enforced by
 * cmake/Mongoose.cmake). Mongoose splits requests into method, path, query, headers and
 * body; request.c decodes them. Everything runs on the thread that calls web_serve.
 */
#include "mongoose.h"
#include "web_http.h"

#include <string.h>

typedef struct server {
	const web_server_opts *o;
	int connections;
} server;

/* Per-connection state lives in c->data (Mongoose's scratch space). */
typedef struct conn_state {
	uint64_t last_ms;
} conn_state;

_Static_assert(sizeof(conn_state) <= MG_DATA_SIZE, "conn_state must fit c->data");

static conn_state *state(struct mg_connection *c)
{
	return (conn_state *)(void *)c->data;
}

static const char *reason(int status)
{
	switch (status) {
	case 200: return "OK";
	case 303: return "See Other";
	case 304: return "Not Modified";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 409: return "Conflict";
	case 413: return "Content Too Large";
	case 415: return "Unsupported Media Type";
	case 421: return "Misdirected Request";
	case 422: return "Unprocessable Content";
	case 500: return "Internal Server Error";
	case 501: return "Not Implemented";
	case 503: return "Service Unavailable";
	default: return "Status";
	}
}

static void send_response(struct mg_connection *c, const server *sv, const web_response *res, bool head)
{
	mg_printf(c, "HTTP/1.1 %d %s\r\n", res->status, reason(res->status));
	if (res->headers.len)
		mg_send(c, res->headers.data, res->headers.len);
	if (sv->o->always_headers)
		mg_printf(c, "%s", sv->o->always_headers);
	mg_printf(c, "Content-Length: %lu\r\n\r\n", (unsigned long)res->body.len);
	if (!head && res->body.len)
		mg_send(c, res->body.data, res->body.len);
	/* The response is complete. Mongoose's own reply helpers clear this; without it the
	 * next pipelined request is never parsed and "Connection: close" isn't honoured. */
	c->is_resp = 0;
}

/* A response the server makes itself; then close once sent. */
static void refuse(struct mg_connection *c, const server *sv, int status, const char *text)
{
	web_response res = WEB_RESPONSE_INIT;
	res.status = status;
	web_header(&res, "Content-Type", "text/plain; charset=utf-8");
	web_header(&res, "Connection", "close");
	tix_buf_adds(&res.body, text);
	tix_buf_addc(&res.body, '\n');
	send_response(c, sv, &res, false);
	web_response_free(&res);
	c->is_draining = 1;
}

static const char *get_header(void *ctx, const char *name, size_t *len)
{
	struct mg_str *v = mg_http_get_header(ctx, name);
	if (!v)
		return NULL;
	*len = v->len;
	return v->buf ? v->buf : "";
}

static void on_message(struct mg_connection *c, server *sv, struct mg_http_message *hm)
{
	web_raw raw = {
		.method = hm->method.buf, .method_len = hm->method.len,
		.uri = hm->uri.buf, .uri_len = hm->uri.len,
		.query = hm->query.buf, .query_len = hm->query.len,
		.body = hm->body.buf, .body_len = hm->body.len,
		.header = get_header, .ctx = hm,
	};
	web_request req;
	char why[128] = "";
	if (!web_request_decode(&raw, &req, why, sizeof(why))) {
		web_request_free(&req);
		refuse(c, sv, strstr(why, "only form") ? 415 : strstr(why, "too large") ? 413 : 400, why);
		return;
	}
	web_response res = WEB_RESPONSE_INIT;
	sv->o->handler(sv->o->app, &req, &res);
	send_response(c, sv, &res, !strcmp(req.method, "HEAD"));
	web_response_free(&res);
	web_request_free(&req);
}

static void on_event(struct mg_connection *c, int ev, void *ev_data)
{
	server *sv = c->fn_data;
	switch (ev) {
	case MG_EV_ACCEPT:
		state(c)->last_ms = mg_millis();
		if (++sv->connections > (sv->o->max_connections ? sv->o->max_connections : WEB_MAX_CONNECTIONS)) {
			refuse(c, sv, 503, "too many connections"); /* sent, then closed */
		}
		break;
	case MG_EV_CLOSE:
		if (c->is_accepted)
			sv->connections--;
		break;
	case MG_EV_READ:
	case MG_EV_WRITE:
		state(c)->last_ms = mg_millis();
		break;
	case MG_EV_POLL:
		if (c->is_accepted && mg_millis() - state(c)->last_ms > (uint64_t)(sv->o->idle_ms ? sv->o->idle_ms : WEB_IDLE_MS))
			c->is_closing = 1;
		break;
	case MG_EV_HTTP_HDRS: {
		struct mg_http_message *hm = ev_data;
		struct mg_str *te = mg_http_get_header(hm, "Transfer-Encoding");
		struct mg_str *cl = mg_http_get_header(hm, "Content-Length");
		const char *bad = NULL;
		int status = 400;
		if (te) {
			bad = "chunked request bodies are not accepted", status = 501;
		} else if (cl) {
			unsigned long long n = 0;
			size_t i = 0;
			for (; i < cl->len && cl->buf[i] >= '0' && cl->buf[i] <= '9'; i++)
				if (n <= WEB_MAX_BODY) /* stop growing once over the limit */
					n = n * 10 + (unsigned long long)(cl->buf[i] - '0');
			if (i == 0 || i != cl->len)
				bad = "bad Content-Length";
			else if (n > WEB_MAX_BODY)
				bad = "body too large", status = 413;
		}
		if (bad) {
			refuse(c, sv, status, bad);
			c->recv.len = 0; /* drop the rest; Mongoose then detaches its HTTP parser */
		}
		break;
	}
	case MG_EV_HTTP_MSG:
		on_message(c, sv, ev_data);
		break;
	default:
		break;
	}
}

bool web_serve(const web_server_opts *o, volatile sig_atomic_t *stop, tix_error *err)
{
	TIX_REQUIRE(o && o->listen && o->handler);
	server sv = {o, 0};
	struct mg_mgr mgr;
	mg_mgr_init(&mgr);
	struct mg_connection *lc = mg_http_listen(&mgr, o->listen, on_event, &sv);
	if (!lc) {
		mg_mgr_free(&mgr);
		tix_error_set(err, TIX_ERR_IO, "cannot listen on %s", o->listen);
		return false;
	}
	if (o->on_listen)
		o->on_listen(o->app, (int)mg_ntohs(lc->loc.port));
	while (!(stop && *stop) && (!o->keep_going || o->keep_going(o->app)))
		mg_mgr_poll(&mgr, 200);
	mg_mgr_free(&mgr);
	return true;
}

/*
 * tix/bridge_forgejo.h -- a tix_tracker on a Forgejo (or Gitea) repository's issues, over
 * its REST API v1 with libcurl (architecture §14.4). The only module that includes curl.h.
 *
 * url: "https://host[:port]/owner/repo" (".git" and a trailing slash are ignored). Plain
 * http is accepted only for 127.0.0.1, [::1] and localhost (tests). token: an access token
 * with issue read/write scope, sent as "Authorization: token ..." to that host only:
 * redirects are never followed. Responses are untrusted: size-capped, strictly parsed.
 * The tracker key is "forgejo:host[:port]/owner/repo".
 */
#ifndef TIX_BRIDGE_FORGEJO_H
#define TIX_BRIDGE_FORGEJO_H

#include "tix/bridge.h"

/* RETURNS TIX_ERR_INVALID for a URL it won't use. ENSURES on TIX_OK: *out owned by the
 * caller (tix_tracker_destroy). No request is made yet. */
tix_status tix_bridge_forgejo_open(const char *url, const char *token, tix_tracker **out, tix_error *err);

/* Parses url into its parts (exposed for tests and for asking git's credential helpers).
 * RETURNS false for a URL tix_bridge_forgejo_open would refuse. */
bool tix_bridge_forgejo_parse(const char *url, tix_buf *scheme, tix_buf *host, tix_buf *owner, tix_buf *repo);

#endif

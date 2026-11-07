/*
 * Ticket summaries: the few fields lists filter and sort on (architecture §8.1), cached
 * next to each entity in the index so `tix_ticket_list` decodes only the tickets it
 * returns. The encoding is core's own and opaque to the index:
 *
 *   "tixsum <codec version> 1\n" then length-prefixed fields "<len>:<bytes>":
 *   status, stage ("" = none), author hex, parent hex ("" = top level), rank ("" = unranked), title,
 *   created, updated, max lamport, label count, labels..., assignee count, resource hexes...
 *
 * It is a cache of validated data written by this process, but the file is on disk, so
 * decode checks everything and any doubt is a miss (the entity is then loaded).
 */
#include "core_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void field(tix_buf *b, const char *s, size_t n)
{
	tix_buf_addf(b, "%zu:", n);
	tix_buf_add(b, s, n);
}

static void field_s(tix_buf *b, const char *s)
{
	field(b, s ? s : "", s ? strlen(s) : 0);
}

static void field_i(tix_buf *b, long long v)
{
	char n[32];
	field(b, n, (size_t)snprintf(n, sizeof(n), "%lld", v));
}

static void field_oid(tix_buf *b, const tix_oid *o)
{
	char hex[TIX_OID_MAX_HEX + 1];
	field_s(b, tix_oid_is_null(o) ? "" : tix_oid_hex(o, hex));
}

static void header(tix_buf *b)
{
	tix_buf_addf(b, "tixsum %d 1\n", TIX_ENTITY_CODEC_VERSION);
}

void core_summary_encode(const tix_entity *e, tix_buf *out)
{
	TIX_REQUIRE(e && e->type == TIX_ENTITY_TICKET && out);
	const tix_ticket *t = &e->u.ticket;
	tix_buf_reset(out);
	header(out);
	field_i(out, t->status);
	field_s(out, t->stage);
	field_oid(out, &t->author);
	field_oid(out, &t->plan.parent);
	field_s(out, t->plan.rank);
	field_s(out, t->title);
	field_i(out, e->created);
	field_i(out, e->updated);
	field_i(out, (long long)e->max_lamport); /* lists still advance the clock (as cache hits do) */
	field_i(out, (long long)t->labels.n);
	for (size_t i = 0; i < t->labels.n; i++)
		field_s(out, t->labels.v[i]);
	field_i(out, (long long)t->plan.nassignments);
	for (size_t i = 0; i < t->plan.nassignments; i++)
		field_oid(out, &t->plan.assignments[i].resource);
}

typedef struct reader {
	const char *p, *end;
	bool ok;
} reader;

/* Next field; sets ok = false (and returns "") on any malformation. */
static const char *next(reader *r, size_t *len)
{
	*len = 0;
	if (!r->ok)
		return "";
	size_t n = 0, digits = 0;
	while (r->p < r->end && *r->p >= '0' && *r->p <= '9' && digits < 9) {
		n = n * 10 + (size_t)(*r->p++ - '0');
		digits++;
	}
	if (!digits || r->p >= r->end || *r->p != ':' || (size_t)(r->end - r->p - 1) < n) {
		r->ok = false;
		return "";
	}
	const char *s = ++r->p;
	r->p += n;
	*len = n;
	return s;
}

static char *next_str(reader *r)
{
	size_t n;
	const char *s = next(r, &n);
	if (!r->ok || memchr(s, '\0', n))
		return r->ok = false, NULL;
	char *c = tix_xmalloc(n + 1);
	memcpy(c, s, n);
	c[n] = '\0';
	return c;
}

static long long next_int(reader *r, long long lo, long long hi)
{
	size_t n;
	const char *s = next(r, &n);
	char tmp[24];
	if (!r->ok || !n || n >= sizeof(tmp))
		return r->ok = false, 0;
	memcpy(tmp, s, n);
	tmp[n] = '\0';
	char *endp;
	errno = 0;
	long long v = strtoll(tmp, &endp, 10);
	if (errno || *endp || v < lo || v > hi)
		return r->ok = false, 0;
	return v;
}

/* An oid of the store's algorithm, or "" for null when allow_null. */
static void next_oid(reader *r, tix_hash_algo algo, bool allow_null, tix_oid *out)
{
	size_t n;
	const char *s = next(r, &n);
	memset(out, 0, sizeof(*out));
	if (!r->ok || (!n && allow_null))
		return;
	if (n != tix_oid_rawlen(algo) * 2 || strspn(s, "0123456789abcdef") < n || !tix_oid_from_hex(out, s, n))
		r->ok = false;
}

bool core_summary_decode(const char *data, size_t len, tix_hash_algo algo, core_facets *f)
{
	char want[32];
	int hl = snprintf(want, sizeof(want), "tixsum %d 1\n", TIX_ENTITY_CODEC_VERSION);
	memset(&f->status, 0, sizeof(*f) - offsetof(core_facets, status));
	if (!data || len < (size_t)hl || memcmp(data, want, (size_t)hl))
		return false;
	reader r = {data + hl, data + len, true};
	f->status = (tix_ticket_status)next_int(&r, 0, TIX_TICKET_CANCELLED);
	f->stage = next_str(&r);
	if (f->stage && !*f->stage) {
		free(f->stage);
		f->stage = NULL;
	} else if (f->stage && !tix_stage_valid(f->stage)) {
		r.ok = false;
	}
	next_oid(&r, algo, false, &f->author);
	next_oid(&r, algo, true, &f->parent);
	f->rank = next_str(&r);
	if (f->rank && !*f->rank) {
		free(f->rank);
		f->rank = NULL;
	}
	f->title = next_str(&r);
	f->created = next_int(&r, INT64_MIN, INT64_MAX);
	f->updated = next_int(&r, INT64_MIN, INT64_MAX);
	f->max_lamport = (uint64_t)next_int(&r, 0, INT64_MAX);
	long long nl = next_int(&r, 0, len);
	for (long long i = 0; i < nl && r.ok; i++) {
		char *l = next_str(&r);
		if (l)
			tix_strs_add(&f->labels, l);
		free(l);
	}
	long long na = next_int(&r, 0, len);
	for (long long i = 0; i < na && r.ok; i++) {
		tix_oid o;
		next_oid(&r, algo, false, &o);
		if (r.ok)
			TIX_VEC_PUSH(&f->assignees, o);
	}
	if (!r.ok || r.p != r.end || !f->title) {
		core_facets_free(f);
		return false;
	}
	return true;
}

void core_facets_from_entity(tix_entity *e, core_facets *f)
{
	const tix_ticket *t = &e->u.ticket;
	memset(&f->status, 0, sizeof(*f) - offsetof(core_facets, status));
	f->status = t->status;
	f->stage = t->stage ? tix_xstrdup(t->stage) : NULL;
	f->author = t->author;
	f->parent = t->plan.parent;
	f->rank = t->plan.rank ? tix_xstrdup(t->plan.rank) : NULL;
	f->title = tix_xstrdup(t->title);
	f->created = e->created;
	f->updated = e->updated;
	f->max_lamport = e->max_lamport;
	tix_strs_copy(&f->labels, &t->labels);
	for (size_t i = 0; i < t->plan.nassignments; i++)
		TIX_VEC_PUSH(&f->assignees, t->plan.assignments[i].resource);
	f->e = e;
}

void core_facets_free(core_facets *f)
{
	free(f->stage);
	free(f->rank);
	free(f->title);
	tix_strs_free(&f->labels);
	free(f->assignees.v);
	tix_entity_free(f->e);
	memset(&f->status, 0, sizeof(*f) - offsetof(core_facets, status));
}

/* The text forms of field values (see tix/input.h). Moved from the CLI in M6. */
#include "tix/input.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- value grammar ------------------------------------------------------------------ */

bool tix_input_date(const char *s, tix_date today, tix_date *out)
{
	if (!strcmp(s, "today")) {
		*out = today;
		return tix_date_valid(*out);
	}
	if (!strcmp(s, "tomorrow")) {
		*out = today + 1;
		return tix_date_valid(*out);
	}
	return tix_date_parse(s, out);
}

bool tix_input_count(const char *s, const char *unit, int64_t max, int64_t *out)
{
	char *end;
	if (*s < '0' || *s > '9')
		return false;
	long long v = strtoll(s, &end, 10);
	if (*end && strcmp(end, unit))
		return false;
	if (v < 0 || v > max)
		return false;
	*out = v;
	return true;
}

bool tix_input_is_none(const char *s)
{
	return !strcmp(s, "none") || !strcmp(s, "-");
}

static const char *const day_names[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};

static int day_index(const char *s, size_t n)
{
	for (int i = 0; i < 7; i++)
		if (n == 3 && !strncmp(s, day_names[i], 3))
			return i;
	return -1;
}

bool tix_input_weekdays(const char *s, int64_t *out)
{
	unsigned mask = 0;
	while (*s) {
		const char *end = strchr(s, ',');
		size_t n = end ? (size_t)(end - s) : strlen(s);
		const char *dash = memchr(s, '-', n);
		int a = day_index(s, dash ? (size_t)(dash - s) : n);
		int b = dash ? day_index(dash + 1, n - (size_t)(dash - s) - 1) : a;
		if (a < 0 || b < 0 || b < a)
			return false;
		for (int d = a; d <= b; d++)
			mask |= 1u << d;
		s += n + (end ? 1 : 0);
		if (end && !*s)
			return false;
	}
	if (!mask)
		return false;
	*out = mask;
	return true;
}

bool tix_input_range(const char *s, tix_date today, tix_date *from, tix_date *to, const char **note)
{
	char a[TIX_DATE_STR + 16], b[TIX_DATE_STR + 16];
	const char *eq = strchr(s, '=');
	size_t n = eq ? (size_t)(eq - s) : strlen(s);
	const char *dots = strstr(s, "..");
	if (dots && dots > s + n)
		dots = NULL;
	size_t an = dots ? (size_t)(dots - s) : n;
	if (an >= sizeof(a))
		return false;
	memcpy(a, s, an);
	a[an] = '\0';
	if (!tix_input_date(a, today, from))
		return false;
	*to = *from;
	if (dots) {
		size_t bn = n - an - 2;
		if (bn >= sizeof(b))
			return false;
		memcpy(b, dots + 2, bn);
		b[bn] = '\0';
		if (!tix_input_date(b, today, to))
			return false;
	}
	if (note)
		*note = eq ? eq + 1 : "";
	return true;
}

bool tix_input_amount(const char *s, int64_t *out)
{
	int64_t whole = 0, frac = 0;
	int digits = 0;
	const char *p = s;
	if (!isdigit((unsigned char)*p))
		return false;
	for (; isdigit((unsigned char)*p); p++)
		if ((whole = whole * 10 + (*p - '0')) > 10000000)
			return false;
	if (*p == '.') {
		for (p++; isdigit((unsigned char)*p) && digits < 2; p++, digits++)
			frac = frac * 10 + (*p - '0');
		if (!digits)
			return false;
	}
	if (*p)
		return false;
	while (digits++ < 2)
		frac *= 10;
	*out = whole * 100 + frac;
	return true;
}

/* ---- fields --------------------------------------------------------------------------- */

typedef enum kind {
	K_DATE,     /* date or none */
	K_ID,       /* id of `ref` or none */
	K_COUNT,    /* count with unit, or none */
	K_ENUM,     /* tix_field_enum name */
	K_PROGRESS, /* 0-100 with optional % */
	K_RANK,
	K_DEP,
	K_TEXT,     /* any string */
	K_CAPACITY,
	K_IDENTITY, /* identity id, me or none */
	K_RATE,
	K_ACTIVE,
	K_WEEKDAYS,
	K_HOURS,
	K_EXCEPTION, /* off / on */
	K_CLEAR,
} kind;

typedef struct field_def {
	tix_entity_type type;
	const char *name;
	kind k;
	tix_field f;
	tix_entity_type ref;  /* K_ID */
	const char *unit;     /* K_COUNT */
	int64_t max;          /* K_COUNT */
	const char *choices;  /* K_ENUM message */
} field_def;

#define T TIX_ENTITY_TICKET
#define R TIX_ENTITY_RESOURCE
#define C TIX_ENTITY_CALENDAR
static const field_def defs[] = {
	{T, "start", K_DATE, TIX_F_START, 0, NULL, 0, NULL},
	{T, "finish", K_DATE, TIX_F_FINISH, 0, NULL, 0, NULL},
	{T, "deadline", K_DATE, TIX_F_DEADLINE, 0, NULL, 0, NULL},
	{T, "actual-start", K_DATE, TIX_F_ACTUAL_START, 0, NULL, 0, NULL},
	{T, "actual-finish", K_DATE, TIX_F_ACTUAL_FINISH, 0, NULL, 0, NULL},
	{T, "parent", K_ID, TIX_F_PARENT, T, NULL, 0, NULL},
	{T, "calendar", K_ID, TIX_F_CALENDAR, C, NULL, 0, NULL},
	{T, "rank", K_RANK, 0, 0, NULL, 0, NULL},
	{T, "kind", K_ENUM, TIX_F_KIND, 0, NULL, 0, "task or milestone"},
	{T, "mode", K_ENUM, TIX_F_MODE, 0, NULL, 0, "auto or manual"},
	{T, "duration", K_COUNT, TIX_F_DURATION, 0, "d", TIX_DURATION_MAX, NULL},
	{T, "effort", K_COUNT, TIX_F_EFFORT, 0, "h", TIX_EFFORT_MAX, NULL},
	{T, "progress", K_PROGRESS, TIX_F_PROGRESS, 0, NULL, 0, NULL},
	{T, "dep", K_DEP, 0, 0, NULL, 0, NULL},
	{R, "name", K_TEXT, TIX_F_RES_NAME, 0, NULL, 0, NULL},
	{R, "kind", K_ENUM, TIX_F_RES_KIND, 0, NULL, 0, "person, role or equipment"},
	{R, "capacity", K_CAPACITY, TIX_F_CAPACITY, 0, NULL, 0, NULL},
	{R, "calendar", K_ID, TIX_F_RES_CALENDAR, C, NULL, 0, NULL},
	{R, "identity", K_IDENTITY, TIX_F_RES_IDENTITY, 0, NULL, 0, NULL},
	{R, "rate", K_RATE, 0, 0, NULL, 0, NULL},
	{R, "active", K_ACTIVE, TIX_F_ACTIVE, 0, NULL, 0, NULL},
	{C, "name", K_TEXT, TIX_F_CAL_NAME, 0, NULL, 0, NULL},
	{C, "weekdays", K_WEEKDAYS, TIX_F_WEEKDAYS, 0, NULL, 0, NULL},
	{C, "hours", K_HOURS, TIX_F_HOURS, 0, NULL, 0, NULL},
	{C, "off", K_EXCEPTION, 0, 0, NULL, 0, NULL},
	{C, "on", K_EXCEPTION, 0, 0, NULL, 0, NULL},
	{C, "clear", K_CLEAR, 0, 0, NULL, 0, NULL},
};
#undef T
#undef R
#undef C

static const field_def *find(tix_entity_type t, const char *name)
{
	for (size_t i = 0; i < sizeof(defs) / sizeof(*defs); i++)
		if (defs[i].type == t && !strcmp(defs[i].name, name))
			return &defs[i];
	return NULL;
}

bool tix_input_has(tix_entity_type t, const char *field)
{
	return find(t, field) != NULL;
}

static tix_status bad(tix_input *in, tix_error *err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static tix_status bad(tix_input *in, tix_error *err, const char *fmt, ...)
{
	char msg[sizeof(err->msg)];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	in->syntax = true;
	return tix_error_set(err, TIX_ERR_INVALID, "%s", msg);
}

static void push(tix_input *in, tix_op op)
{
	TIX_VEC_PUSH(&in->ops, op);
}

/* "+ID[:KIND[(+|-)Nd]]" or "-ID". */
static tix_status parse_dep(const tix_input_ctx *ctx, const char *arg, tix_input *in, tix_error *err)
{
	char idbuf[128];
	bool add = arg[0] == '+';
	if ((arg[0] != '+' && arg[0] != '-') || !arg[1])
		return bad(in, err, "dep: use +ID[:FS|SS|FF|SF[+Nd|-Nd]] to add or -ID to remove");
	const char *colon = strchr(arg + 1, ':');
	size_t idlen = colon ? (size_t)(colon - arg - 1) : strlen(arg + 1);
	if (idlen >= sizeof(idbuf))
		return bad(in, err, "dep: id too long");
	memcpy(idbuf, arg + 1, idlen);
	idbuf[idlen] = '\0';
	if (!add && colon)
		return bad(in, err, "dep: -ID takes no kind or lag");
	tix_dep_kind dk = TIX_DEP_FS;
	long lag = 0;
	if (colon) {
		char k[3] = {colon[1], colon[1] ? colon[2] : 0, 0};
		if (!tix_dep_kind_parse(k, &dk))
			return bad(in, err, "dep: kind must be FS, SS, FF or SF");
		const char *l = colon + 3;
		if (*l) {
			char *end;
			if (*l != '+' && *l != '-')
				return bad(in, err, "dep: lag must look like +2d or -1d");
			lag = strtol(l, &end, 10);
			if (strcmp(end, "d") || lag < -TIX_LAG_MAX || lag > TIX_LAG_MAX)
				return bad(in, err, "dep: lag must look like +2d or -1d (at most %d days)", TIX_LAG_MAX);
		}
	}
	tix_oid pred;
	tix_status st = tix_entity_resolve(ctx->repo, TIX_ENTITY_TICKET, idbuf, &pred, err);
	if (st)
		return st;
	push(in, add ? tix_op_dep_add(&pred, dk, (int32_t)lag) : tix_op_dep_remove(&pred));
	return TIX_OK;
}

static tix_status parse_rank(const tix_input_ctx *ctx, const char *arg, tix_input *in, tix_error *err)
{
	if (!strcmp(arg, "first") || !strcmp(arg, "last")) {
		in->rank_set = true;
		in->rank_pos = arg[0] == 'f' ? TIX_RANK_FIRST : TIX_RANK_LAST;
		return TIX_OK;
	}
	if (!strncmp(arg, "before:", 7) || !strncmp(arg, "after:", 6)) {
		tix_status st = tix_entity_resolve(ctx->repo, TIX_ENTITY_TICKET, strchr(arg, ':') + 1, &in->rank_anchor, err);
		if (st)
			return st;
		in->rank_set = true;
		in->rank_pos = arg[0] == 'b' ? TIX_RANK_BEFORE : TIX_RANK_AFTER;
		return TIX_OK;
	}
	return bad(in, err, "rank: use first, last, before:ID or after:ID");
}

static tix_status parse_rate(const char *v, tix_input *in, tix_error *err)
{
	if (tix_input_is_none(v)) {
		push(in, tix_op_set(TIX_F_RATE, tix_value_null()));
		push(in, tix_op_set(TIX_F_CURRENCY, tix_value_null()));
		return TIX_OK;
	}
	char cur[4] = {0}, num[32];
	const char *sp = strchr(v, ' ');
	size_t nl = sp ? (size_t)(sp - v) : strlen(v);
	if (nl >= sizeof(num) || !sp || strlen(sp + 1) != 3)
		return bad(in, err, "rate: an hourly amount and currency, like \"95.50 EUR\"");
	memcpy(num, v, nl);
	num[nl] = '\0';
	for (int k = 0; k < 3; k++) {
		if (!isalpha((unsigned char)sp[1 + k]))
			return bad(in, err, "rate: the currency is three letters, like EUR");
		cur[k] = (char)toupper((unsigned char)sp[1 + k]);
	}
	int64_t amount;
	if (!tix_input_amount(num, &amount))
		return bad(in, err, "rate: amount like 95 or 95.50");
	push(in, tix_op_set(TIX_F_RATE, tix_value_int(TIX_V_INT, amount)));
	push(in, tix_op_set(TIX_F_CURRENCY, tix_value_str(cur)));
	return TIX_OK;
}

tix_status tix_input_field(const tix_input_ctx *ctx, tix_entity_type t, const char *field, const char *v,
			   tix_input *in, tix_error *err)
{
	const field_def *d = find(t, field);
	TIX_REQUIRE(d && ctx && v && in);
	in->syntax = false;
	tix_oid id;
	tix_date from, to;
	const char *note;
	int64_t n;
	tix_status st;

	switch (d->k) {
	case K_DATE:
		if (tix_input_is_none(v)) {
			push(in, tix_op_set(d->f, tix_value_null()));
		} else if (tix_input_date(v, ctx->today, &from)) {
			push(in, tix_op_set(d->f, tix_value_int(TIX_V_DATE, from)));
		} else {
			return bad(in, err, "%s: '%s' is not a date (YYYY-MM-DD, today, tomorrow or none)", field, v);
		}
		return TIX_OK;
	case K_ID:
		if (tix_input_is_none(v)) {
			memset(&id, 0, sizeof(id));
			push(in, tix_op_set(d->f, tix_value_null()));
		} else {
			if ((st = tix_entity_resolve(ctx->repo, d->ref, v, &id, err)))
				return st;
			push(in, tix_op_set(d->f, tix_value_id(&id)));
		}
		if (d->f == TIX_F_PARENT) {
			in->parent_set = true;
			in->parent = id;
		}
		return TIX_OK;
	case K_COUNT:
		if (tix_input_is_none(v))
			push(in, tix_op_set(d->f, tix_value_null()));
		else if (tix_input_count(v, d->unit, d->max, &n))
			push(in, tix_op_set(d->f, tix_value_int(TIX_V_INT, n)));
		else
			return bad(in, err, "%s: '%s' should look like 3%s (or none)", field, v, d->unit);
		return TIX_OK;
	case K_ENUM: {
		int k = tix_field_enum(d->f, v);
		if (k < 0)
			return bad(in, err, "%s: use %s", field, d->choices);
		push(in, tix_op_set(d->f, tix_value_int(TIX_V_ENUM, k)));
		return TIX_OK;
	}
	case K_PROGRESS:
		if (!tix_input_count(v, "%", 100, &n))
			return bad(in, err, "progress: a percentage 0-100");
		push(in, tix_op_set(d->f, tix_value_int(TIX_V_INT, n)));
		return TIX_OK;
	case K_RANK:
		return parse_rank(ctx, v, in, err);
	case K_DEP:
		return parse_dep(ctx, v, in, err);
	case K_TEXT:
		if (!*v)
			return bad(in, err, "%s: must not be empty", field);
		push(in, tix_op_set(d->f, tix_value_str(v)));
		return TIX_OK;
	case K_CAPACITY:
		if (!tix_input_count(v, "%", TIX_UNITS_MAX, &n) || n < 1)
			return bad(in, err, "capacity: percent of one full-time unit, like 300%%");
		push(in, tix_op_set(d->f, tix_value_int(TIX_V_INT, n)));
		return TIX_OK;
	case K_IDENTITY:
		if (tix_input_is_none(v)) {
			push(in, tix_op_set(d->f, tix_value_null()));
			return TIX_OK;
		}
		if (!strcmp(v, "me")) {
			if (!tix_repo_identity(ctx->repo, &id))
				return bad(in, err, "identity: me: no identity configured");
		} else if ((st = tix_entity_resolve(ctx->repo, TIX_ENTITY_IDENTITY, v, &id, err))) {
			return st;
		}
		push(in, tix_op_set(d->f, tix_value_id(&id)));
		return TIX_OK;
	case K_RATE:
		return parse_rate(v, in, err);
	case K_ACTIVE:
		if (strcmp(v, "yes") && strcmp(v, "no"))
			return bad(in, err, "active: yes or no");
		push(in, tix_op_set(d->f, tix_value_int(TIX_V_BOOL, !strcmp(v, "yes"))));
		return TIX_OK;
	case K_WEEKDAYS:
		if (!tix_input_weekdays(v, &n))
			return bad(in, err, "weekdays: like mon-fri or mon,tue,thu");
		push(in, tix_op_set(d->f, tix_value_int(TIX_V_INT, n)));
		return TIX_OK;
	case K_HOURS:
		if (!tix_input_count(v, "h", 24, &n) || n < 1)
			return bad(in, err, "hours: working hours per day, 1-24");
		push(in, tix_op_set(d->f, tix_value_int(TIX_V_INT, n)));
		return TIX_OK;
	case K_EXCEPTION:
		if (!tix_input_range(v, ctx->today, &from, &to, &note))
			return bad(in, err, "%s: FROM[..TO][=NOTE], dates as YYYY-MM-DD", field);
		push(in, tix_op_exception_add(from, to, !strcmp(field, "on"), note));
		return TIX_OK;
	case K_CLEAR:
		if (!tix_input_range(v, ctx->today, &from, &to, NULL))
			return bad(in, err, "clear: FROM[..TO] of an existing exception");
		push(in, tix_op_exception_remove(from, to));
		return TIX_OK;
	}
	TIX_REQUIRE(!"unreachable");
	return TIX_ERR_INVALID;
}

tix_status tix_input_finish(const tix_input_ctx *ctx, tix_input *in, const tix_oid *self,
			    const tix_oid *current_parent, tix_error *err)
{
	if (!in->rank_set)
		return TIX_OK;
	const tix_oid *parent = in->parent_set ? (tix_oid_is_null(&in->parent) ? NULL : &in->parent)
					       : (current_parent && !tix_oid_is_null(current_parent) ? current_parent : NULL);
	tix_oid none;
	memset(&none, 0, sizeof(none));
	tix_buf rank = TIX_BUF_INIT;
	tix_status st = tix_ticket_rank(ctx->repo, parent, self ? self : &none, in->rank_pos, &in->rank_anchor, &rank, err);
	if (!st)
		push(in, tix_op_set(TIX_F_RANK, tix_value_str(rank.data)));
	tix_buf_free(&rank);
	in->syntax = false;
	return st;
}

void tix_input_free(tix_input *in)
{
	for (size_t i = 0; i < in->ops.n; i++)
		tix_op_free(&in->ops.v[i]);
	free(in->ops.v);
	memset(in, 0, sizeof(*in));
}

tix_status tix_input_assignment(const tix_input_ctx *ctx, const char *res, const char *effort_text, tix_op *out,
				bool *syntax, tix_error *err)
{
	tix_input tmp = {0}; /* only for bad()'s syntax flag */
	tix_status st;
	int64_t effort = -1, units = 100;
	char rbuf[128];
	*syntax = false;
	if (effort_text && !tix_input_count(effort_text, "h", TIX_EFFORT_MAX, &effort)) {
		st = bad(&tmp, err, "assign: effort must be hours, like 16h");
		goto out;
	}
	const char *colon = strchr(res, ':');
	size_t rlen = colon ? (size_t)(colon - res) : strlen(res);
	if (rlen >= sizeof(rbuf)) {
		st = bad(&tmp, err, "assign: resource id too long");
		goto out;
	}
	memcpy(rbuf, res, rlen);
	rbuf[rlen] = '\0';
	if (colon && !tix_input_count(colon + 1, "%", TIX_UNITS_MAX, &units)) {
		st = bad(&tmp, err, "assign: units must look like 50%%");
		goto out;
	}
	if (units < 1) {
		st = bad(&tmp, err, "assign: units must be at least 1%%");
		goto out;
	}
	tix_oid r;
	if ((st = tix_entity_resolve(ctx->repo, TIX_ENTITY_RESOURCE, rbuf, &r, err)))
		goto out;
	*out = tix_op_assign(&r, (int32_t)units, effort);
out:
	*syntax = tmp.syntax;
	return st;
}

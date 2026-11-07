/* Lossless tix_entity <-> JSON, for caches (see tix/model_replay.h). */
#include "tix/base_json.h"
#include "tix/model_replay.h"

#include <cjson/cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- encode --------------------------------------------------------------------- */

static void e_id(tix_jw *w, const char *k, const tix_oid *id)
{
	char hex[TIX_OID_MAX_HEX + 1];
	tix_jw_key(w, k);
	if (tix_oid_is_null(id))
		tix_jw_null(w);
	else
		tix_jw_str(w, tix_oid_hex(id, hex));
}

static void e_int(tix_jw *w, const char *k, int64_t v)
{
	tix_jw_key(w, k);
	tix_jw_int(w, v);
}

static void e_str(tix_jw *w, const char *k, const char *s)
{
	tix_jw_key(w, k);
	if (s)
		tix_jw_str(w, s);
	else
		tix_jw_null(w);
}

static void e_opid(tix_jw *w, const char *k, const tix_opid *id)
{
	char s[TIX_OID_MAX_HEX + 12];
	tix_jw_key(w, k);
	if (tix_oid_is_null(&id->commit))
		tix_jw_null(w);
	else
		tix_jw_str(w, tix_opid_str(id, s));
}

static void e_order(tix_jw *w, const char *k, const tix_order *o)
{
	tix_jw_key(w, k);
	tix_jw_obj_begin(w);
	e_int(w, "l", (int64_t)o->lamport);
	e_opid(w, "id", &o->id);
	tix_jw_obj_end(w);
}

static void encode_ticket(tix_jw *w, const tix_ticket *t)
{
	const tix_ticket_plan *p = &t->plan;
	e_id(w, "author", &t->author);
	e_str(w, "title", t->title);
	e_str(w, "body", t->body);
	e_int(w, "status", t->status);
	e_str(w, "stage", t->stage);
	tix_jw_key(w, "stages");
	tix_jw_arr_begin(w);
	for (size_t i = 0; i < t->nstages; i++) {
		tix_jw_obj_begin(w);
		e_str(w, "name", t->stages[i].name);
		e_int(w, "phase", t->stages[i].phase);
		tix_jw_key(w, "human");
		tix_jw_bool(w, t->stages[i].human);
		e_int(w, "rank", t->stages[i].rank);
		tix_jw_obj_end(w);
	}
	tix_jw_arr_end(w);
	tix_jw_key(w, "labels");
	tix_jw_arr_begin(w);
	for (size_t i = 0; i < t->labels.n; i++)
		tix_jw_str(w, t->labels.v[i]);
	tix_jw_arr_end(w);
	tix_jw_key(w, "comments");
	tix_jw_arr_begin(w);
	for (size_t i = 0; i < t->ncomments; i++) {
		const tix_comment *c = &t->comments[i];
		tix_jw_obj_begin(w);
		e_opid(w, "id", &c->id);
		e_id(w, "author", &c->author);
		e_int(w, "time", c->time);
		e_str(w, "body", c->body);
		tix_jw_key(w, "edited");
		tix_jw_bool(w, c->edited);
		tix_jw_obj_end(w);
	}
	tix_jw_arr_end(w);
	e_id(w, "parent", &p->parent);
	e_order(w, "parent_order", &p->parent_order);
	e_str(w, "rank", p->rank);
	e_str(w, "external", p->external);
	e_int(w, "kind", p->kind);
	e_id(w, "calendar", &p->calendar);
	e_int(w, "mode", p->mode);
	e_int(w, "start", p->start);
	e_int(w, "finish", p->finish);
	e_int(w, "deadline", p->deadline);
	e_int(w, "actual_start", p->actual_start);
	e_int(w, "actual_finish", p->actual_finish);
	e_int(w, "duration", p->duration);
	e_int(w, "effort", p->effort);
	e_int(w, "progress", p->progress);
	e_int(w, "started_at", p->started_at);
	e_int(w, "done_at", p->done_at);
	tix_jw_key(w, "deps");
	tix_jw_arr_begin(w);
	for (size_t i = 0; i < p->ndeps; i++) {
		tix_jw_obj_begin(w);
		e_id(w, "pred", &p->deps[i].pred);
		e_int(w, "kind", p->deps[i].kind);
		e_int(w, "lag", p->deps[i].lag);
		e_order(w, "order", &p->deps[i].order);
		tix_jw_obj_end(w);
	}
	tix_jw_arr_end(w);
	tix_jw_key(w, "assignments");
	tix_jw_arr_begin(w);
	for (size_t i = 0; i < p->nassignments; i++) {
		tix_jw_obj_begin(w);
		e_id(w, "resource", &p->assignments[i].resource);
		e_int(w, "units", p->assignments[i].units);
		e_int(w, "effort", p->assignments[i].effort);
		tix_jw_obj_end(w);
	}
	tix_jw_arr_end(w);
}

void tix_entity_encode(const tix_entity *e, tix_buf *out)
{
	tix_jw w;
	tix_jw_init(&w, out);
	tix_jw_obj_begin(&w);
	e_int(&w, "v", TIX_ENTITY_CODEC_VERSION);
	e_int(&w, "type", e->type);
	e_id(&w, "id", &e->id);
	e_id(&w, "head", &e->head);
	e_int(&w, "max_lamport", (int64_t)e->max_lamport);
	e_int(&w, "created", e->created);
	e_int(&w, "updated", e->updated);
	e_int(&w, "nops", (int64_t)e->nops);
	e_int(&w, "nunknown", (int64_t)e->nunknown);
	switch (e->type) {
	case TIX_ENTITY_TICKET: encode_ticket(&w, &e->u.ticket); break;
	case TIX_ENTITY_IDENTITY: {
		const tix_identity *id = &e->u.identity;
		e_str(&w, "name", id->name);
		e_str(&w, "email", id->email);
		e_int(&w, "kind", id->kind);
		tix_jw_key(&w, "keys");
		tix_jw_arr_begin(&w);
		for (size_t i = 0; i < id->nkeys; i++) {
			tix_jw_obj_begin(&w);
			e_str(&w, "kind", id->keys[i].kind);
			e_str(&w, "fingerprint", id->keys[i].fingerprint);
			e_str(&w, "pubkey", id->keys[i].pubkey);
			tix_jw_obj_end(&w);
		}
		tix_jw_arr_end(&w);
		break;
	}
	case TIX_ENTITY_RESOURCE: {
		const tix_resource *r = &e->u.resource;
		e_str(&w, "name", r->name);
		e_int(&w, "kind", r->kind);
		e_int(&w, "capacity", r->capacity);
		e_id(&w, "calendar", &r->calendar);
		e_id(&w, "identity", &r->identity);
		e_int(&w, "rate", r->rate);
		e_str(&w, "currency", r->currency);
		tix_jw_key(&w, "active");
		tix_jw_bool(&w, r->active);
		break;
	}
	case TIX_ENTITY_CALENDAR: {
		const tix_calendar *c = &e->u.calendar;
		e_str(&w, "name", c->name);
		e_int(&w, "weekdays", c->weekdays);
		e_int(&w, "hours", c->hours);
		tix_jw_key(&w, "exceptions");
		tix_jw_arr_begin(&w);
		for (size_t i = 0; i < c->nexceptions; i++) {
			const tix_cal_exception *x = &c->exceptions[i];
			tix_jw_obj_begin(&w);
			e_int(&w, "from", x->from);
			e_int(&w, "to", x->to);
			tix_jw_key(&w, "working");
			tix_jw_bool(&w, x->working);
			e_str(&w, "note", x->note);
			e_order(&w, "order", &x->order);
			tix_jw_obj_end(&w);
		}
		tix_jw_arr_end(&w);
		break;
	}
	case TIX_ENTITY_BASELINE: {
		const tix_baseline *b = &e->u.baseline;
		e_str(&w, "name", b->name);
		e_int(&w, "taken", b->taken);
		e_id(&w, "root", &b->root);
		tix_jw_key(&w, "tasks");
		tix_jw_arr_begin(&w);
		for (size_t i = 0; i < b->ntasks; i++) {
			const tix_baseline_task *t = &b->tasks[i];
			tix_jw_obj_begin(&w);
			e_id(&w, "ticket", &t->ticket);
			e_int(&w, "start", t->start);
			e_int(&w, "finish", t->finish);
			e_int(&w, "duration", t->duration);
			e_int(&w, "effort", t->effort);
			e_int(&w, "cost", t->cost);
			e_str(&w, "currency", t->currency);
			tix_jw_obj_end(&w);
		}
		tix_jw_arr_end(&w);
		break;
	}
	}
	tix_jw_obj_end(&w);
}

/* ---- decode: the bytes come from a local cache, but are still checked --------------- */

typedef struct dec {
	tix_hash_algo algo;
	bool bad;
} dec;

static const cJSON *item(dec *d, const cJSON *o, const char *k)
{
	const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
	if (!it)
		d->bad = true;
	return it;
}

static int64_t d_int(dec *d, const cJSON *o, const char *k, int64_t min, int64_t max)
{
	const cJSON *it = item(d, o, k);
	if (!cJSON_IsNumber(it) || floor(it->valuedouble) != it->valuedouble || it->valuedouble < (double)min ||
	    it->valuedouble > (double)max) {
		d->bad = true;
		return min;
	}
	return (int64_t)it->valuedouble;
}

static bool d_bool(dec *d, const cJSON *o, const char *k)
{
	const cJSON *it = item(d, o, k);
	if (!cJSON_IsBool(it))
		d->bad = true;
	return cJSON_IsTrue(it);
}

/* NULL for JSON null (allowed when nullable). OWNS the result. */
static char *d_str(dec *d, const cJSON *o, const char *k, bool nullable)
{
	const cJSON *it = item(d, o, k);
	if (nullable && cJSON_IsNull(it))
		return NULL;
	if (!cJSON_IsString(it) || !tix_utf8_valid(it->valuestring, strlen(it->valuestring))) {
		d->bad = true;
		return nullable ? NULL : tix_xstrdup("");
	}
	return tix_xstrdup(it->valuestring);
}

static tix_oid d_id(dec *d, const cJSON *o, const char *k)
{
	tix_oid id;
	memset(&id, 0, sizeof(id));
	const cJSON *it = item(d, o, k);
	if (cJSON_IsNull(it))
		return id;
	if (!cJSON_IsString(it) || !tix_oid_from_hex(&id, it->valuestring, strlen(it->valuestring)) ||
	    id.algo != d->algo) {
		d->bad = true;
		memset(&id, 0, sizeof(id));
	}
	return id;
}

static tix_opid d_opid(dec *d, const cJSON *o, const char *k)
{
	tix_opid id;
	memset(&id, 0, sizeof(id));
	const cJSON *it = item(d, o, k);
	if (cJSON_IsNull(it))
		return id;
	if (!cJSON_IsString(it) || !tix_opid_parse(&id, it->valuestring) || id.commit.algo != d->algo) {
		d->bad = true;
		memset(&id, 0, sizeof(id));
	}
	return id;
}

static tix_order d_order(dec *d, const cJSON *o, const char *k)
{
	tix_order ord;
	memset(&ord, 0, sizeof(ord));
	const cJSON *it = item(d, o, k);
	if (!cJSON_IsObject(it)) {
		d->bad = true;
		return ord;
	}
	ord.lamport = (uint64_t)d_int(d, it, "l", 0, (int64_t)TIX_LAMPORT_MAX);
	ord.id = d_opid(d, it, "id");
	return ord;
}

static tix_date d_date(dec *d, const cJSON *o, const char *k)
{
	int64_t v = d_int(d, o, k, INT32_MIN, TIX_DATE_MAX);
	if (v != TIX_DATE_NONE && !tix_date_valid((tix_date)v))
		d->bad = true;
	return (tix_date)v;
}

static const cJSON *d_arr(dec *d, const cJSON *o, const char *k, size_t max, size_t *n)
{
	const cJSON *a = item(d, o, k);
	*n = 0;
	if (!cJSON_IsArray(a)) {
		d->bad = true;
		return NULL;
	}
	int sz = cJSON_GetArraySize(a);
	if (sz < 0 || (size_t)sz > max) {
		d->bad = true;
		return NULL;
	}
	*n = (size_t)sz;
	return a;
}

#define MAX_ITEMS ((size_t)TIX_PACK_MAX_OPS * TIX_ENTITY_MAX_COMMITS)

static void decode_ticket(dec *d, const cJSON *o, tix_ticket *t)
{
	tix_ticket_plan *p = &t->plan;
	size_t n;
	const cJSON *a, *it;
	t->author = d_id(d, o, "author");
	t->title = d_str(d, o, "title", false);
	t->body = d_str(d, o, "body", false);
	t->status = (tix_ticket_status)d_int(d, o, "status", 0, TIX_TICKET_CANCELLED);
	t->stage = d_str(d, o, "stage", true);
	if (t->stage && !tix_stage_valid(t->stage))
		d->bad = true;
	if ((a = d_arr(d, o, "stages", MAX_ITEMS, &n)) && n) {
		t->stages = tix_xcalloc(n, sizeof(tix_stage_def));
		cJSON_ArrayForEach(it, a)
		{
			tix_stage_def *x = &t->stages[t->nstages++];
			x->name = d_str(d, it, "name", false);
			if (!x->name || !tix_stage_valid(x->name))
				d->bad = true;
			x->phase = (tix_ticket_status)d_int(d, it, "phase", 0, TIX_TICKET_CANCELLED);
			x->human = d_bool(d, it, "human");
			x->rank = (int32_t)d_int(d, it, "rank", 0, TIX_STAGE_RANK_MAX);
		}
	}
	if ((a = d_arr(d, o, "labels", MAX_ITEMS, &n)))
		cJSON_ArrayForEach(it, a)
		{
			if (cJSON_IsString(it) && tix_utf8_valid(it->valuestring, strlen(it->valuestring)))
				tix_strs_add(&t->labels, it->valuestring);
			else
				d->bad = true;
		}
	if ((a = d_arr(d, o, "comments", MAX_ITEMS, &n)) && n) {
		t->comments = tix_xcalloc(n, sizeof(tix_comment));
		cJSON_ArrayForEach(it, a)
		{
			tix_comment *c = &t->comments[t->ncomments++];
			c->id = d_opid(d, it, "id");
			c->author = d_id(d, it, "author");
			c->time = d_int(d, it, "time", 0, (int64_t)TIX_LAMPORT_MAX);
			c->body = d_str(d, it, "body", false);
			c->edited = d_bool(d, it, "edited");
		}
	}
	p->parent = d_id(d, o, "parent");
	p->parent_order = d_order(d, o, "parent_order");
	p->rank = d_str(d, o, "rank", true);
	p->external = d_str(d, o, "external", true);
	p->kind = (tix_ticket_kind)d_int(d, o, "kind", 0, TIX_KIND_MILESTONE);
	p->calendar = d_id(d, o, "calendar");
	p->mode = (tix_sched_mode)d_int(d, o, "mode", 0, TIX_MODE_MANUAL);
	p->start = d_date(d, o, "start");
	p->finish = d_date(d, o, "finish");
	p->deadline = d_date(d, o, "deadline");
	p->actual_start = d_date(d, o, "actual_start");
	p->actual_finish = d_date(d, o, "actual_finish");
	p->duration = d_int(d, o, "duration", -1, TIX_DURATION_MAX);
	p->effort = d_int(d, o, "effort", -1, TIX_EFFORT_MAX);
	p->progress = (int)d_int(d, o, "progress", 0, 100);
	p->started_at = d_int(d, o, "started_at", -1, (int64_t)TIX_LAMPORT_MAX);
	p->done_at = d_int(d, o, "done_at", -1, (int64_t)TIX_LAMPORT_MAX);
	if ((a = d_arr(d, o, "deps", MAX_ITEMS, &n)) && n) {
		p->deps = tix_xcalloc(n, sizeof(tix_dep));
		cJSON_ArrayForEach(it, a)
		{
			tix_dep *x = &p->deps[p->ndeps++];
			x->pred = d_id(d, it, "pred");
			x->kind = (tix_dep_kind)d_int(d, it, "kind", 0, TIX_DEP_SF);
			x->lag = (int32_t)d_int(d, it, "lag", -TIX_LAG_MAX, TIX_LAG_MAX);
			x->order = d_order(d, it, "order");
		}
	}
	if ((a = d_arr(d, o, "assignments", MAX_ITEMS, &n)) && n) {
		p->assignments = tix_xcalloc(n, sizeof(tix_assignment));
		cJSON_ArrayForEach(it, a)
		{
			tix_assignment *x = &p->assignments[p->nassignments++];
			x->resource = d_id(d, it, "resource");
			x->units = (int32_t)d_int(d, it, "units", 1, TIX_UNITS_MAX);
			x->effort = d_int(d, it, "effort", -1, TIX_EFFORT_MAX);
		}
	}
}

tix_status tix_entity_decode(const char *data, size_t len, tix_hash_algo algo, tix_entity **out, tix_error *err)
{
	TIX_REQUIRE(out && (data || !len));
	*out = NULL;
	if (!tix_utf8_valid(data ? data : "", len))
		return tix_error_set(err, TIX_ERR_INVALID, "cached entity is not UTF-8");
	cJSON *o = cJSON_ParseWithLength(data, len);
	if (!cJSON_IsObject(o)) {
		cJSON_Delete(o);
		return tix_error_set(err, TIX_ERR_INVALID, "cached entity is not a JSON object");
	}
	dec d = {algo, false};
	if (d_int(&d, o, "v", 0, 1000) != TIX_ENTITY_CODEC_VERSION || d.bad) {
		cJSON_Delete(o);
		return tix_error_set(err, TIX_ERR_UNSUPPORTED, "cached entity has another codec version");
	}
	tix_entity *e = tix_xcalloc(1, sizeof(*e));
	e->type = (tix_entity_type)d_int(&d, o, "type", TIX_ENTITY_IDENTITY, TIX_ENTITY_BASELINE);
	e->id = d_id(&d, o, "id");
	e->head = d_id(&d, o, "head");
	e->max_lamport = (uint64_t)d_int(&d, o, "max_lamport", 0, (int64_t)TIX_LAMPORT_MAX);
	e->created = d_int(&d, o, "created", 0, (int64_t)TIX_LAMPORT_MAX);
	e->updated = d_int(&d, o, "updated", 0, (int64_t)TIX_LAMPORT_MAX);
	e->nops = (size_t)d_int(&d, o, "nops", 0, (int64_t)MAX_ITEMS);
	e->nunknown = (size_t)d_int(&d, o, "nunknown", 0, (int64_t)MAX_ITEMS);
	if (tix_oid_is_null(&e->id) || tix_oid_is_null(&e->head))
		d.bad = true;
	size_t n;
	const cJSON *a, *it;
	if (d.bad) {
		/* type unknown: leave the union zeroed (as IDENTITY, freeing is safe) */
		e->type = TIX_ENTITY_IDENTITY;
	} else if (e->type == TIX_ENTITY_TICKET) {
		decode_ticket(&d, o, &e->u.ticket);
	} else if (e->type == TIX_ENTITY_IDENTITY) {
		tix_identity *id = &e->u.identity;
		id->name = d_str(&d, o, "name", false);
		id->email = d_str(&d, o, "email", false);
		id->kind = (tix_identity_kind)d_int(&d, o, "kind", 0, TIX_ID_AGENT);
		if ((a = d_arr(&d, o, "keys", MAX_ITEMS, &n)) && n) {
			id->keys = tix_xcalloc(n, sizeof(tix_key));
			cJSON_ArrayForEach(it, a)
			{
				tix_key *k = &id->keys[id->nkeys++];
				k->kind = d_str(&d, it, "kind", false);
				k->fingerprint = d_str(&d, it, "fingerprint", false);
				k->pubkey = d_str(&d, it, "pubkey", false);
			}
		}
	} else if (e->type == TIX_ENTITY_RESOURCE) {
		tix_resource *r = &e->u.resource;
		r->name = d_str(&d, o, "name", false);
		r->kind = (tix_resource_kind)d_int(&d, o, "kind", 0, TIX_RES_EQUIPMENT);
		r->capacity = (int32_t)d_int(&d, o, "capacity", 1, TIX_UNITS_MAX);
		r->calendar = d_id(&d, o, "calendar");
		r->identity = d_id(&d, o, "identity");
		r->rate = d_int(&d, o, "rate", -1, 1000000000);
		r->currency = d_str(&d, o, "currency", true);
		r->active = d_bool(&d, o, "active");
	} else if (e->type == TIX_ENTITY_BASELINE) {
		tix_baseline *b = &e->u.baseline;
		b->name = d_str(&d, o, "name", false);
		b->taken = d_date(&d, o, "taken");
		b->root = d_id(&d, o, "root");
		if (b->taken == TIX_DATE_NONE)
			d.bad = true;
		if ((a = d_arr(&d, o, "tasks", MAX_ITEMS, &n)) && n) {
			b->tasks = tix_xcalloc(n, sizeof(tix_baseline_task));
			cJSON_ArrayForEach(it, a)
			{
				tix_baseline_task *t = &b->tasks[b->ntasks++];
				t->ticket = d_id(&d, it, "ticket");
				t->start = d_date(&d, it, "start");
				t->finish = d_date(&d, it, "finish");
				t->duration = d_int(&d, it, "duration", -1, TIX_DURATION_MAX);
				t->effort = d_int(&d, it, "effort", -1, TIX_EFFORT_MAX);
				t->cost = d_int(&d, it, "cost", -1, TIX_COST_MAX);
				t->currency = d_str(&d, it, "currency", true);
				if (tix_oid_is_null(&t->ticket) || (b->ntasks > 1 && tix_oid_cmp(&b->tasks[b->ntasks - 2].ticket,
												 &t->ticket) >= 0))
					d.bad = true; /* sorted and unique, as replay leaves them */
			}
		}
	} else {
		tix_calendar *c = &e->u.calendar;
		c->name = d_str(&d, o, "name", false);
		c->weekdays = (unsigned)d_int(&d, o, "weekdays", 0, 127);
		c->hours = (int)d_int(&d, o, "hours", 1, 24);
		if ((a = d_arr(&d, o, "exceptions", MAX_ITEMS, &n)) && n) {
			c->exceptions = tix_xcalloc(n, sizeof(tix_cal_exception));
			cJSON_ArrayForEach(it, a)
			{
				tix_cal_exception *x = &c->exceptions[c->nexceptions++];
				x->from = d_date(&d, it, "from");
				x->to = d_date(&d, it, "to");
				x->working = d_bool(&d, it, "working");
				x->note = d_str(&d, it, "note", false);
				x->order = d_order(&d, it, "order");
				if (x->from == TIX_DATE_NONE || x->to < x->from)
					d.bad = true;
			}
		}
	}
	cJSON_Delete(o);
	if (d.bad) {
		tix_entity_free(e);
		return tix_error_set(err, TIX_ERR_INVALID, "cached entity is malformed");
	}
	*out = e;
	return TIX_OK;
}

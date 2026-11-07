#include "tix/index_mem.h"

#include <stdlib.h>
#include <string.h>

typedef struct entry {
	tix_entity_type t;
	tix_oid id, head;
	tix_buf data, summary;
} entry;

typedef struct mem_index {
	tix_index base; /* must be first */
	TIX_VEC(entry) v;
	size_t hits, misses, scans;
} mem_index;

static const tix_index_vt mem_vt;

static mem_index *as_mem(tix_index *x)
{
	TIX_REQUIRE(x && x->vt == &mem_vt);
	return (mem_index *)x;
}

static entry *find(mem_index *m, tix_entity_type t, const tix_oid *id)
{
	for (size_t i = 0; i < m->v.n; i++)
		if (m->v.v[i].t == t && tix_oid_eq(&m->v.v[i].id, id))
			return &m->v.v[i];
	return NULL;
}

static tix_status m_get(tix_index *x, tix_entity_type t, const tix_oid *id, const tix_oid *head, tix_buf *out,
			tix_error *err)
{
	mem_index *m = as_mem(x);
	entry *e = find(m, t, id);
	if (!e || !tix_oid_eq(&e->head, head)) {
		m->misses++;
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "not cached");
	}
	m->hits++;
	tix_buf_reset(out);
	tix_buf_add(out, e->data.data ? e->data.data : "", e->data.len);
	return TIX_OK;
}

static tix_status m_put(tix_index *x, tix_entity_type t, const tix_oid *id, const tix_oid *head, const char *data,
			size_t len, const char *summary, size_t summary_len, tix_error *err)
{
	mem_index *m = as_mem(x);
	entry *e = find(m, t, id);
	if (!e) {
		entry n = {t, *id, *head, TIX_BUF_INIT, TIX_BUF_INIT};
		TIX_VEC_PUSH(&m->v, n);
		e = &m->v.v[m->v.n - 1];
	}
	e->head = *head;
	tix_buf_reset(&e->data);
	tix_buf_add(&e->data, data, len);
	tix_buf_reset(&e->summary);
	tix_buf_add(&e->summary, summary, summary_len);
	return TIX_OK;
}

static tix_status m_scan(tix_index *x, tix_entity_type t, tix_index_rows *out, tix_error *err)
{
	mem_index *m = as_mem(x);
	TIX_VEC(tix_index_row) rows = {0};
	for (size_t i = 0; i < m->v.n; i++) {
		if (m->v.v[i].t != t)
			continue;
		tix_index_row r = {m->v.v[i].id, m->v.v[i].head, TIX_BUF_INIT};
		tix_buf_add(&r.summary, m->v.v[i].summary.data, m->v.v[i].summary.len);
		TIX_VEC_PUSH(&rows, r);
	}
	m->scans++;
	out->v = rows.v;
	out->n = rows.n;
	return TIX_OK;
}

static tix_status m_clear(tix_index *x, tix_error *err)
{
	mem_index *m = as_mem(x);
	for (size_t i = 0; i < m->v.n; i++) {
		tix_buf_free(&m->v.v[i].data);
		tix_buf_free(&m->v.v[i].summary);
	}
	m->v.n = 0;
	return TIX_OK;
}

static tix_status m_count(tix_index *x, size_t *out, tix_error *err)
{
	*out = as_mem(x)->v.n;
	return TIX_OK;
}

static void m_destroy(tix_index *x)
{
	mem_index *m = as_mem(x);
	m_clear(x, NULL);
	free(m->v.v);
	free(m);
}

static const tix_index_vt mem_vt = {m_get, m_put, m_scan, m_clear, m_count, m_destroy};

tix_index *tix_index_mem_new(void)
{
	mem_index *m = tix_xcalloc(1, sizeof(*m));
	m->base.vt = &mem_vt;
	return &m->base;
}

size_t tix_index_mem_hits(tix_index *x)
{
	return as_mem(x)->hits;
}

size_t tix_index_mem_misses(tix_index *x)
{
	return as_mem(x)->misses;
}

size_t tix_index_mem_scans(tix_index *x)
{
	return as_mem(x)->scans;
}

void tix_index_mem_corrupt_summary(tix_index *x, tix_entity_type t, const tix_oid *id, const char *summary)
{
	entry *e = find(as_mem(x), t, id);
	TIX_REQUIRE(e);
	tix_buf_reset(&e->summary);
	tix_buf_adds(&e->summary, summary);
}

void tix_index_mem_corrupt(tix_index *x, tix_entity_type t, const tix_oid *id, const char *data)
{
	entry *e = find(as_mem(x), t, id);
	TIX_REQUIRE(e);
	tix_buf_reset(&e->data);
	tix_buf_adds(&e->data, data);
}

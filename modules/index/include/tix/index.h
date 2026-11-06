/*
 * tix/index.h -- the cache port (architecture §2, M5).
 *
 * A key-value cache of replayed entities: (type, entity id) -> (head, bytes),
 * where the bytes are tix_entity_encode output. A lookup only hits when the
 * stored head equals the ref's current head, so a moved ref simply misses and
 * gets re-validated by full replay. The cache is disposable: an adapter may lose
 * or refuse anything, and core treats every failure as a miss.
 *
 * Adapters: index_sqlite ($GIT_DIR/tix/index.db), index_mem (tests).
 * Interface convention as for tix_store.
 */
#ifndef TIX_INDEX_H
#define TIX_INDEX_H

#include "tix/base.h"
#include "tix/base_buf.h"
#include "tix/base_oid.h"
#include "tix/model.h"

typedef struct tix_index tix_index;

typedef struct tix_index_vt {
	tix_status (*get)(tix_index *x, tix_entity_type t, const tix_oid *id, const tix_oid *head, tix_buf *out,
			  tix_error *err);
	tix_status (*put)(tix_index *x, tix_entity_type t, const tix_oid *id, const tix_oid *head, const char *data,
			  size_t len, tix_error *err);
	tix_status (*clear)(tix_index *x, tix_error *err);
	tix_status (*count)(tix_index *x, size_t *out, tix_error *err);
	void (*destroy)(tix_index *x);
} tix_index_vt;

struct tix_index {
	const tix_index_vt *vt;
};

/* RETURNS "1": the port's version (bumped if the vtable changes). */
const char *tix_index_port_version(void);

/* Largest cached entry an adapter must accept (bigger ones may be refused). */
#define TIX_INDEX_MAX_ENTRY (64u << 20)

/* RETURNS TIX_ERR_NOT_FOUND unless an entry for (t, id) exists with this head;
 * on TIX_OK out holds exactly the stored bytes. */
static inline tix_status tix_index_get(tix_index *x, tix_entity_type t, const tix_oid *id, const tix_oid *head,
				       tix_buf *out, tix_error *err)
{
	TIX_REQUIRE(x && id && head && out);
	return x->vt->get(x, t, id, head, out, err);
}

/* Stores (replacing any entry for (t, id)). */
static inline tix_status tix_index_put(tix_index *x, tix_entity_type t, const tix_oid *id, const tix_oid *head,
				       const char *data, size_t len, tix_error *err)
{
	TIX_REQUIRE(x && id && head && (data || !len));
	return x->vt->put(x, t, id, head, data, len, err);
}

static inline tix_status tix_index_clear(tix_index *x, tix_error *err)
{
	TIX_REQUIRE(x);
	return x->vt->clear(x, err);
}

static inline tix_status tix_index_count(tix_index *x, size_t *out, tix_error *err)
{
	TIX_REQUIRE(x && out);
	return x->vt->count(x, out, err);
}

static inline void tix_index_destroy(tix_index *x)
{
	if (x)
		x->vt->destroy(x);
}

#endif

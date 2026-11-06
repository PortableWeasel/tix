/* tix_index on SQLite. The only module that includes sqlite3.h. */
#include "tix/index_sqlite.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SCHEMA_VERSION 1

typedef struct sql_index {
	tix_index base; /* must be first */
	sqlite3 *db;
	sqlite3_stmt *get, *put, *clear, *count;
} sql_index;

static const tix_index_vt sql_vt;

static sql_index *as_sql(tix_index *x)
{
	TIX_REQUIRE(x && x->vt == &sql_vt);
	return (sql_index *)x;
}

static tix_status sql_err(tix_error *err, sqlite3 *db, const char *what)
{
	return tix_error_set(err, TIX_ERR_IO, "index: %s: %s", what, db ? sqlite3_errmsg(db) : "out of memory");
}

/* Key: type byte + raw oid bytes (+ algo), so SHA-1 and SHA-256 ids never collide. */
static void key_of(const tix_oid *id, uint8_t *out, int *len)
{
	size_t n = tix_oid_rawlen((tix_hash_algo)id->algo);
	out[0] = id->algo;
	memcpy(out + 1, id->bytes, n);
	*len = (int)n + 1;
}

static tix_status s_get(tix_index *x, tix_entity_type t, const tix_oid *id, const tix_oid *head, tix_buf *out,
			tix_error *err)
{
	sql_index *s = as_sql(x);
	uint8_t k[40], h[40];
	int kl, hl;
	key_of(id, k, &kl);
	key_of(head, h, &hl);
	sqlite3_reset(s->get);
	sqlite3_bind_int(s->get, 1, (int)t);
	sqlite3_bind_blob(s->get, 2, k, kl, SQLITE_STATIC);
	sqlite3_bind_blob(s->get, 3, h, hl, SQLITE_STATIC);
	int rc = sqlite3_step(s->get);
	tix_status st;
	if (rc == SQLITE_ROW) {
		const void *data = sqlite3_column_blob(s->get, 0);
		int n = sqlite3_column_bytes(s->get, 0);
		tix_buf_reset(out);
		if (n > 0)
			tix_buf_add(out, data, (size_t)n);
		st = TIX_OK;
	} else if (rc == SQLITE_DONE) {
		st = tix_error_set(err, TIX_ERR_NOT_FOUND, "not cached");
	} else {
		st = sql_err(err, s->db, "lookup");
	}
	sqlite3_reset(s->get);
	return st;
}

static tix_status s_put(tix_index *x, tix_entity_type t, const tix_oid *id, const tix_oid *head, const char *data,
			size_t len, tix_error *err)
{
	sql_index *s = as_sql(x);
	uint8_t k[40], h[40];
	int kl, hl;
	if (len > TIX_INDEX_MAX_ENTRY)
		return tix_error_set(err, TIX_ERR_INVALID, "index: entry too large");
	key_of(id, k, &kl);
	key_of(head, h, &hl);
	sqlite3_reset(s->put);
	sqlite3_bind_int(s->put, 1, (int)t);
	sqlite3_bind_blob(s->put, 2, k, kl, SQLITE_STATIC);
	sqlite3_bind_blob(s->put, 3, h, hl, SQLITE_STATIC);
	sqlite3_bind_blob(s->put, 4, len ? data : "", (int)len, SQLITE_STATIC);
	int rc = sqlite3_step(s->put);
	sqlite3_reset(s->put);
	return rc == SQLITE_DONE ? TIX_OK : sql_err(err, s->db, "store");
}

static tix_status s_clear(tix_index *x, tix_error *err)
{
	sql_index *s = as_sql(x);
	sqlite3_reset(s->clear);
	int rc = sqlite3_step(s->clear);
	sqlite3_reset(s->clear);
	return rc == SQLITE_DONE ? TIX_OK : sql_err(err, s->db, "clear");
}

static tix_status s_count(tix_index *x, size_t *out, tix_error *err)
{
	sql_index *s = as_sql(x);
	sqlite3_reset(s->count);
	int rc = sqlite3_step(s->count);
	tix_status st = TIX_OK;
	if (rc == SQLITE_ROW)
		*out = (size_t)sqlite3_column_int64(s->count, 0);
	else
		st = sql_err(err, s->db, "count");
	sqlite3_reset(s->count);
	return st;
}

static void close_all(sql_index *s)
{
	sqlite3_finalize(s->get);
	sqlite3_finalize(s->put);
	sqlite3_finalize(s->clear);
	sqlite3_finalize(s->count);
	sqlite3_close(s->db);
}

static void s_destroy(tix_index *x)
{
	sql_index *s = as_sql(x);
	close_all(s);
	free(s);
}

static const tix_index_vt sql_vt = {s_get, s_put, s_clear, s_count, s_destroy};

static const char schema[] =
	"CREATE TABLE IF NOT EXISTS entities ("
	" type INTEGER NOT NULL, id BLOB NOT NULL, head BLOB NOT NULL, data BLOB NOT NULL,"
	" PRIMARY KEY (type, id)) WITHOUT ROWID;";

/* RETURNS SQLITE_OK, or the code that made this database unusable. */
static int try_open(const char *path, sql_index *s)
{
	int rc = sqlite3_open_v2(path, &s->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW, NULL);
	if (rc != SQLITE_OK)
		return rc;
	sqlite3_busy_timeout(s->db, 2000);
	sqlite3_stmt *v = NULL;
	rc = sqlite3_prepare_v2(s->db, "PRAGMA user_version", -1, &v, NULL);
	int version = -1;
	if (rc == SQLITE_OK && (rc = sqlite3_step(v)) == SQLITE_ROW) {
		version = sqlite3_column_int(v, 0);
		rc = SQLITE_OK;
	}
	sqlite3_finalize(v);
	if (rc != SQLITE_OK)
		return rc;
	if (version != 0 && version != SCHEMA_VERSION)
		return SQLITE_SCHEMA; /* another layout: start over */
	char pragmas[128];
	snprintf(pragmas, sizeof(pragmas),
		 "PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF; PRAGMA user_version=%d;", SCHEMA_VERSION);
	/* a cache: losing the last writes in a crash is fine, fsync per entry is not */
	if ((rc = sqlite3_exec(s->db, pragmas, NULL, NULL, NULL)) != SQLITE_OK ||
	    (rc = sqlite3_exec(s->db, schema, NULL, NULL, NULL)) != SQLITE_OK)
		return rc;
	if ((rc = sqlite3_prepare_v2(s->db, "SELECT data FROM entities WHERE type=?1 AND id=?2 AND head=?3", -1,
				     &s->get, NULL)) ||
	    (rc = sqlite3_prepare_v2(s->db, "INSERT OR REPLACE INTO entities(type, id, head, data) VALUES(?1,?2,?3,?4)",
				     -1, &s->put, NULL)) ||
	    (rc = sqlite3_prepare_v2(s->db, "DELETE FROM entities", -1, &s->clear, NULL)) ||
	    (rc = sqlite3_prepare_v2(s->db, "SELECT count(*) FROM entities", -1, &s->count, NULL)))
		return rc;
	return SQLITE_OK;
}

static void remove_db(const char *path)
{
	char side[4096];
	unlink(path);
	snprintf(side, sizeof(side), "%s-wal", path);
	unlink(side);
	snprintf(side, sizeof(side), "%s-shm", path);
	unlink(side);
}

tix_status tix_index_sqlite_open(const char *path, tix_index **out, tix_error *err)
{
	TIX_REQUIRE(path && out);
	*out = NULL;
	bool memory = !strcmp(path, ":memory:");
	struct stat st;
	if (!memory && !lstat(path, &st) && !S_ISREG(st.st_mode))
		return tix_error_set(err, TIX_ERR_IO, "index: %s is not a regular file", path);
	for (int attempt = 0; attempt < 2; attempt++) {
		sql_index *s = tix_xcalloc(1, sizeof(*s));
		s->base.vt = &sql_vt;
		int rc = try_open(path, s);
		if (rc == SQLITE_OK) {
			*out = &s->base;
			return TIX_OK;
		}
		tix_error_set(err, TIX_ERR_IO, "index: cannot open %s: %s", path,
			      s->db ? sqlite3_errmsg(s->db) : sqlite3_errstr(rc));
		close_all(s);
		free(s);
		bool unusable = rc == SQLITE_NOTADB || rc == SQLITE_CORRUPT || rc == SQLITE_SCHEMA;
		if (memory || !unusable)
			break;
		remove_db(path); /* disposable: rebuild from the refs */
	}
	return TIX_ERR_IO;
}

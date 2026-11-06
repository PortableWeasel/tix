/*
 * tix/index_sqlite.h -- tix_index on SQLite (M5). The database is a disposable
 * cache, conventionally $GIT_DIR/tix/index.db: it is never synced, may be
 * deleted at any time, and is rebuilt on demand.
 */
#ifndef TIX_INDEX_SQLITE_H
#define TIX_INDEX_SQLITE_H

#include "tix/index.h"

/*
 * Opens (creating if needed) the cache at path; ":memory:" gives a private
 * in-memory database. A file that isn't a usable tix cache (corrupt, another
 * schema version, not SQLite at all) is deleted and recreated.
 * RETURNS TIX_ERR_IO if no database can be opened at all.
 * ENSURES on TIX_OK: *out owned by the caller (tix_index_destroy).
 */
tix_status tix_index_sqlite_open(const char *path, tix_index **out, tix_error *err);

#endif

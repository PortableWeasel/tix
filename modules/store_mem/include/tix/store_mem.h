/*
 * tix/store_mem.h -- in-memory tix_store for tests and simulations.
 *
 * Oids are a deterministic, NON-cryptographic hash of (parents, message, pack),
 * so equal content gives equal oids across stores, as in git. Never use for real data.
 */
#ifndef TIX_STORE_MEM_H
#define TIX_STORE_MEM_H

#include "tix/store.h"

/* OWNS: free with tix_store_destroy. REQUIRES algo is SHA1 or SHA256 (sets oid length). */
tix_store *tix_store_mem_new(tix_hash_algo algo);

/*
 * Simulates `git fetch <remote>` with refspec +refs/tix/(all):refs/remotes/<remote>/tix/(all):
 * copies every commit of `src` missing from `dst`, then force-sets (and prunes)
 * dst's refs/remotes/<remote>/tix/... to src's refs/tix/... values.
 * REQUIRES: both are store_mem stores with the same algo; remote is a valid ref component.
 */
void tix_store_mem_fetch(tix_store *dst, tix_store *src, const char *remote);

/*
 * Registers `peer` as remote `name`, so tix_store_remote_fetch/push work like a
 * git remote (a "server" is just another store_mem nobody opens a repo on).
 * BORROWS peer: it must outlive s. REQUIRES a valid, not yet registered name.
 */
void tix_store_mem_add_remote(tix_store *s, const char *name, tix_store *peer);

/* RETURNS number of commits stored (for tests). */
size_t tix_store_mem_count(tix_store *s);

#endif

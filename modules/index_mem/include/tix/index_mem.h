/* tix/index_mem.h -- in-memory tix_index for tests. */
#ifndef TIX_INDEX_MEM_H
#define TIX_INDEX_MEM_H

#include "tix/index.h"

/* OWNS: free with tix_index_destroy. */
tix_index *tix_index_mem_new(void);
/* Test hooks: number of get calls that hit / missed since creation. */
size_t tix_index_mem_hits(tix_index *x);
size_t tix_index_mem_misses(tix_index *x);
/* Overwrites the stored bytes of (t, id), keeping its head (to test corrupt entries). */
void tix_index_mem_corrupt(tix_index *x, tix_entity_type t, const tix_oid *id, const char *data);

#endif

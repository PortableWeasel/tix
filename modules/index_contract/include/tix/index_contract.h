/* tix/index_contract.h -- the behaviour every tix_index adapter must have (test-only). */
#ifndef TIX_INDEX_CONTRACT_H
#define TIX_INDEX_CONTRACT_H

#include "tix/index.h"

/* Runs every contract case on an empty index x, reporting through testkit. */
void tix_index_contract_run(tix_index *x);

#endif

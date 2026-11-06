/*
 * tix/store_contract.h -- the behaviour every tix_store adapter must have
 * (test-only). Uses refs under refs/tix/contract/ and local names "contract-*",
 * so it can run on a store that also holds other data.
 */
#ifndef TIX_STORE_CONTRACT_H
#define TIX_STORE_CONTRACT_H

#include "tix/store.h"

/* Runs every contract case on s, reporting through testkit. */
void tix_store_contract_run(tix_store *s);

#endif

/* index_mem: the shared index contract. */
#include "tix/index_contract.h"
#include "tix/index_mem.h"
#include "tix/testkit.h"

int main(void)
{
	tix_index *x = tix_index_mem_new();
	tix_index_contract_run(x);
	tix_index_destroy(x);
	return tk_finish();
}

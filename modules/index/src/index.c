#include "tix/index.h"

#include <stdlib.h>

const char *tix_index_port_version(void)
{
	return "2";
}

void tix_index_rows_free(tix_index_rows *rows)
{
	if (!rows)
		return;
	for (size_t i = 0; i < rows->n; i++)
		tix_buf_free(&rows->v[i].summary);
	free(rows->v);
	rows->v = NULL;
	rows->n = 0;
}

/* index_sqlite: the shared contract (memory and file), persistence, recovery. */
#include "tix/index_contract.h"
#include "tix/index_sqlite.h"
#include "tix/testkit.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static tix_error e = TIX_ERROR_INIT;

static tix_oid oid(int n)
{
	tix_oid o;
	memset(&o, 0, sizeof(o));
	o.algo = TIX_HASH_SHA1;
	o.bytes[0] = (uint8_t)n;
	return o;
}

static void write_file(const char *path, const char *data)
{
	FILE *f = fopen(path, "wb");
	TIX_REQUIRE(f);
	fputs(data, f);
	fclose(f);
}

int main(void)
{
	tix_index *x = NULL;
	TK_OK(tix_index_sqlite_open(":memory:", &x, &e), &e);
	if (x)
		tix_index_contract_run(x);
	tix_index_destroy(x);

	char dir[] = "/tmp/tix-index-XXXXXX", path[256], side[300];
	TIX_REQUIRE(mkdtemp(dir));
	snprintf(path, sizeof(path), "%s/index.db", dir);
	x = NULL;
	TK_OK(tix_index_sqlite_open(path, &x, &e), &e);
	if (x)
		tix_index_contract_run(x);
	tix_oid a = oid(1), h = oid(2);
	tix_buf b = TIX_BUF_INIT;
	if (x)
		TK_OK(tix_index_put(x, TIX_ENTITY_TICKET, &a, &h, "kept", 4, NULL, 0, &e), &e);
	tix_index_destroy(x);

	/* persists across opens */
	x = NULL;
	TK_OK(tix_index_sqlite_open(path, &x, &e), &e);
	if (x) {
		TK_OK(tix_index_get(x, TIX_ENTITY_TICKET, &a, &h, &b, &e), &e);
		TK_CHECK(b.len == 4 && !memcmp(b.data, "kept", 4));
	}
	tix_index_destroy(x);

	/* a corrupt file is discarded and rebuilt, not an error */
	snprintf(side, sizeof(side), "%s-wal", path);
	unlink(side);
	snprintf(side, sizeof(side), "%s-shm", path);
	unlink(side);
	write_file(path, "this is not an sqlite database, not even close; just text to fill a page......");
	x = NULL;
	TK_OK(tix_index_sqlite_open(path, &x, &e), &e);
	if (x) {
		TK_STATUS(tix_index_get(x, TIX_ENTITY_TICKET, &a, &h, &b, &e), TIX_ERR_NOT_FOUND, &e);
		TK_OK(tix_index_put(x, TIX_ENTITY_TICKET, &a, &h, "again", 5, NULL, 0, &e), &e);
	}
	tix_index_destroy(x);

	/* a symlink or directory in its place is refused (never followed) */
	char link[300];
	snprintf(link, sizeof(link), "%s/link.db", dir);
	TIX_REQUIRE(!symlink(path, link));
	x = NULL;
	TK_STATUS(tix_index_sqlite_open(link, &x, &e), TIX_ERR_IO, &e);
	TK_STATUS(tix_index_sqlite_open(dir, &x, &e), TIX_ERR_IO, &e);
	TK_STATUS(tix_index_sqlite_open("/nonexistent-dir/x/index.db", &x, &e), TIX_ERR_IO, &e);
	unlink(link);

	snprintf(side, sizeof(side), "%s-wal", path);
	unlink(side);
	snprintf(side, sizeof(side), "%s-shm", path);
	unlink(side);
	unlink(path);
	rmdir(dir);
	tix_buf_free(&b);
	return tk_finish();
}

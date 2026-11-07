/* tix-tui: a composition root like tix and tix-web (architecture §14.5). */
#include "tui.h"

#include "tix/index_sqlite.h"
#include "tix/store_libgit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

static int64_t env_now(void *ctx)
{
	(void)ctx;
	return (int64_t)time(NULL);
}

static void env_random(void *ctx, uint8_t *out, size_t n)
{
	(void)ctx;
	for (size_t got = 0; got < n;) {
		ssize_t r = getrandom(out + got, n - got, 0);
		if (r < 0) {
			perror("tix tui: getrandom");
			abort(); /* never a predictable nonce */
		}
		got += (size_t)r;
	}
}

static tix_date today(void)
{
	const char *fixed = getenv("TIX_TODAY");
	tix_date d;
	if (fixed && tix_date_parse(fixed, &d))
		return d;
	time_t now = time(NULL);
	struct tm tm;
	char buf[16];
	if (!localtime_r(&now, &tm) || !strftime(buf, sizeof(buf), "%Y-%m-%d", &tm) || !tix_date_parse(buf, &d))
		return tix_date_from_unix((int64_t)now);
	return d;
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		bool help = !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help");
		fputs("usage: tix tui\n  tickets, WBS and Gantt in the terminal; ? inside for the keys\n", help ? stdout : stderr);
		return help ? 0 : 2;
	}
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		fprintf(stderr, "tix tui: needs a terminal\n");
		return 2;
	}
	tix_error err = TIX_ERROR_INIT;
	tix_store *store = NULL;
	tix_index *index = NULL;
	tix_repo *repo = NULL;
	if (tix_store_libgit_open(&store, &err)) {
		fprintf(stderr, "tix tui: %s\n", err.msg);
		return 1;
	}
	tix_buf path = TIX_BUF_INIT, b = TIX_BUF_INIT, ed = TIX_BUF_INIT;
	const char *off = getenv("TIX_NO_INDEX");
	if ((off && *off && strcmp(off, "0")) || tix_store_libgit_local_path(store, "index.db", &path, NULL) ||
	    tix_index_sqlite_open(path.data, &index, NULL))
		index = NULL;
	tix_buf_free(&path);
	tix_repo_config cfg = {.store = store, .env = {env_now, env_random, NULL}, .index = index};
	if (!tix_store_libgit_config_get(store, "tix.identity", &b, NULL) && !tix_oid_from_hex(&cfg.identity, b.data, b.len))
		memset(&cfg.identity, 0, sizeof(cfg.identity));
	if (tix_repo_open(&repo, &cfg, &err)) {
		memset(&cfg.identity, 0, sizeof(cfg.identity)); /* run read-only-ish without one */
		err = (tix_error)TIX_ERROR_INIT;
		if (tix_repo_open(&repo, &cfg, &err)) {
			fprintf(stderr, "tix tui: %s\n", err.msg);
			tix_buf_free(&b);
			tix_index_destroy(index);
			tix_store_destroy(store);
			return 1;
		}
	}
	/* $TIX_EDITOR, core.editor, $VISUAL, $EDITOR, vi: as the CLI */
	const char *e = getenv("TIX_EDITOR");
	if (e && *e)
		tix_buf_adds(&ed, e);
	else if (tix_store_libgit_config_get(store, "core.editor", &ed, NULL) || !ed.len) {
		tix_buf_reset(&ed);
		tix_buf_adds(&ed, ((e = getenv("VISUAL")) && *e) || ((e = getenv("EDITOR")) && *e) ? e : "vi");
	}
	tui_config tc = {repo, ed.data, today};
	int rc = tui_run(&tc);
	tix_repo_close(repo);
	tix_index_destroy(index);
	tix_store_destroy(store);
	tix_buf_free(&b);
	tix_buf_free(&ed);
	return rc;
}

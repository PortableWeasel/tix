/* tix -- distributed, git-native ticket tracker. Composition root. */
#include "cli.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

static const char usage_text[] =
	"usage: tix <command> [<args>]\n"
	"\n"
	"identities\n"
	"  id new [--name N] [--email E]     create an identity (defaults from git config)\n"
	"  id ls | id show [ID] | id use ID   list / show / choose the identity you write as\n"
	"  id edit [--name N] [--email E] [--add-key KIND FPR PUBKEY] [--remove-key FPR]\n"
	"\n"
	"tickets\n"
	"  new [-t TITLE] [-m BODY] [-l LABEL]... [PLANNING]   create (opens $EDITOR without -t)\n"
	"  ls [--all] [--status S]... [--label L] [--author ID] [--assignee RES] [--under ID]\n"
	"  show ID\n"
	"  comment ID [-m TEXT] [--edit COMMENT-ID]\n"
	"  edit ID [--title T] [--body B] [--status S] [+LABEL]... [-LABEL]... [PLANNING]\n"
	"\n"
	"planning options (new, edit; 'none' clears a value)\n"
	"  --parent ID   --rank first|last|before:ID|after:ID   --kind task|milestone\n"
	"  --calendar CAL   --mode auto|manual   --duration 3d   --effort 16h   --progress 40\n"
	"  --start D  --finish D  --deadline D  --actual-start D  --actual-finish D\n"
	"  --dep +ID[:FS|SS|FF|SF[+2d|-1d]]   --dep -ID          (D: YYYY-MM-DD, today, tomorrow)\n"
	"  assign ID RES[:50%] [--effort 8h]      unassign ID RES\n"
	"\n"
	"resources and calendars\n"
	"  res new NAME [--kind person|role|equipment] [--capacity 300%] [--calendar CAL]\n"
	"              [--identity ID|me] [--rate \"95.50 EUR\"]\n"
	"  res ls | res show RES | res edit RES [same options] [--name N] [--active yes|no]\n"
	"  cal new NAME [--weekdays mon-fri] [--hours 8] [--off D[..D][=NOTE]] [--on D[..D]]\n"
	"  cal ls | cal show CAL | cal edit CAL [same options] [--name N] [--clear D[..D]]\n"
	"\n"
	"plans (ROOT: a ticket; default every ticket; --today D overrides today)\n"
	"  wbs [ROOT]                            tree with WBS codes, dates and rollups\n"
	"  plan [ROOT] [--gantt] [--critical] [--load]\n"
	"  export [ROOT] --format mermaid|csv|json\n"
	"\n"
	"sync (REMOTE defaults to git config tix.remote, else origin)\n"
	"  pull [REMOTE]    fetch, validate and merge the remote's tickets and identities\n"
	"  push [REMOTE]    send yours (never forced)\n"
	"  sync [REMOTE]    pull, then push; repeats if the remote moved meanwhile\n"
	"\n"
	"statuses: open, in-progress, blocked, done, cancelled\n";

int cli_fail(cli *c, tix_status st)
{
	fprintf(stderr, "tix: %s\n", c->err.msg[0] ? c->err.msg : tix_status_name(st));
	return EXIT_ERR;
}

int cli_usage(const char *fmt, ...)
{
	va_list ap;
	fputs("tix: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputs("\n(see `tix help`)\n", stderr);
	return EXIT_USAGE;
}

int cli_resolve(cli *c, tix_entity_type t, const char *arg, tix_oid *out)
{
	tix_status st = tix_entity_resolve(c->repo, t, arg, out, &c->err);
	return st ? cli_fail(c, st) : EXIT_OK;
}

const char *cli_short(const tix_oid *id, char *out)
{
	char hex[TIX_OID_MAX_HEX + 1];
	tix_oid_hex(id, hex);
	memcpy(out, hex, TIX_ID_MIN_PREFIX);
	out[TIX_ID_MIN_PREFIX] = '\0';
	return out;
}

const char *cli_time(int64_t t, char *out)
{
	time_t tt = (time_t)t;
	struct tm tm;
	if (!gmtime_r(&tt, &tm) || !strftime(out, 32, "%Y-%m-%d %H:%M UTC", &tm))
		snprintf(out, 32, "@%lld", (long long)t);
	return out;
}

const char *cli_who(cli *c, const tix_oid *id, char *out, size_t n)
{
	tix_entity *e;
	char s[16];
	if (!tix_entity_load(c->repo, TIX_ENTITY_IDENTITY, id, &e, NULL)) {
		snprintf(out, n, "%s", e->u.identity.name);
		tix_entity_free(e);
	} else {
		snprintf(out, n, "%s (unknown identity)", cli_short(id, s));
	}
	return out;
}

static int64_t env_now(void *ctx)
{
	(void)ctx;
	return (int64_t)time(NULL);
}

static void env_random(void *ctx, uint8_t *out, size_t n)
{
	(void)ctx;
	size_t got = 0;
	while (got < n) {
		ssize_t r = getrandom(out + got, n - got, 0);
		if (r < 0) {
			perror("tix: getrandom");
			abort(); /* never write a predictable nonce */
		}
		got += (size_t)r;
	}
}

/* Reads git config tix.identity; a missing or malformed value means "none". */
static void configured_identity(cli *c, tix_oid *out)
{
	tix_buf b = TIX_BUF_INIT;
	memset(out, 0, sizeof(*out));
	if (!tix_store_libgit_config_get(c->store, "tix.identity", &b, NULL) &&
	    !tix_oid_from_hex(out, b.data, b.len))
		fprintf(stderr, "tix: ignoring malformed tix.identity '%s'\n", b.data);
	tix_buf_free(&b);
}

int main(int argc, char **argv)
{
	cli c = {0};
	if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
		fputs(usage_text, argc < 2 ? stderr : stdout);
		return argc < 2 ? EXIT_USAGE : EXIT_OK;
	}
	static const struct {
		const char *name;
		int (*fn)(cli *, int, char **);
	} cmds[] = {
		{"id", cmd_id}, {"new", cmd_new}, {"ls", cmd_ls}, {"show", cmd_show},
		{"comment", cmd_comment}, {"edit", cmd_edit}, {"pull", cmd_pull},
		{"push", cmd_push}, {"sync", cmd_sync}, {"assign", cmd_assign},
		{"unassign", cmd_unassign}, {"res", cmd_res}, {"cal", cmd_cal},
		{"wbs", cmd_wbs}, {"plan", cmd_plan}, {"export", cmd_export},
	};
	int (*fn)(cli *, int, char **) = NULL;
	for (size_t i = 0; i < sizeof(cmds) / sizeof(*cmds); i++)
		if (!strcmp(argv[1], cmds[i].name))
			fn = cmds[i].fn;
	if (!fn)
		return cli_usage("unknown command '%s'", argv[1]);

	tix_status st = tix_store_libgit_open(&c.store, &c.err);
	if (st)
		return cli_fail(&c, st);
	tix_repo_config cfg = {c.store, {env_now, env_random, NULL}, {0}};
	configured_identity(&c, &cfg.identity);
	if ((st = tix_repo_open(&c.repo, &cfg, &c.err))) {
		if (tix_oid_is_null(&cfg.identity))
			return cli_fail(&c, st);
		/* a configured identity that doesn't load: run without one and say so */
		fprintf(stderr, "tix: configured tix.identity unusable (%s)\n", c.err.msg);
		memset(&cfg.identity, 0, sizeof(cfg.identity));
		c.err = (tix_error)TIX_ERROR_INIT;
		if ((st = tix_repo_open(&c.repo, &cfg, &c.err)))
			return cli_fail(&c, st);
	}
	int rc = fn(&c, argc - 1, argv + 1);
	tix_repo_close(c.repo);
	tix_store_destroy(c.store);
	return rc;
}

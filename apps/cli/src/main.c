/* tix -- distributed, git-native ticket tracker. Composition root. */
#include "cli.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

static const char usage_text[] =
	"usage: tix <command> [<args>]      (tix --version: versions)\n"
	"\n"
	"identities\n"
	"  id new [--name N] [--email E] [--kind human|agent]   create an identity (defaults from git config)\n"
	"  id ls | id show [ID] | id use ID   list / show / choose the identity you write as\n"
	"  id edit [--name N] [--email E] [--kind human|agent] [--add-key KIND FPR PUBKEY] [--add-signing-key]\n"
	"          [--remove-key FPR]\n"
	"\n"
	"tickets\n"
	"  new [-t TITLE] [-m BODY] [-l LABEL]... [--status S] [PLANNING]   create ($EDITOR without -t)\n"
	"  ls [--all] [--status S]... [--label L] [--author ID] [--assignee RES] [--under ID]\n"
	"     [--sort status|wbs|updated|newest]   (default: active first, then WBS order)\n"
	"  show ID\n"
	"  comment ID [-m TEXT] [--edit COMMENT-ID]\n"
	"  note ID --kind K [-m TEXT] [--reply-to COMMENT-ID] [--data JSON | --data-file F|-]\n"
	"       [--field K=V]... [--item ID=TEXT[=STATE]]... [--question ID=TEXT]... [--update CID#ITEM=STATE]...\n"
	"       a typed comment: a kind plus structured data (questions, review findings, ...)\n"
	"  edit ID [--title T] [--body B] [--status S] [+LABEL]... [-LABEL]... [PLANNING]\n"
	"\n"
	"workflow stages (S may be PHASE/STAGE, e.g. open/ready; lists are inherited down the WBS)\n"
	"  stage ls [ID]                        the stage list that applies to a ticket (or every list)\n"
	"  stage add ID NAME --phase P [--human] [--rank N]   stage rm ID NAME\n"
	"\n"
	"claims (who works on what; leases end by themselves)\n"
	"  claim ID [--for 30m] | claim ID --renew [--for D] | claim ID --release\n"
	"  claim --next [--status S]... [--under ID] [--for 30m] [--remote R | --local]\n"
	"        first free ticket in WBS order whose predecessors are done and whose stage\n"
	"        doesn't wait for a person; syncs through the remote; exit 3 = no work\n"
	"\n"
	"autonomy (agents keep working without a person; set on a WBS root, inherited)\n"
	"  autonomy ID on --until 08:00 | --for 10h | --no-expiry     autonomy ID off | halt | show\n"
	"        while on, agents can't park tickets in stages that wait for a person\n"
	"        (blocked stages excepted); agents may only halt\n"
	"\n"
	"for agents and scripts (--json: one JSON value per line)\n"
	"  ls, show, claim --next, stage ls, autonomy show: add --json\n"
	"  log [ID] [--since 2h|DATE] [--under ID] [--limit N] [--json]   ticket activity, oldest first\n"
	"  watch [--under ID] [--pull 30s] [--max-events N] [--json]     one line per change, as it happens\n"
	"\n";
static const char usage_text2[] =
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
	"  export [ROOT] --format mermaid|csv|json|mspdi   (mspdi: MS Project XML)\n"
	"  (wbs, plan and export take --baseline B: slips and deltas against a baseline)\n"
	"\n"
	"baselines (snapshots of the plan that sync)\n"
	"  baseline take NAME [ROOT] [--today D]   baseline ls | show B | rename B NAME\n"
	"\n"
	"signatures (git config tix.sign, tix.verify off|warn|require)\n"
	"  id edit --add-signing-key          add your git signing key (user.signingkey) to your identity\n"
	"  verify [ID]...                     check commit signatures (default: every entity)\n"
	"  pull | sync [--verify off|warn|require] [REMOTE]\n"
	"\n"
	"issue trackers (Forgejo; token from TIX_FORGEJO_TOKEN or a git credential helper)\n"
	"  bridge pull|push|sync [--url https://host/owner/repo] [--under ID | --label L | --all]\n"
	"         [--no-comments] [--dry-run]     (git config tix.bridge.url for the default URL)\n"
	"\n"
	"sync (REMOTE defaults to git config tix.remote, else origin)\n"
	"  pull [REMOTE]    fetch, validate and merge the remote's tickets and identities\n"
	"  push [REMOTE]    send yours (never forced)\n"
	"  sync [REMOTE]    pull, then push; repeats if the remote moved meanwhile\n"
	"\n"
	"web and terminal\n"
	"  web [--listen 127.0.0.1:7480] [--read-only] [--no-open]   browser UI for this repository\n"
	"  tui                              tickets, WBS and Gantt in the terminal (? for keys)\n"
	"\n"
	"cache\n"
	"  index status | index clear      $GIT_DIR/tix/index.db (disposable; TIX_NO_INDEX=1 disables)\n"
	"\n"
	"statuses: open, in-progress, blocked, done, cancelled, each optionally /STAGE\n";

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

/* The cache is optional: any failure just means running without it. */
static void open_index(cli *c)
{
	const char *off = getenv("TIX_NO_INDEX");
	tix_buf path = TIX_BUF_INIT;
	if ((off && *off && strcmp(off, "0")) ||
	    tix_store_libgit_local_path(c->store, "index.db", &path, NULL) ||
	    tix_index_sqlite_open(path.data, &c->index, NULL))
		c->index = NULL;
	tix_buf_free(&path);
}

int cmd_index(cli *c, int argc, char **argv)
{
	size_t n;
	if (argc != 2 || (strcmp(argv[1], "status") && strcmp(argv[1], "clear")))
		return cli_usage("usage: tix index status|clear");
	if (!c->index) {
		printf("index: off (TIX_NO_INDEX set, or $GIT_DIR/tix/index.db unusable)\n");
		return EXIT_OK;
	}
	tix_status st = !strcmp(argv[1], "clear") ? tix_index_clear(c->index, &c->err) : TIX_OK;
	if (!st)
		st = tix_index_count(c->index, &n, &c->err);
	if (st)
		return cli_fail(c, st);
	printf("index: %zu cached entit%s\n", n, n == 1 ? "y" : "ies");
	return EXIT_OK;
}

/* `tix web` / `tix tui`: exec tix-<name> from this executable's directory, else from PATH
 * (argv only). */
static int exec_sibling(const char *name, int argc, char **argv)
{
	char self[4096], path[4200];
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	char **args = calloc((size_t)argc + 1, sizeof(char *));
	if (!args)
		return EXIT_ERR;
	char prog[32];
	snprintf(prog, sizeof(prog), "tix-%s", name);
	args[0] = prog;
	for (int i = 2; i < argc; i++)
		args[i - 1] = argv[i];
	if (n > 0) {
		self[n] = '\0';
		char *slash = strrchr(self, '/');
		if (slash) {
			*slash = '\0';
			snprintf(path, sizeof(path), "%s/%s", self, prog);
			execv(path, args);
		}
	}
	execvp(prog, args);
	fprintf(stderr, "tix: can't run %s (installed next to tix?)\n", prog);
	free(args);
	return EXIT_ERR;
}

int main(int argc, char **argv)
{
	cli c = {0};
	if (argc >= 2 && (!strcmp(argv[1], "web") || !strcmp(argv[1], "tui")))
		return exec_sibling(argv[1], argc, argv);
	if (argc == 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "version"))) {
		printf("tix %s (git.git %s)\n", TIX_VERSION, tix_store_libgit_git_version());
		return EXIT_OK;
	}
	if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
		fputs(usage_text, argc < 2 ? stderr : stdout);
		fputs(usage_text2, argc < 2 ? stderr : stdout);
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
		{"wbs", cmd_wbs}, {"plan", cmd_plan}, {"export", cmd_export}, {"index", cmd_index},
		{"baseline", cmd_baseline}, {"verify", cmd_verify}, {"bridge", cmd_bridge}, {"stage", cmd_stage},
		{"claim", cmd_claim}, {"note", cmd_note}, {"autonomy", cmd_autonomy},
		{"log", cmd_log}, {"watch", cmd_watch},
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
	open_index(&c);
	tix_repo_config cfg = {.store = c.store, .env = {env_now, env_random, NULL}, .index = c.index};
	configured_identity(&c, &cfg.identity);
	tix_buf vb = TIX_BUF_INIT;
	if (!tix_store_libgit_config_get(c.store, "tix.verify", &vb, NULL)) {
		bool ok;
		cfg.verify = tix_verify_policy_parse(vb.data, &ok);
		if (!ok)
			fprintf(stderr, "tix: ignoring tix.verify '%s' (off, warn or require)\n", vb.data);
	}
	tix_buf_free(&vb);
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
	tix_index_destroy(c.index);
	tix_store_destroy(c.store);
	return rc;
}

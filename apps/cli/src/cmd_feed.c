/* tix log | watch, and the JSON shapes the CLI prints (architecture §15.6) */
#include "cli.h"
#include "tix/base_json.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void j_id(tix_jw *w, const char *k, const tix_oid *id)
{
	char hex[TIX_OID_MAX_HEX + 1];
	tix_jw_key(w, k);
	if (tix_oid_is_null(id))
		tix_jw_null(w);
	else
		tix_jw_str(w, tix_oid_hex(id, hex));
}

static void j_str(tix_jw *w, const char *k, const char *v)
{
	tix_jw_key(w, k);
	if (v)
		tix_jw_str(w, v);
	else
		tix_jw_null(w);
}

void cli_json_ticket(cli *c, tix_jw *w, const tix_entity *e, int64_t now)
{
	const tix_ticket *t = &e->u.ticket;
	char s[16], who[300];
	tix_jw_obj_begin(w);
	j_id(w, "id", &e->id);
	j_str(w, "short", cli_short(&e->id, s));
	j_str(w, "title", t->title);
	j_str(w, "status", tix_ticket_status_name(t->status));
	j_str(w, "stage", t->stage);
	tix_jw_key(w, "labels");
	tix_jw_arr_begin(w);
	for (size_t i = 0; i < t->labels.n; i++)
		tix_jw_str(w, t->labels.v[i]);
	tix_jw_arr_end(w);
	j_id(w, "parent", &t->plan.parent);
	tix_jw_key(w, "claim");
	if (tix_oid_is_null(&t->claim.identity)) {
		tix_jw_null(w);
	} else {
		tix_jw_obj_begin(w);
		j_id(w, "identity", &t->claim.identity);
		j_str(w, "name", cli_who(c, &t->claim.identity, who, sizeof(who)));
		tix_jw_key(w, "until");
		tix_jw_int(w, t->claim.until);
		tix_jw_key(w, "live");
		tix_jw_bool(w, tix_claim_live(&t->claim, now));
		tix_jw_obj_end(w);
	}
	tix_jw_key(w, "updated");
	tix_jw_int(w, e->updated);
	tix_jw_obj_end(w);
}

static void json_activity(cli *c, tix_jw *w, const tix_activity *a)
{
	char opid[TIX_OID_MAX_HEX + 12], s[16], who[300];
	tix_jw_obj_begin(w);
	tix_jw_key(w, "time");
	tix_jw_int(w, a->time);
	j_str(w, "id", tix_opid_str(&a->id, opid));
	j_id(w, "ticket", &a->ticket);
	j_str(w, "short", cli_short(&a->ticket, s));
	j_str(w, "title", a->title);
	j_id(w, "author", &a->author);
	j_str(w, "name", cli_who(c, &a->author, who, sizeof(who)));
	j_str(w, "op", a->op);
	j_str(w, "summary", a->summary);
	tix_jw_obj_end(w);
}

static void text_activity(cli *c, const tix_activity *a)
{
	char s[16], when[32], who[300];
	printf("%s  %s  %s: %s  (%s)\n", cli_time(a->time, when), cli_short(&a->ticket, s), a->title, a->summary,
	       cli_who(c, &a->author, who, sizeof(who)));
}

/* "2h" / "30m" / "3d" ago, "YYYY-MM-DD" or "YYYY-MM-DD HH:MM" (local). RETURNS -1 if bad. */
static int64_t parse_since(const char *s, int64_t now)
{
	char *end;
	long long v = strtoll(s, &end, 10);
	if (end != s && v >= 0 && *end && !end[1]) {
		if (*end == 'm')
			return now - v * 60;
		if (*end == 'h')
			return now - v * 3600;
		if (*end == 'd')
			return now - v * 86400;
	}
	struct tm tm;
	memset(&tm, 0, sizeof(tm));
	int y, mo, d, h = 0, mi = 0;
	char tail;
	int n = sscanf(s, "%d-%d-%d %d:%d%c", &y, &mo, &d, &h, &mi, &tail);
	if (n != 3 && n != 5)
		return -1;
	tm.tm_year = y - 1900;
	tm.tm_mon = mo - 1;
	tm.tm_mday = d;
	tm.tm_hour = h;
	tm.tm_min = mi;
	tm.tm_isdst = -1;
	time_t t = mktime(&tm);
	return t == (time_t)-1 ? -1 : (int64_t)t;
}

int cmd_log(cli *c, int argc, char **argv)
{
	tix_activity_query q = {0};
	tix_oid under, one;
	bool json = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--json")) {
			json = true;
		} else if (!strcmp(argv[i], "--since") && i + 1 < argc) {
			if ((q.since = parse_since(argv[++i], (int64_t)time(NULL))) < 0)
				return cli_usage("--since is like 2h, 3d, 2026-11-07 or \"2026-11-07 20:00\"");
		} else if (!strcmp(argv[i], "--under") && i + 1 < argc) {
			int rc = cli_resolve(c, TIX_ENTITY_TICKET, argv[++i], &under);
			if (rc)
				return rc;
			q.under = &under;
		} else if (!strcmp(argv[i], "--limit") && i + 1 < argc) {
			char *end;
			long n = strtol(argv[++i], &end, 10);
			if (*end || n < 1)
				return cli_usage("--limit is a positive number");
			q.limit = (size_t)n;
		} else if (argv[i][0] != '-' && !q.ticket) {
			int rc = cli_resolve(c, TIX_ENTITY_TICKET, argv[i], &one);
			if (rc)
				return rc;
			q.ticket = &one;
		} else {
			return cli_usage("log: unexpected '%s'", argv[i]);
		}
	}
	tix_activity_list l;
	tix_status st = tix_activity_log(c->repo, &q, &l, &c->err);
	if (st)
		return cli_fail(c, st);
	for (size_t i = 0; i < l.n; i++) {
		if (json) {
			tix_buf b = TIX_BUF_INIT;
			tix_jw w;
			tix_jw_init(&w, &b);
			json_activity(c, &w, &l.v[i]);
			puts(tix_buf_cstr(&b));
			tix_buf_free(&b);
		} else {
			text_activity(c, &l.v[i]);
		}
	}
	tix_activity_list_free(&l);
	return EXIT_OK;
}

/* ---- watch ------------------------------------------------------------------------------ */

static volatile sig_atomic_t stop_watch;
static void on_stop(int sig)
{
	(void)sig;
	stop_watch = 1;
}

typedef struct head_entry {
	tix_oid id, head;
} head_entry;
typedef TIX_VEC(head_entry) head_vec;

static tix_status heads(cli *c, head_vec *out, tix_error *err)
{
	tix_refs refs;
	const char *prefix = tix_entity_ref_prefix(TIX_ENTITY_TICKET);
	out->n = 0;
	tix_status st = tix_store_ref_list(c->store, prefix, &refs, err);
	if (st)
		return st;
	size_t plen = strlen(prefix);
	for (size_t i = 0; i < refs.n; i++) {
		head_entry h;
		const char *hex = refs.v[i].name + plen;
		if (!tix_oid_from_hex(&h.id, hex, strlen(hex)))
			continue;
		h.head = refs.v[i].oid;
		TIX_VEC_PUSH(out, h);
	}
	tix_refs_free(&refs);
	return TIX_OK;
}

static const head_entry *find_head(const head_vec *v, const tix_oid *id)
{
	for (size_t i = 0; i < v->n; i++)
		if (tix_oid_eq(&v->v[i].id, id))
			return &v->v[i];
	return NULL;
}

static bool is_under(cli *c, const tix_oid *id, const tix_oid *root)
{
	tix_oid cur = *id;
	for (int depth = 0; depth < 100000; depth++) {
		if (tix_oid_eq(&cur, root))
			return true;
		tix_entity *e;
		if (tix_entity_load(c->repo, TIX_ENTITY_TICKET, &cur, &e, NULL))
			return false;
		bool top = tix_oid_is_null(&e->u.ticket.plan.parent);
		cur = e->u.ticket.plan.parent;
		tix_entity_free(e);
		if (top)
			return false;
	}
	return false;
}

int cmd_watch(cli *c, int argc, char **argv)
{
	tix_oid under;
	const tix_oid *underp = NULL;
	bool json = false;
	long pull_every = 0, max_events = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--json")) {
			json = true;
		} else if (!strcmp(argv[i], "--under") && i + 1 < argc) {
			int rc = cli_resolve(c, TIX_ENTITY_TICKET, argv[++i], &under);
			if (rc)
				return rc;
			underp = &under;
		} else if (!strcmp(argv[i], "--pull") && i + 1 < argc) {
			char *end;
			pull_every = strtol(argv[++i], &end, 10);
			if (*end == 'm')
				pull_every *= 60, end++;
			else if (*end == 's')
				end++;
			if (*end || pull_every < 5)
				return cli_usage("--pull takes an interval of at least 5s, like 30s or 2m");
		} else if (!strcmp(argv[i], "--max-events") && i + 1 < argc) {
			char *end;
			max_events = strtol(argv[++i], &end, 10);
			if (*end || max_events < 1)
				return cli_usage("--max-events is a positive number");
		} else {
			return cli_usage("watch: unexpected '%s'", argv[i]);
		}
	}
	tix_buf remote = TIX_BUF_INIT, key = TIX_BUF_INIT, url = TIX_BUF_INIT;
	if (pull_every) {
		if (tix_store_libgit_config_get(c->store, "tix.remote", &remote, NULL)) {
			tix_buf_reset(&remote);
			tix_buf_adds(&remote, "origin");
		}
		tix_buf_addf(&key, "remote.%s.url", remote.data);
		if (tix_store_libgit_config_get(c->store, key.data, &url, NULL)) {
			tix_buf_free(&remote);
			tix_buf_free(&key);
			tix_buf_free(&url);
			return cli_usage("watch --pull: remote '%s' isn't configured", remote.data);
		}
	}
	tix_buf_free(&key);
	tix_buf_free(&url);
	signal(SIGINT, on_stop);
	signal(SIGTERM, on_stop);
	setvbuf(stdout, NULL, _IOLBF, 0);
	head_vec before = {0}, now_heads = {0};
	tix_status st = heads(c, &before, &c->err);
	long events = 0;
	time_t last_pull = time(NULL);
	while (!st && !stop_watch && (!max_events || events < max_events)) {
		struct timespec ts = {1, 0};
		nanosleep(&ts, NULL);
		if (stop_watch)
			break;
		if (pull_every && time(NULL) - last_pull >= pull_every) {
			tix_sync_report rep = {0};
			tix_error perr = TIX_ERROR_INIT;
			if (tix_pull(c->repo, remote.data, &rep, &perr))
				fprintf(stderr, "tix: watch: pull failed: %s\n", perr.msg);
			tix_sync_report_free(&rep);
			last_pull = time(NULL);
		}
		if ((st = tix_repo_refresh(c->repo, &c->err)) || (st = heads(c, &now_heads, &c->err)))
			break;
		for (size_t i = 0; i < now_heads.n && (!max_events || events < max_events); i++) {
			const head_entry *h = &now_heads.v[i], *old = find_head(&before, &h->id);
			if (old && tix_oid_eq(&old->head, &h->head))
				continue;
			if (underp && !is_under(c, &h->id, underp))
				continue;
			tix_activity_query q = {.ticket = &h->id, .after = old ? &old->head : NULL};
			tix_activity_list l;
			if (tix_activity_log(c->repo, &q, &l, NULL))
				continue; /* doesn't validate (yet): say nothing */
			if (!l.n) {
				tix_activity_list_free(&l);
				continue; /* a merge without new ops */
			}
			events++;
			if (json) {
				tix_buf b = TIX_BUF_INIT;
				tix_jw w;
				char s[16];
				tix_jw_init(&w, &b);
				tix_jw_obj_begin(&w);
				j_id(&w, "ticket", &h->id);
				j_str(&w, "short", cli_short(&h->id, s));
				j_str(&w, "title", l.v[0].title);
				j_id(&w, "head", &h->head);
				tix_jw_key(&w, "ops");
				tix_jw_arr_begin(&w);
				for (size_t k = 0; k < l.n; k++)
					json_activity(c, &w, &l.v[k]);
				tix_jw_arr_end(&w);
				tix_jw_obj_end(&w);
				puts(tix_buf_cstr(&b));
				tix_buf_free(&b);
			} else {
				for (size_t k = 0; k < l.n; k++)
					text_activity(c, &l.v[k]);
			}
			tix_activity_list_free(&l);
		}
		head_vec tmp = before;
		before = now_heads;
		now_heads = tmp;
	}
	free(before.v);
	free(now_heads.v);
	tix_buf_free(&remote);
	return st ? cli_fail(c, st) : EXIT_OK;
}

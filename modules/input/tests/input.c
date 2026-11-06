/* input: the shared text grammar and field -> op mapping, on core over store_mem. */
#include "tix/input.h"
#include "tix/store_mem.h"
#include "tix/testkit.h"

#include <string.h>

static tix_error e = TIX_ERROR_INIT;

typedef struct fake_env {
	int64_t now;
	uint64_t rng;
} fake_env;

static int64_t fake_now(void *ctx)
{
	return ((fake_env *)ctx)->now += 60;
}

static void fake_random(void *ctx, uint8_t *out, size_t n)
{
	fake_env *f = ctx;
	for (size_t i = 0; i < n; i++)
		out[i] = (uint8_t)tk_rand(&f->rng);
}

static void test_grammar(void)
{
	tix_date today, d, from, to;
	int64_t n;
	const char *note;
	TK_CHECK(tix_date_parse("2026-11-06", &today));
	TK_CHECK(tix_input_date("today", today, &d) && d == today);
	TK_CHECK(tix_input_date("tomorrow", today, &d) && d == today + 1);
	TK_CHECK(tix_input_date("2026-02-28", today, &d) && !tix_input_date("2026-02-30", today, &d));
	TK_CHECK(!tix_input_date("", today, &d) && !tix_input_date("Today", today, &d));
	TK_CHECK(tix_input_count("3d", "d", 10, &n) && n == 3 && tix_input_count("3", "d", 10, &n));
	TK_CHECK(!tix_input_count("3x", "d", 10, &n) && !tix_input_count("11", "d", 10, &n));
	TK_CHECK(!tix_input_count("-1", "d", 10, &n) && !tix_input_count(" 1", "d", 10, &n));
	TK_CHECK(!tix_input_count("", "", 10, &n) && !tix_input_count("99999999999999999999", "", 10, &n));
	TK_CHECK(tix_input_is_none("none") && tix_input_is_none("-") && !tix_input_is_none(""));
	TK_CHECK(tix_input_weekdays("mon-fri", &n) && n == 0x1f);
	TK_CHECK(tix_input_weekdays("mon,wed,sun", &n) && n == (1 | 4 | 64));
	TK_CHECK(!tix_input_weekdays("fri-mon", &n) && !tix_input_weekdays("mon,", &n) && !tix_input_weekdays("", &n));
	TK_CHECK(tix_input_range("2026-12-24..2026-12-26=Holidays", today, &from, &to, &note));
	TK_CHECK(to - from == 2 && !strcmp(note, "Holidays"));
	TK_CHECK(tix_input_range("today", today, &from, &to, &note) && from == today && to == today && !*note);
	TK_CHECK(!tix_input_range("2026-12-24..", today, &from, &to, NULL));
	TK_CHECK(tix_input_amount("95.5", &n) && n == 9550 && tix_input_amount("7", &n) && n == 700);
	TK_CHECK(!tix_input_amount("9.", &n) && !tix_input_amount(".5", &n) && !tix_input_amount("1.234", &n));
	TK_CHECK(!tix_input_amount("100000001", &n));
}

static void test_fields(void)
{
	fake_env env = {1700000000, 7};
	tix_store *s = tix_store_mem_new(TIX_HASH_SHA1);
	tix_repo_config cfg = {.store = s, .env = {fake_now, fake_random, &env}};
	tix_repo *r = NULL;
	TK_OK(tix_repo_open(&r, &cfg, &e), &e);
	tix_oid me, a, b, res, cal;
	TK_OK(tix_identity_create(r, "Ada", "ada@example.org", &me, &e), &e);
	TK_OK(tix_repo_set_identity(r, &me, &e), &e);
	TK_OK(tix_ticket_create(r, "A", "", NULL, &a, &e), &e);
	TK_OK(tix_ticket_create(r, "B", "", NULL, &b, &e), &e);
	TK_OK(tix_resource_create(r, "Crew", TIX_RES_ROLE, &res, &e), &e);
	TK_OK(tix_calendar_create(r, "Site", &cal, &e), &e);
	char ahex[TIX_OID_MAX_HEX + 1], rhex[TIX_OID_MAX_HEX + 1], chex[TIX_OID_MAX_HEX + 1], buf[160];
	tix_oid_hex(&a, ahex);
	tix_oid_hex(&res, rhex);
	tix_oid_hex(&cal, chex);
	ahex[8] = rhex[8] = chex[8] = '\0'; /* prefixes, as users type them */

	tix_input_ctx ctx = {r, 0};
	TK_CHECK(tix_date_parse("2026-11-06", &ctx.today));
	tix_input in = {0};
	const tix_entity_type T = TIX_ENTITY_TICKET;

	TK_CHECK(tix_input_has(T, "dep") && tix_input_has(TIX_ENTITY_RESOURCE, "rate") && !tix_input_has(T, "rate"));
	TK_CHECK(!tix_input_has(T, "title") && tix_input_has(TIX_ENTITY_CALENDAR, "off"));

	TK_OK(tix_input_field(&ctx, T, "start", "tomorrow", &in, &e), &e);
	TK_CHECK(in.ops.v[0].kind == TIX_OP_SET_FIELD && in.ops.v[0].u.set.field == TIX_F_START);
	TK_CHECK(in.ops.v[0].u.set.v.kind == TIX_V_DATE && in.ops.v[0].u.set.v.i == ctx.today + 1);
	TK_OK(tix_input_field(&ctx, T, "deadline", "none", &in, &e), &e);
	TK_CHECK(in.ops.v[1].u.set.v.kind == TIX_V_NULL);
	TK_OK(tix_input_field(&ctx, T, "duration", "3d", &in, &e), &e);
	TK_OK(tix_input_field(&ctx, T, "effort", "16h", &in, &e), &e);
	TK_OK(tix_input_field(&ctx, T, "progress", "40%", &in, &e), &e);
	TK_OK(tix_input_field(&ctx, T, "kind", "milestone", &in, &e), &e);
	TK_OK(tix_input_field(&ctx, T, "calendar", chex, &in, &e), &e);
	TK_CHECK(tix_oid_eq(&in.ops.v[6].u.set.v.id, &cal));
	snprintf(buf, sizeof(buf), "+%s:SS-2d", ahex);
	TK_OK(tix_input_field(&ctx, T, "dep", buf, &in, &e), &e);
	TK_CHECK(in.ops.v[7].kind == TIX_OP_DEP_ADD && in.ops.v[7].u.dep_add.kind == TIX_DEP_SS);
	TK_CHECK(in.ops.v[7].u.dep_add.lag == -2 && tix_oid_eq(&in.ops.v[7].u.dep_add.pred, &a));
	snprintf(buf, sizeof(buf), "-%s", ahex);
	TK_OK(tix_input_field(&ctx, T, "dep", buf, &in, &e), &e);
	TK_CHECK(in.ops.v[8].kind == TIX_OP_DEP_REMOVE);
	TK_OK(tix_input_field(&ctx, T, "parent", ahex, &in, &e), &e);
	TK_CHECK(in.parent_set && tix_oid_eq(&in.parent, &a));
	TK_OK(tix_input_field(&ctx, T, "rank", "last", &in, &e), &e);
	size_t before = in.ops.n;
	TK_OK(tix_input_finish(&ctx, &in, &b, NULL, &e), &e);
	TK_CHECK(in.ops.n == before + 1 && in.ops.v[before].u.set.field == TIX_F_RANK);

	/* malformed text: INVALID, syntax flag, "<field>: ..." messages (the CLI adds "--") */
	struct {
		const char *field, *text, *msg;
	} bad[] = {
		{"start", "2026-02-30", "start: '2026-02-30' is not a date"},
		{"duration", "3x", "duration: '3x' should look like 3d (or none)"},
		{"effort", "3d", "effort: '3d' should look like 3h"},
		{"progress", "101", "progress: a percentage 0-100"},
		{"kind", "epic", "kind: use task or milestone"},
		{"mode", "x", "mode: use auto or manual"},
		{"rank", "middle", "rank: use first, last"},
		{"dep", "x", "dep: use +ID"},
		{"dep", "+abcdef12:XX", "dep: kind must be"},
		{"dep", "+abcdef12:FS2d", "dep: lag must look like"},
		{"dep", "+abcdef12:FS+99999d", "at most 10000 days"},
		{"dep", "-abcdef12:FS", "dep: -ID takes no kind or lag"},
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		tix_error er = TIX_ERROR_INIT;
		TK_CHECK(tix_input_field(&ctx, T, bad[i].field, bad[i].text, &in, &er) == TIX_ERR_INVALID);
		TK_CHECK(in.syntax);
		if (!strstr(er.msg, bad[i].msg))
			tk_fail(__FILE__, __LINE__, "'%s' for %s=%s, want '%s'", er.msg, bad[i].field, bad[i].text, bad[i].msg);
	}
	/* well-formed id that doesn't resolve: core's status, not a syntax error */
	TK_CHECK(tix_input_field(&ctx, T, "parent", "abcdef12", &in, &e) == TIX_ERR_NOT_FOUND && !in.syntax);
	TK_CHECK(tix_input_field(&ctx, T, "dep", "+abcdef12", &in, &e) == TIX_ERR_NOT_FOUND && !in.syntax);
	TK_CHECK(tix_input_field(&ctx, T, "calendar", ahex, &in, &e) == TIX_ERR_NOT_FOUND); /* wrong type */
	tix_input_free(&in);

	/* resources and calendars */
	const tix_entity_type R = TIX_ENTITY_RESOURCE, C = TIX_ENTITY_CALENDAR;
	TK_OK(tix_input_field(&ctx, R, "capacity", "300%", &in, &e), &e);
	TK_OK(tix_input_field(&ctx, R, "identity", "me", &in, &e), &e);
	TK_CHECK(tix_oid_eq(&in.ops.v[1].u.set.v.id, &me));
	TK_OK(tix_input_field(&ctx, R, "rate", "95.50 eur", &in, &e), &e);
	TK_CHECK(in.ops.v[2].u.set.v.i == 9550 && !strcmp(in.ops.v[3].u.set.v.s, "EUR"));
	TK_OK(tix_input_field(&ctx, R, "active", "no", &in, &e), &e);
	TK_OK(tix_input_field(&ctx, R, "kind", "equipment", &in, &e), &e);
	TK_CHECK(tix_input_field(&ctx, R, "capacity", "0%", &in, &e) == TIX_ERR_INVALID && strstr(e.msg, "capacity:"));
	TK_CHECK(tix_input_field(&ctx, R, "rate", "95", &in, &e) == TIX_ERR_INVALID);
	TK_CHECK(tix_input_field(&ctx, R, "rate", "95 E1R", &in, &e) == TIX_ERR_INVALID);
	TK_CHECK(tix_input_field(&ctx, R, "name", "", &in, &e) == TIX_ERR_INVALID);
	TK_CHECK(tix_input_field(&ctx, R, "active", "maybe", &in, &e) == TIX_ERR_INVALID);
	tix_input_free(&in);
	TK_OK(tix_input_field(&ctx, C, "weekdays", "mon-sat", &in, &e), &e);
	TK_OK(tix_input_field(&ctx, C, "hours", "7h", &in, &e), &e);
	TK_OK(tix_input_field(&ctx, C, "off", "2026-12-24..2026-12-26=Holidays", &in, &e), &e);
	TK_OK(tix_input_field(&ctx, C, "on", "2026-12-27", &in, &e), &e);
	TK_CHECK(in.ops.v[2].kind == TIX_OP_EXCEPTION_ADD && !in.ops.v[2].u.exception_add.working);
	TK_CHECK(in.ops.v[3].u.exception_add.working && !strcmp(in.ops.v[2].u.exception_add.note, "Holidays"));
	TK_OK(tix_input_field(&ctx, C, "clear", "2026-12-27", &in, &e), &e);
	TK_CHECK(tix_input_field(&ctx, C, "hours", "25", &in, &e) == TIX_ERR_INVALID && strstr(e.msg, "hours:"));
	TK_CHECK(tix_input_field(&ctx, C, "off", "soon", &in, &e) == TIX_ERR_INVALID && strstr(e.msg, "off: FROM"));
	tix_input_free(&in);

	/* assignments */
	tix_op op;
	bool syntax;
	snprintf(buf, sizeof(buf), "%s:50%%", rhex);
	TK_OK(tix_input_assignment(&ctx, buf, "8h", &op, &syntax, &e), &e);
	TK_CHECK(op.kind == TIX_OP_ASSIGN && op.u.assign.units == 50 && op.u.assign.effort == 8);
	tix_op_free(&op);
	TK_OK(tix_input_assignment(&ctx, rhex, NULL, &op, &syntax, &e), &e);
	TK_CHECK(op.u.assign.units == 100 && op.u.assign.effort == -1);
	tix_op_free(&op);
	snprintf(buf, sizeof(buf), "%s:abc", rhex);
	TK_CHECK(tix_input_assignment(&ctx, buf, NULL, &op, &syntax, &e) == TIX_ERR_INVALID && syntax);
	snprintf(buf, sizeof(buf), "%s:0%%", rhex);
	TK_CHECK(tix_input_assignment(&ctx, buf, NULL, &op, &syntax, &e) == TIX_ERR_INVALID && strstr(e.msg, "at least"));
	TK_CHECK(tix_input_assignment(&ctx, rhex, "8d", &op, &syntax, &e) == TIX_ERR_INVALID && syntax);
	TK_CHECK(tix_input_assignment(&ctx, "abcdef12", NULL, &op, &syntax, &e) == TIX_ERR_NOT_FOUND && !syntax);

	tix_repo_close(r);
	tix_store_destroy(s);
}

int main(void)
{
	test_grammar();
	test_fields();
	return tk_finish();
}

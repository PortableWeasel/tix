/* Op-pack codec: round trips, canonical form, rejection of malformed input. */
#include "tix/model.h"
#include "tix/testkit.h"

#include <stdlib.h>

#define A40 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define B40 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define HDR "{\"tix\":1,\"type\":\"ticket\",\"author\":\"" A40 "\",\"lamport\":7,\"time\":1700000000}\n"
#define IHDR "{\"tix\":1,\"type\":\"identity\",\"author\":null,\"lamport\":1,\"time\":5}\n"

static tix_oid oid_of(const char *hex)
{
	tix_oid o;
	TIX_REQUIRE(tix_oid_from_hex(&o, hex, strlen(hex)));
	return o;
}

static void test_round_trip(void)
{
	tix_error e = TIX_ERROR_INIT;
	tix_strs labels = {0}, rm = {0};
	tix_strs_add(&labels, "ui");
	tix_strs_add(&labels, "bug");
	tix_strs_add(&rm, "wontfix");
	tix_opid target = {oid_of(B40), 3};
	tix_op ops[] = {
		tix_op_ticket_create("0123456789abcdef0123456789abcdef", "Crash on \"save\"", "line1\nline2\t\x01é", &labels),
		tix_op_text(TIX_OP_SET_TITLE, "Crash on save"),
		tix_op_text(TIX_OP_SET_BODY, ""),
		tix_op_set_status(TIX_TICKET_IN_PROGRESS),
		tix_op_label(&labels, &rm),
		tix_op_text(TIX_OP_COMMENT, "looking 👀"),
		tix_op_edit_comment(&target, "edited"),
	};
	size_t n = sizeof(ops) / sizeof(*ops);
	tix_pack p = {TIX_ENTITY_TICKET, oid_of(A40), 7, 1700000000, ops, n};
	tix_buf b = TIX_BUF_INIT, b2 = TIX_BUF_INIT;

	/* body with \x01 is a control char: multiline text allows only \n and \t */
	TK_STATUS(tix_pack_encode(&p, &b, &e), TIX_ERR_INVALID, &e);
	TK_CHECK(b.len == 0);
	tix_op_free(&ops[0]);
	ops[0] = tix_op_ticket_create("0123456789abcdef0123456789abcdef", "Crash on \"save\"", "line1\nline2\té", &labels);

	TK_OK(tix_pack_encode(&p, &b, &e), &e);
	const char *want = HDR
		"{\"op\":\"create\",\"nonce\":\"0123456789abcdef0123456789abcdef\",\"title\":\"Crash on \\\"save\\\"\",\"body\":\"line1\\nline2\\té\",\"labels\":[\"bug\",\"ui\"]}\n"
		"{\"op\":\"set-title\",\"title\":\"Crash on save\"}\n"
		"{\"op\":\"set-body\",\"body\":\"\"}\n"
		"{\"op\":\"set-status\",\"status\":\"in-progress\"}\n"
		"{\"op\":\"label\",\"add\":[\"bug\",\"ui\"],\"remove\":[\"wontfix\"]}\n"
		"{\"op\":\"comment\",\"body\":\"looking 👀\"}\n"
		"{\"op\":\"edit-comment\",\"target\":\"" B40 ":3\",\"body\":\"edited\"}\n";
	TK_STR_EQ(tix_buf_cstr(&b), want);

	tix_pack d;
	TK_OK(tix_pack_decode(b.data, b.len, TIX_HASH_SHA1, &d, &e), &e);
	TK_CHECK(d.type == TIX_ENTITY_TICKET && d.lamport == 7 && d.time == 1700000000 && d.nops == n);
	TK_CHECK(tix_oid_eq(&d.author, &p.author));
	if (d.nops == n) {
		TK_STR_EQ(d.ops[0].u.ticket_create.body, "line1\nline2\té");
		TK_CHECK(d.ops[3].u.set_status.status == TIX_TICKET_IN_PROGRESS);
		TK_CHECK(d.ops[6].u.edit_comment.target.line == 3);
	}
	TK_OK(tix_pack_encode(&d, &b2, &e), &e);
	TK_STR_EQ(tix_buf_cstr(&b2), want);
	tix_pack_free(&d);

	for (size_t i = 0; i < n; i++)
		tix_op_free(&ops[i]);
	tix_strs_free(&labels);
	tix_strs_free(&rm);
	tix_buf_free(&b);
	tix_buf_free(&b2);
}

static void test_identity_and_unknown(void)
{
	tix_error e = TIX_ERROR_INIT;
	const char *pack = IHDR
		"{\"op\":\"create\",\"nonce\":\"0123456789abcdef0123456789abcdef\",\"name\":\"Ada\",\"email\":\"ada@example.org\"}\n"
		"{\"op\":\"key-add\",\"kind\":\"ssh\",\"fingerprint\":\"SHA256:abc\",\"pubkey\":\"ssh-ed25519 AAAA ada\"}\n"
		"{\"op\":\"future-op\", \"x\": [1, 2.5]}\n"
		"{\"op\":\"set-title\",\"title\":\"not an identity op\"}\n";
	tix_pack d;
	TK_OK(tix_pack_decode(pack, strlen(pack), TIX_HASH_SHA1, &d, &e), &e);
	TK_CHECK(d.type == TIX_ENTITY_IDENTITY && tix_oid_is_null(&d.author) && d.nops == 4);
	if (d.nops == 4) {
		TK_STR_EQ(d.ops[0].u.identity_create.name, "Ada");
		TK_STR_EQ(d.ops[1].u.key_add.fingerprint, "SHA256:abc");
		TK_CHECK(d.ops[2].kind == TIX_OP_UNKNOWN);
		TK_STR_EQ(d.ops[2].u.unknown.name, "future-op");
		TK_STR_EQ(d.ops[2].u.unknown.raw, "{\"op\":\"future-op\", \"x\": [1, 2.5]}");
		TK_CHECK(d.ops[3].kind == TIX_OP_UNKNOWN); /* ticket op name inside an identity */
	}
	tix_buf b = TIX_BUF_INIT;
	TK_STATUS(tix_pack_encode(&d, &b, &e), TIX_ERR_INVALID, &e); /* unknown ops are never written */
	tix_pack_free(&d);
	tix_buf_free(&b);
}

typedef struct bad_case {
	const char *what;
	const char *pack;
	tix_status want;
} bad_case;

#define TIX_ERR_UNKNOWN_OK ((tix_status)-1) /* test marker: decodes, as an unknown op */

static void test_rejects(void)
{
	static const bad_case cases[] = {
		{"empty", "", TIX_ERR_INVALID},
		{"no final newline", HDR "{\"op\":\"comment\",\"body\":\"x\"}", TIX_ERR_INVALID},
		{"empty line", HDR "\n{\"op\":\"comment\",\"body\":\"x\"}\n", TIX_ERR_INVALID},
		{"header whitespace", "{\"tix\": 1,\"type\":\"ticket\",\"author\":\"" A40 "\",\"lamport\":7,\"time\":1}\n", TIX_ERR_INVALID},
		{"header key order", "{\"type\":\"ticket\",\"tix\":1,\"author\":\"" A40 "\",\"lamport\":7,\"time\":1}\n", TIX_ERR_INVALID},
		{"header extra key", "{\"tix\":1,\"type\":\"ticket\",\"author\":\"" A40 "\",\"lamport\":7,\"time\":1,\"x\":1}\n", TIX_ERR_INVALID},
		{"lamport 0", "{\"tix\":1,\"type\":\"ticket\",\"author\":\"" A40 "\",\"lamport\":0,\"time\":1}\n", TIX_ERR_INVALID},
		{"lamport float", "{\"tix\":1,\"type\":\"ticket\",\"author\":\"" A40 "\",\"lamport\":7.0,\"time\":1}\n", TIX_ERR_INVALID},
		{"lamport exp", "{\"tix\":1,\"type\":\"ticket\",\"author\":\"" A40 "\",\"lamport\":7e0,\"time\":1}\n", TIX_ERR_INVALID},
		{"negative time", "{\"tix\":1,\"type\":\"ticket\",\"author\":\"" A40 "\",\"lamport\":7,\"time\":-1}\n", TIX_ERR_INVALID},
		{"future version", "{\"tix\":2,\"type\":\"ticket\",\"author\":\"" A40 "\",\"lamport\":7,\"time\":1}\n", TIX_ERR_UNSUPPORTED},
		{"unknown type", "{\"tix\":1,\"type\":\"widget\",\"author\":\"" A40 "\",\"lamport\":7,\"time\":1}\n", TIX_ERR_INVALID},
		{"ticket without author", "{\"tix\":1,\"type\":\"ticket\",\"author\":null,\"lamport\":7,\"time\":1}\n", TIX_ERR_INVALID},
		{"uppercase author", "{\"tix\":1,\"type\":\"ticket\",\"author\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"lamport\":7,\"time\":1}\n", TIX_ERR_INVALID},
		{"sha256 author in sha1 repo", "{\"tix\":1,\"type\":\"ticket\",\"author\":\"" A40 A40 "aaaaaaaaaaaaaaaaaaaaaaaa\",\"lamport\":7,\"time\":1}\n", TIX_ERR_INVALID},
		{"header is array", "[1]\n", TIX_ERR_INVALID},
		{"op whitespace", HDR "{\"op\":\"comment\", \"body\":\"x\"}\n", TIX_ERR_INVALID},
		{"op missing field", HDR "{\"op\":\"comment\"}\n", TIX_ERR_INVALID},
		{"op wrong field type", HDR "{\"op\":\"comment\",\"body\":1}\n", TIX_ERR_INVALID},
		{"op extra field", HDR "{\"op\":\"comment\",\"body\":\"x\",\"y\":1}\n", TIX_ERR_INVALID},
		{"op duplicate key", HDR "{\"op\":\"set-title\",\"title\":\"a\",\"title\":\"b\"}\n", TIX_ERR_INVALID},
		{"op trailing garbage", HDR "{\"op\":\"comment\",\"body\":\"x\"} x\n", TIX_ERR_INVALID},
		{"op not first", HDR "{\"body\":\"x\",\"op\":\"comment\"}\n", TIX_ERR_INVALID},
		{"empty op name", HDR "{\"op\":\"\"}\n", TIX_ERR_INVALID},
		{"escaped plain char", HDR "{\"op\":\"comment\",\"body\":\"\\u0041\"}\n", TIX_ERR_INVALID},
		{"escaped NUL", HDR "{\"op\":\"comment\",\"body\":\"a\\u0000b\"}\n", TIX_ERR_INVALID},
		{"invalid utf-8", HDR "{\"op\":\"comment\",\"body\":\"\xc3\x28\"}\n", TIX_ERR_INVALID},
		{"empty comment", HDR "{\"op\":\"comment\",\"body\":\"\"}\n", TIX_ERR_INVALID},
		{"empty title", HDR "{\"op\":\"set-title\",\"title\":\"\"}\n", TIX_ERR_INVALID},
		{"title newline", HDR "{\"op\":\"set-title\",\"title\":\"a\\nb\"}\n", TIX_ERR_INVALID},
		{"bad status", HDR "{\"op\":\"set-status\",\"status\":\"wip\"}\n", TIX_ERR_INVALID},
		{"labels unsorted", HDR "{\"op\":\"label\",\"add\":[\"b\",\"a\"],\"remove\":[]}\n", TIX_ERR_INVALID},
		{"labels duplicate", HDR "{\"op\":\"label\",\"add\":[\"a\",\"a\"],\"remove\":[]}\n", TIX_ERR_INVALID},
		{"label with space", HDR "{\"op\":\"label\",\"add\":[\"a b\"],\"remove\":[]}\n", TIX_ERR_INVALID},
		{"label add+remove", HDR "{\"op\":\"label\",\"add\":[\"a\"],\"remove\":[\"a\"]}\n", TIX_ERR_INVALID},
		{"label empty op", HDR "{\"op\":\"label\",\"add\":[],\"remove\":[]}\n", TIX_ERR_INVALID},
		{"edit target line 1", HDR "{\"op\":\"edit-comment\",\"target\":\"" B40 ":1\",\"body\":\"x\"}\n", TIX_ERR_INVALID},
		{"edit target leading zero", HDR "{\"op\":\"edit-comment\",\"target\":\"" B40 ":03\",\"body\":\"x\"}\n", TIX_ERR_INVALID},
		{"identity create missing name", IHDR "{\"op\":\"create\",\"title\":\"x\",\"body\":\"\",\"labels\":[]}\n", TIX_ERR_INVALID},
		{"identity create without nonce", IHDR "{\"op\":\"create\",\"name\":\"n\",\"email\":\"e\"}\n", TIX_ERR_INVALID},
		{"short nonce", IHDR "{\"op\":\"create\",\"nonce\":\"abc\",\"name\":\"n\",\"email\":\"e\"}\n", TIX_ERR_INVALID},
		{"key kind", IHDR "{\"op\":\"key-add\",\"kind\":\"rsa\",\"fingerprint\":\"f\",\"pubkey\":\"k\"}\n", TIX_ERR_INVALID},
		{"email with space", IHDR "{\"op\":\"set-email\",\"email\":\"a b\"}\n", TIX_ERR_INVALID},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		tix_error e = TIX_ERROR_INIT;
		tix_pack d;
		tix_status st = tix_pack_decode(cases[i].pack, strlen(cases[i].pack), TIX_HASH_SHA1, &d, &e);
		tk_checks++;
		if (st != cases[i].want)
			tk_fail(__FILE__, __LINE__, "case '%s': got %s (want %s) %s", cases[i].what,
				tix_status_name(st), tix_status_name(cases[i].want), e.msg);
		TK_CHECK(d.nops == 0 && d.ops == NULL);
		if (!st)
			tix_pack_free(&d);
	}
}

static void test_encode_limits(void)
{
	tix_error e = TIX_ERROR_INIT;
	char title[TIX_TITLE_MAX + 2];
	memset(title, 'x', sizeof(title) - 1);
	title[sizeof(title) - 1] = 0;
	tix_op op = tix_op_text(TIX_OP_SET_TITLE, title);
	tix_pack p = {TIX_ENTITY_TICKET, oid_of(A40), 1, 0, &op, 1};
	tix_buf b = TIX_BUF_INIT;
	TK_STATUS(tix_pack_encode(&p, &b, &e), TIX_ERR_INVALID, &e);
	tix_op_free(&op);

	op = tix_op_text(TIX_OP_SET_NAME, "Ada");
	TK_STATUS(tix_pack_encode(&p, &b, &e), TIX_ERR_INVALID, &e); /* identity op in a ticket */
	p.type = TIX_ENTITY_IDENTITY;
	TK_OK(tix_pack_encode(&p, &b, &e), &e);
	tix_op_free(&op);

	p.type = TIX_ENTITY_TICKET;
	p.nops = 0;
	memset(&p.author, 0, sizeof(p.author));
	TK_STATUS(tix_pack_encode(&p, &b, &e), TIX_ERR_INVALID, &e); /* ticket needs an author */
	tix_buf_free(&b);
}

#define RHDR "{\"tix\":1,\"type\":\"resource\",\"author\":\"" A40 "\",\"lamport\":3,\"time\":9}\n"
#define CHDR "{\"tix\":1,\"type\":\"calendar\",\"author\":\"" A40 "\",\"lamport\":3,\"time\":9}\n"

static void round_trip(tix_entity_type type, tix_op *ops, size_t n, const char *want)
{
	tix_error e = TIX_ERROR_INIT;
	tix_pack p = {type, oid_of(A40), 3, 9, ops, n}, d;
	tix_buf b = TIX_BUF_INIT, b2 = TIX_BUF_INIT;
	TK_OK(tix_pack_encode(&p, &b, &e), &e);
	TK_STR_EQ(tix_buf_cstr(&b), want);
	TK_OK(tix_pack_decode(b.data, b.len, TIX_HASH_SHA1, &d, &e), &e);
	TK_CHECK(d.nops == n);
	TK_OK(tix_pack_encode(&d, &b2, &e), &e);
	TK_STR_EQ(tix_buf_cstr(&b2), want);
	tix_pack_free(&d);
	tix_buf_free(&b);
	tix_buf_free(&b2);
	for (size_t i = 0; i < n; i++)
		tix_op_free(&ops[i]);
}

static void test_planning_ops(void)
{
	tix_date d1, d2;
	TIX_REQUIRE(tix_date_parse("2026-11-09", &d1) && tix_date_parse("2026-12-24", &d2));
	tix_oid b = oid_of(B40);
	tix_op t[] = {
		tix_op_set(TIX_F_PARENT, tix_value_id(&b)),
		tix_op_set(TIX_F_PARENT, tix_value_null()),
		tix_op_set(TIX_F_RANK, tix_value_str("i")),
		tix_op_set(TIX_F_KIND, tix_value_int(TIX_V_ENUM, TIX_KIND_MILESTONE)),
		tix_op_set(TIX_F_MODE, tix_value_int(TIX_V_ENUM, TIX_MODE_MANUAL)),
		tix_op_set(TIX_F_START, tix_value_int(TIX_V_DATE, d1)),
		tix_op_set(TIX_F_DURATION, tix_value_int(TIX_V_INT, 3)),
		tix_op_set(TIX_F_EFFORT, tix_value_null()),
		tix_op_set(TIX_F_PROGRESS, tix_value_int(TIX_V_INT, 40)),
		tix_op_set(TIX_F_ACTUAL_FINISH, tix_value_int(TIX_V_DATE, d2)),
		tix_op_dep_add(&b, TIX_DEP_SS, -2),
		tix_op_dep_remove(&b),
		tix_op_assign(&b, 50, 16),
		tix_op_assign(&b, 100, -1),
		tix_op_unassign(&b),
	};
	round_trip(TIX_ENTITY_TICKET, t, sizeof(t) / sizeof(*t),
		"{\"tix\":1,\"type\":\"ticket\",\"author\":\"" A40 "\",\"lamport\":3,\"time\":9}\n"
		"{\"op\":\"set-parent\",\"parent\":\"" B40 "\"}\n"
		"{\"op\":\"set-parent\",\"parent\":null}\n"
		"{\"op\":\"set-rank\",\"rank\":\"i\"}\n"
		"{\"op\":\"set-kind\",\"kind\":\"milestone\"}\n"
		"{\"op\":\"set-mode\",\"mode\":\"manual\"}\n"
		"{\"op\":\"set-start\",\"start\":\"2026-11-09\"}\n"
		"{\"op\":\"set-duration\",\"duration\":3}\n"
		"{\"op\":\"set-effort\",\"effort\":null}\n"
		"{\"op\":\"set-progress\",\"progress\":40}\n"
		"{\"op\":\"set-actual-finish\",\"actual_finish\":\"2026-12-24\"}\n"
		"{\"op\":\"dep-add\",\"pred\":\"" B40 "\",\"kind\":\"SS\",\"lag\":-2}\n"
		"{\"op\":\"dep-remove\",\"pred\":\"" B40 "\"}\n"
		"{\"op\":\"assign\",\"resource\":\"" B40 "\",\"units\":50,\"effort\":16}\n"
		"{\"op\":\"assign\",\"resource\":\"" B40 "\",\"units\":100,\"effort\":null}\n"
		"{\"op\":\"unassign\",\"resource\":\"" B40 "\"}\n");

	tix_op r[] = {
		tix_op_resource_create("0123456789abcdef0123456789abcdef", "Backend dev", TIX_RES_ROLE),
		tix_op_set(TIX_F_RES_NAME, tix_value_str("Backend devs")),
		tix_op_set(TIX_F_CAPACITY, tix_value_int(TIX_V_INT, 300)),
		tix_op_set(TIX_F_RES_IDENTITY, tix_value_id(&b)),
		tix_op_set(TIX_F_RATE, tix_value_int(TIX_V_INT, 9500)),
		tix_op_set(TIX_F_CURRENCY, tix_value_str("EUR")),
		tix_op_set(TIX_F_ACTIVE, tix_value_int(TIX_V_BOOL, 0)),
	};
	round_trip(TIX_ENTITY_RESOURCE, r, sizeof(r) / sizeof(*r), RHDR
		"{\"op\":\"create\",\"nonce\":\"0123456789abcdef0123456789abcdef\",\"name\":\"Backend dev\",\"kind\":\"role\"}\n"
		"{\"op\":\"set-name\",\"name\":\"Backend devs\"}\n"
		"{\"op\":\"set-capacity\",\"capacity\":300}\n"
		"{\"op\":\"set-identity\",\"identity\":\"" B40 "\"}\n"
		"{\"op\":\"set-rate\",\"rate\":9500}\n"
		"{\"op\":\"set-currency\",\"currency\":\"EUR\"}\n"
		"{\"op\":\"set-active\",\"active\":false}\n");

	tix_op c[] = {
		tix_op_calendar_create("0123456789abcdef0123456789abcdef", "Site crew"),
		tix_op_set(TIX_F_WEEKDAYS, tix_value_int(TIX_V_INT, 0x3f)),
		tix_op_set(TIX_F_HOURS, tix_value_int(TIX_V_INT, 10)),
		tix_op_exception_add(d2, d2 + 2, false, "Holidays"),
		tix_op_exception_remove(d2, d2 + 2),
	};
	round_trip(TIX_ENTITY_CALENDAR, c, sizeof(c) / sizeof(*c), CHDR
		"{\"op\":\"create\",\"nonce\":\"0123456789abcdef0123456789abcdef\",\"name\":\"Site crew\"}\n"
		"{\"op\":\"set-weekdays\",\"weekdays\":63}\n"
		"{\"op\":\"set-hours\",\"hours\":10}\n"
		"{\"op\":\"exception-add\",\"from\":\"2026-12-24\",\"to\":\"2026-12-26\",\"working\":false,\"note\":\"Holidays\"}\n"
		"{\"op\":\"exception-remove\",\"from\":\"2026-12-24\",\"to\":\"2026-12-26\"}\n");

	/* encode-time validation */
	tix_error e = TIX_ERROR_INIT;
	tix_buf out = TIX_BUF_INIT;
	tix_op bad[] = {
		tix_op_set(TIX_F_PROGRESS, tix_value_int(TIX_V_INT, 101)),
		tix_op_set(TIX_F_PROGRESS, tix_value_null()),
		tix_op_set(TIX_F_RANK, tix_value_str("a0")),
		tix_op_set(TIX_F_START, tix_value_str("2026-01-01")),
		tix_op_set(TIX_F_CAPACITY, tix_value_int(TIX_V_INT, 100)), /* resource field on a ticket */
		tix_op_dep_add(&b, TIX_DEP_FS, TIX_LAG_MAX + 1),
		tix_op_assign(&b, 0, -1),
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		tix_pack p = {TIX_ENTITY_TICKET, oid_of(A40), 1, 0, &bad[i], 1};
		TK_STATUS(tix_pack_encode(&p, &out, &e), TIX_ERR_INVALID, &e);
		tix_op_free(&bad[i]);
	}
	tix_oid b256;
	TIX_REQUIRE(tix_oid_from_hex(&b256, B40 B40 "bbbbbbbbbbbbbbbbbbbbbbbb", 64));
	tix_op mixed = tix_op_dep_add(&b256, TIX_DEP_FS, 0);
	tix_pack p = {TIX_ENTITY_TICKET, oid_of(A40), 1, 0, &mixed, 1};
	TK_STATUS(tix_pack_encode(&p, &out, &e), TIX_ERR_INVALID, &e); /* sha256 id in a sha1 pack */
	tix_op_free(&mixed);
	TK_CHECK(out.len == 0);
	tix_buf_free(&out);
}

static void test_planning_rejects(void)
{
	static const bad_case cases[] = {
		{"date format", HDR "{\"op\":\"set-start\",\"start\":\"2026-1-09\"}\n", TIX_ERR_INVALID},
		{"not a day", HDR "{\"op\":\"set-start\",\"start\":\"2026-02-30\"}\n", TIX_ERR_INVALID},
		{"date as number", HDR "{\"op\":\"set-start\",\"start\":20260109}\n", TIX_ERR_INVALID},
		{"missing value", HDR "{\"op\":\"set-start\"}\n", TIX_ERR_INVALID},
		{"wrong key", HDR "{\"op\":\"set-start\",\"finish\":null}\n", TIX_ERR_INVALID},
		{"progress null", HDR "{\"op\":\"set-progress\",\"progress\":null}\n", TIX_ERR_INVALID},
		{"progress range", HDR "{\"op\":\"set-progress\",\"progress\":101}\n", TIX_ERR_INVALID},
		{"progress float", HDR "{\"op\":\"set-progress\",\"progress\":4.5}\n", TIX_ERR_INVALID},
		{"duration negative", HDR "{\"op\":\"set-duration\",\"duration\":-1}\n", TIX_ERR_INVALID},
		{"duration huge", HDR "{\"op\":\"set-duration\",\"duration\":1e300}\n", TIX_ERR_INVALID},
		{"kind", HDR "{\"op\":\"set-kind\",\"kind\":\"summary\"}\n", TIX_ERR_INVALID},
		{"rank trailing 0", HDR "{\"op\":\"set-rank\",\"rank\":\"a0\"}\n", TIX_ERR_INVALID},
		{"rank uppercase", HDR "{\"op\":\"set-rank\",\"rank\":\"A\"}\n", TIX_ERR_INVALID},
		{"parent uppercase", HDR "{\"op\":\"set-parent\",\"parent\":\"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\"}\n", TIX_ERR_INVALID},
		{"parent short", HDR "{\"op\":\"set-parent\",\"parent\":\"bbbb\"}\n", TIX_ERR_INVALID},
		{"dep kind", HDR "{\"op\":\"dep-add\",\"pred\":\"" B40 "\",\"kind\":\"fs\",\"lag\":0}\n", TIX_ERR_INVALID},
		{"dep lag string", HDR "{\"op\":\"dep-add\",\"pred\":\"" B40 "\",\"kind\":\"FS\",\"lag\":\"2d\"}\n", TIX_ERR_INVALID},
		{"dep key order", HDR "{\"op\":\"dep-add\",\"kind\":\"FS\",\"pred\":\"" B40 "\",\"lag\":0}\n", TIX_ERR_INVALID},
		{"assign units 0", HDR "{\"op\":\"assign\",\"resource\":\"" B40 "\",\"units\":0,\"effort\":null}\n", TIX_ERR_INVALID},
		{"assign no effort", HDR "{\"op\":\"assign\",\"resource\":\"" B40 "\",\"units\":10}\n", TIX_ERR_INVALID},
		{"resource kind", RHDR "{\"op\":\"create\",\"nonce\":\"0123456789abcdef0123456789abcdef\",\"name\":\"x\",\"kind\":\"robot\"}\n", TIX_ERR_INVALID},
		{"capacity 0", RHDR "{\"op\":\"set-capacity\",\"capacity\":0}\n", TIX_ERR_INVALID},
		{"currency lower", RHDR "{\"op\":\"set-currency\",\"currency\":\"eur\"}\n", TIX_ERR_INVALID},
		{"active as int", RHDR "{\"op\":\"set-active\",\"active\":1}\n", TIX_ERR_INVALID},
		{"resource without author", "{\"tix\":1,\"type\":\"resource\",\"author\":null,\"lamport\":3,\"time\":9}\n", TIX_ERR_INVALID},
		{"ticket op on calendar", CHDR "{\"op\":\"set-start\",\"start\":null}\n", TIX_ERR_UNKNOWN_OK},
		{"weekdays range", CHDR "{\"op\":\"set-weekdays\",\"weekdays\":128}\n", TIX_ERR_INVALID},
		{"hours 0", CHDR "{\"op\":\"set-hours\",\"hours\":0}\n", TIX_ERR_INVALID},
		{"exception reversed", CHDR "{\"op\":\"exception-add\",\"from\":\"2026-12-26\",\"to\":\"2026-12-24\",\"working\":false,\"note\":\"\"}\n", TIX_ERR_INVALID},
		{"exception too long", CHDR "{\"op\":\"exception-add\",\"from\":\"2000-01-01\",\"to\":\"2026-12-24\",\"working\":false,\"note\":\"\"}\n", TIX_ERR_INVALID},
		{"exception note newline", CHDR "{\"op\":\"exception-add\",\"from\":\"2026-12-24\",\"to\":\"2026-12-24\",\"working\":false,\"note\":\"a\\nb\"}\n", TIX_ERR_INVALID},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		tix_error e = TIX_ERROR_INIT;
		tix_pack d;
		tix_status st = tix_pack_decode(cases[i].pack, strlen(cases[i].pack), TIX_HASH_SHA1, &d, &e);
		tix_status want = cases[i].want == TIX_ERR_UNKNOWN_OK ? TIX_OK : cases[i].want;
		tk_checks++;
		if (st != want)
			tk_fail(__FILE__, __LINE__, "case '%s': got %s (want %s) %s", cases[i].what,
				tix_status_name(st), tix_status_name(want), e.msg);
		if (!st) { /* only acceptable as an unknown op, preserved verbatim */
			TK_CHECK(d.nops == 1 && d.ops[0].kind == TIX_OP_UNKNOWN);
			tix_pack_free(&d);
		}
	}
}

#define BHDR "{\"tix\":1,\"type\":\"baseline\",\"author\":\"" A40 "\",\"lamport\":3,\"time\":9}\n"
#define NONCE "\"nonce\":\"0123456789abcdef0123456789abcdef\""

static void test_baseline_ops(void)
{
	tix_date d1, d2;
	TIX_REQUIRE(tix_date_parse("2026-11-09", &d1) && tix_date_parse("2026-11-12", &d2));
	tix_oid b = oid_of(B40), none;
	memset(&none, 0, sizeof(none));
	tix_op ops[] = {
		tix_op_baseline_create("0123456789abcdef0123456789abcdef", "Plan of record", d1, &b),
		tix_op_text(TIX_OP_BASELINE_SET_NAME, "Q4 plan"),
		tix_op_baseline_task(&b, d1, d2, 4, 32, 304000, "EUR"),
		tix_op_baseline_task(&b, TIX_DATE_NONE, TIX_DATE_NONE, -1, -1, -1, NULL),
	};
	round_trip(TIX_ENTITY_BASELINE, ops, sizeof(ops) / sizeof(*ops), BHDR
		"{\"op\":\"create\"," NONCE ",\"name\":\"Plan of record\",\"taken\":\"2026-11-09\",\"root\":\"" B40 "\"}\n"
		"{\"op\":\"set-name\",\"name\":\"Q4 plan\"}\n"
		"{\"op\":\"task\",\"ticket\":\"" B40 "\",\"start\":\"2026-11-09\",\"finish\":\"2026-11-12\",\"duration\":4,"
		"\"effort\":32,\"cost\":304000,\"currency\":\"EUR\"}\n"
		"{\"op\":\"task\",\"ticket\":\"" B40 "\",\"start\":null,\"finish\":null,\"duration\":null,\"effort\":null,"
		"\"cost\":null,\"currency\":null}\n");
	tix_op whole[] = {tix_op_baseline_create("0123456789abcdef0123456789abcdef", "All", d1, &none)};
	round_trip(TIX_ENTITY_BASELINE, whole, 1, BHDR
		"{\"op\":\"create\"," NONCE ",\"name\":\"All\",\"taken\":\"2026-11-09\",\"root\":null}\n");

	static const bad_case cases[] = {
		{"no taken", BHDR "{\"op\":\"create\"," NONCE ",\"name\":\"x\",\"root\":null}\n", TIX_ERR_INVALID},
		{"empty name", BHDR "{\"op\":\"create\"," NONCE ",\"name\":\"\",\"taken\":\"2026-11-09\",\"root\":null}\n", TIX_ERR_INVALID},
		{"one date", BHDR "{\"op\":\"task\",\"ticket\":\"" B40 "\",\"start\":\"2026-11-09\",\"finish\":null,\"duration\":null,"
			     "\"effort\":null,\"cost\":null,\"currency\":null}\n", TIX_ERR_INVALID},
		{"reversed", BHDR "{\"op\":\"task\",\"ticket\":\"" B40 "\",\"start\":\"2026-11-12\",\"finish\":\"2026-11-09\","
			     "\"duration\":null,\"effort\":null,\"cost\":null,\"currency\":null}\n", TIX_ERR_INVALID},
		{"cost without currency", BHDR "{\"op\":\"task\",\"ticket\":\"" B40 "\",\"start\":null,\"finish\":null,"
			     "\"duration\":null,\"effort\":null,\"cost\":5,\"currency\":null}\n", TIX_ERR_INVALID},
		{"currency without cost", BHDR "{\"op\":\"task\",\"ticket\":\"" B40 "\",\"start\":null,\"finish\":null,"
			     "\"duration\":null,\"effort\":null,\"cost\":null,\"currency\":\"EUR\"}\n", TIX_ERR_INVALID},
		{"currency digits", BHDR "{\"op\":\"task\",\"ticket\":\"" B40 "\",\"start\":null,\"finish\":null,"
			     "\"duration\":null,\"effort\":null,\"cost\":5,\"currency\":\"E1\"}\n", TIX_ERR_INVALID},
		{"negative effort", BHDR "{\"op\":\"task\",\"ticket\":\"" B40 "\",\"start\":null,\"finish\":null,"
			     "\"duration\":null,\"effort\":-3,\"cost\":null,\"currency\":null}\n", TIX_ERR_INVALID},
		{"missing key", BHDR "{\"op\":\"task\",\"ticket\":\"" B40 "\",\"start\":null,\"finish\":null}\n", TIX_ERR_INVALID},
		{"ticket op on baseline", BHDR "{\"op\":\"set-title\",\"title\":\"x\"}\n", TIX_ERR_UNKNOWN_OK},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		tix_error e = TIX_ERROR_INIT;
		tix_pack d;
		tix_status st = tix_pack_decode(cases[i].pack, strlen(cases[i].pack), TIX_HASH_SHA1, &d, &e);
		tix_status want = cases[i].want == TIX_ERR_UNKNOWN_OK ? TIX_OK : cases[i].want;
		tk_checks++;
		if (st != want)
			tk_fail(__FILE__, __LINE__, "case '%s': got %s (want %s) %s", cases[i].what,
				tix_status_name(st), tix_status_name(want), e.msg);
		if (!st)
			tix_pack_free(&d);
	}
}

static void test_workflow_ops(void)
{
	tix_op t[] = {
		tix_op_set_status(TIX_TICKET_OPEN),
		tix_op_set_state(TIX_TICKET_OPEN, "ready"),
		tix_op_set_state(TIX_TICKET_DONE, NULL),
		tix_op_stage_add("needs-input", TIX_TICKET_IN_PROGRESS, true, 40),
		tix_op_stage_remove("needs-input", TIX_TICKET_IN_PROGRESS),
		tix_op_claim(1700001800),
		tix_op_release(),
		tix_op_set_autonomy(TIX_AUTO_AUTONOMOUS, 1700030000),
		tix_op_set_autonomy(TIX_AUTO_HALTED, -1),
	};
	round_trip(TIX_ENTITY_TICKET, t, sizeof(t) / sizeof(*t),
		"{\"tix\":1,\"type\":\"ticket\",\"author\":\"" A40 "\",\"lamport\":3,\"time\":9}\n"
		"{\"op\":\"set-status\",\"status\":\"open\"}\n"
		"{\"op\":\"set-state\",\"status\":\"open\",\"stage\":\"ready\"}\n"
		"{\"op\":\"set-state\",\"status\":\"done\",\"stage\":null}\n"
		"{\"op\":\"stage-add\",\"name\":\"needs-input\",\"phase\":\"in-progress\",\"human\":true,\"rank\":40}\n"
		"{\"op\":\"stage-remove\",\"name\":\"needs-input\",\"phase\":\"in-progress\"}\n"
		"{\"op\":\"claim\",\"until\":1700001800}\n"
		"{\"op\":\"release\"}\n"
		"{\"op\":\"set-autonomy\",\"mode\":\"autonomous\",\"until\":1700030000}\n"
		"{\"op\":\"set-autonomy\",\"mode\":\"halted\",\"until\":null}\n");

	static const bad_case cases[] = {
		{"stage uppercase", HDR "{\"op\":\"set-state\",\"status\":\"open\",\"stage\":\"Ready\"}\n", TIX_ERR_INVALID},
		{"stage empty", HDR "{\"op\":\"set-state\",\"status\":\"open\",\"stage\":\"\"}\n", TIX_ERR_INVALID},
		{"stage digit first", HDR "{\"op\":\"set-state\",\"status\":\"open\",\"stage\":\"1a\"}\n", TIX_ERR_INVALID},
		{"stage slash", HDR "{\"op\":\"set-state\",\"status\":\"open\",\"stage\":\"a/b\"}\n", TIX_ERR_INVALID},
		{"stage too long", HDR "{\"op\":\"set-state\",\"status\":\"open\",\"stage\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}\n", TIX_ERR_INVALID},
		{"stage missing", HDR "{\"op\":\"set-state\",\"status\":\"open\"}\n", TIX_ERR_INVALID},
		{"state bad status", HDR "{\"op\":\"set-state\",\"status\":\"wip\",\"stage\":null}\n", TIX_ERR_INVALID},
		{"stage-add bad phase", HDR "{\"op\":\"stage-add\",\"name\":\"x\",\"phase\":\"later\",\"human\":false,\"rank\":1}\n", TIX_ERR_INVALID},
		{"stage-add human not bool", HDR "{\"op\":\"stage-add\",\"name\":\"x\",\"phase\":\"open\",\"human\":1,\"rank\":1}\n", TIX_ERR_INVALID},
		{"stage-add negative rank", HDR "{\"op\":\"stage-add\",\"name\":\"x\",\"phase\":\"open\",\"human\":false,\"rank\":-1}\n", TIX_ERR_INVALID},
		{"stage-add key order", HDR "{\"op\":\"stage-add\",\"phase\":\"open\",\"name\":\"x\",\"human\":false,\"rank\":1}\n", TIX_ERR_INVALID},
		{"stage-remove bad name", HDR "{\"op\":\"stage-remove\",\"name\":\"X\",\"phase\":\"open\"}\n", TIX_ERR_INVALID},
		{"stage-remove no phase", HDR "{\"op\":\"stage-remove\",\"name\":\"x\"}\n", TIX_ERR_INVALID},
		{"claim negative", HDR "{\"op\":\"claim\",\"until\":-1}\n", TIX_ERR_INVALID},
		{"claim float", HDR "{\"op\":\"claim\",\"until\":1.5}\n", TIX_ERR_INVALID},
		{"claim missing until", HDR "{\"op\":\"claim\"}\n", TIX_ERR_INVALID},
		{"claim with identity", HDR "{\"op\":\"claim\",\"until\":5,\"identity\":\"" A40 "\"}\n", TIX_ERR_INVALID},
		{"release extra", HDR "{\"op\":\"release\",\"x\":1}\n", TIX_ERR_INVALID},
		{"autonomy bad mode", HDR "{\"op\":\"set-autonomy\",\"mode\":\"yolo\",\"until\":null}\n", TIX_ERR_INVALID},
		{"autonomy no until", HDR "{\"op\":\"set-autonomy\",\"mode\":\"halted\"}\n", TIX_ERR_INVALID},
		{"autonomy negative", HDR "{\"op\":\"set-autonomy\",\"mode\":\"halted\",\"until\":-1}\n", TIX_ERR_INVALID},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		tix_error e = TIX_ERROR_INIT;
		tix_pack d;
		tix_status st = tix_pack_decode(cases[i].pack, strlen(cases[i].pack), TIX_HASH_SHA1, &d, &e);
		tk_checks++;
		if (st != cases[i].want)
			tk_fail(__FILE__, __LINE__, "case '%s': got %s (want %s) %s", cases[i].what,
				tix_status_name(st), tix_status_name(cases[i].want), e.msg);
		if (!st)
			tix_pack_free(&d);
	}

	tix_opid target = {oid_of(B40), 4};
	tix_op tc[] = {
		tix_op_typed_comment("review", "Two findings.",
				     "{\"fields\":{\"verdict\":\"changes\"},\"items\":[{\"id\":\"f1\",\"text\":\"x\",\"state\":\"open\"}]}",
				     NULL),
		tix_op_typed_comment("answer", "", "{\"fields\":{\"q1\":\"yes\"}}", &target),
	};
	round_trip(TIX_ENTITY_TICKET, tc, 2,
		"{\"tix\":1,\"type\":\"ticket\",\"author\":\"" A40 "\",\"lamport\":3,\"time\":9}\n"
		"{\"op\":\"typed-comment\",\"kind\":\"review\",\"body\":\"Two findings.\",\"data\":"
		"\"{\\\"fields\\\":{\\\"verdict\\\":\\\"changes\\\"},\\\"items\\\":[{\\\"id\\\":\\\"f1\\\",\\\"text\\\":\\\"x\\\",\\\"state\\\":\\\"open\\\"}]}\","
		"\"reply-to\":null}\n"
		"{\"op\":\"typed-comment\",\"kind\":\"answer\",\"body\":\"\",\"data\":\"{\\\"fields\\\":{\\\"q1\\\":\\\"yes\\\"}}\","
		"\"reply-to\":\"" B40 ":4\"}\n");
	static const struct { const char *what, *data; bool ok; } notes[] = {
		{"empty object", "{}", true},
		{"unknown key kept", "{\"later\":[1,2]}", true},
		{"questions", "{\"questions\":[{\"id\":\"q1\",\"text\":\"Which DB?\"},{\"id\":\"q2\",\"text\":\"\"}]}", true},
		{"updates", "{\"updates\":[{\"ref\":\"" B40 ":3#f1\",\"state\":\"addressed\"}]}", true},
		{"array", "[]", false},
		{"two objects", "{}{}", false},
		{"field not string", "{\"fields\":{\"a\":1}}", false},
		{"field bad name", "{\"fields\":{\"A B\":\"x\"}}", false},
		{"item without state", "{\"items\":[{\"id\":\"f1\",\"text\":\"x\"}]}", false},
		{"item extra member", "{\"items\":[{\"id\":\"f1\",\"text\":\"x\",\"state\":\"open\",\"y\":1}]}", false},
		{"item duplicate id", "{\"items\":[{\"id\":\"f1\",\"text\":\"\",\"state\":\"open\"},{\"id\":\"f1\",\"text\":\"\",\"state\":\"open\"}]}", false},
		{"item bad state", "{\"items\":[{\"id\":\"f1\",\"text\":\"x\",\"state\":\"Open\"}]}", false},
		{"item bad id", "{\"items\":[{\"id\":\"-x\",\"text\":\"x\",\"state\":\"open\"}]}", false},
		{"question control char", "{\"questions\":[{\"id\":\"q\",\"text\":\"a\\u0001\"}]}", false},
		{"update bad ref", "{\"updates\":[{\"ref\":\"f1\",\"state\":\"done\"}]}", false},
		{"items not a list", "{\"items\":{}}", false},
	};
	for (size_t i = 0; i < sizeof(notes) / sizeof(*notes); i++) {
		tix_error ne = TIX_ERROR_INIT;
		tix_status st = tix_note_data_check(notes[i].data, &ne);
		tk_checks++;
		if ((st == TIX_OK) != notes[i].ok)
			tk_fail(__FILE__, __LINE__, "note '%s': got %s %s", notes[i].what, tix_status_name(st), ne.msg);
	}
	const char *badkind = HDR "{\"op\":\"typed-comment\",\"kind\":\"Review\",\"body\":\"\",\"data\":\"{}\",\"reply-to\":null}\n";
	tix_pack bk;
	tix_error bke = TIX_ERROR_INIT;
	TK_STATUS(tix_pack_decode(badkind, strlen(badkind), TIX_HASH_SHA1, &bk, &bke), TIX_ERR_INVALID, &bke);
	const char *baddata = HDR "{\"op\":\"typed-comment\",\"kind\":\"x\",\"body\":\"\",\"data\":\"[]\",\"reply-to\":null}\n";
	TK_STATUS(tix_pack_decode(baddata, strlen(baddata), TIX_HASH_SHA1, &bk, &bke), TIX_ERR_INVALID, &bke);

	tix_op id[] = {tix_op_set(TIX_F_ID_KIND, tix_value_int(TIX_V_ENUM, TIX_ID_AGENT))};
	round_trip(TIX_ENTITY_IDENTITY, id, 1,
		"{\"tix\":1,\"type\":\"identity\",\"author\":\"" A40 "\",\"lamport\":3,\"time\":9}\n"
		"{\"op\":\"set-kind\",\"kind\":\"agent\"}\n");
	tix_pack bad;
	tix_error be = TIX_ERROR_INIT;
	const char *robot = IHDR "{\"op\":\"set-kind\",\"kind\":\"robot\"}\n";
	TK_STATUS(tix_pack_decode(robot, strlen(robot), TIX_HASH_SHA1, &bad, &be), TIX_ERR_INVALID, &be);

	char stage[TIX_STAGE_MAX + 1];
	tix_ticket_status ph;
	TK_CHECK(tix_state_parse("open", &ph, stage) && ph == TIX_TICKET_OPEN && !stage[0]);
	TK_CHECK(tix_state_parse("in-progress/review", &ph, stage) && ph == TIX_TICKET_IN_PROGRESS &&
		 !strcmp(stage, "review"));
	TK_CHECK(!tix_state_parse("open/", &ph, stage));
	TK_CHECK(!tix_state_parse("later/x", &ph, stage));
	TK_CHECK(!tix_state_parse("open/Ready", &ph, stage));
	TK_CHECK(!tix_state_parse("open/a/b", &ph, stage));
	TK_CHECK(!tix_state_parse("in-progress-and-more-than-sixteen/x", &ph, stage));
}

int main(void)
{
	test_workflow_ops();
	test_baseline_ops();
	test_round_trip();
	test_identity_and_unknown();
	test_rejects();
	test_encode_limits();
	test_planning_ops();
	test_planning_rejects();
	return tk_finish();
}

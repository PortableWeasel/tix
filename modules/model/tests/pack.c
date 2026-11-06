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

int main(void)
{
	test_round_trip();
	test_identity_and_unknown();
	test_rejects();
	test_encode_limits();
	return tk_finish();
}

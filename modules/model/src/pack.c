/* Op-pack codec: canonical NDJSON encode, strict decode (architecture §3.3). */
#include "tix/base_json.h"
#include "tix/model.h"

#include <cjson/cJSON.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- op names per entity type ----------------------------------------- */

typedef struct op_name {
	tix_entity_type type;
	tix_op_kind kind;
	const char *name;
} op_name;

static const op_name op_names[] = {
	{TIX_ENTITY_TICKET, TIX_OP_TICKET_CREATE, "create"},
	{TIX_ENTITY_TICKET, TIX_OP_SET_TITLE, "set-title"},
	{TIX_ENTITY_TICKET, TIX_OP_SET_BODY, "set-body"},
	{TIX_ENTITY_TICKET, TIX_OP_SET_STATUS, "set-status"},
	{TIX_ENTITY_TICKET, TIX_OP_LABEL, "label"},
	{TIX_ENTITY_TICKET, TIX_OP_COMMENT, "comment"},
	{TIX_ENTITY_TICKET, TIX_OP_EDIT_COMMENT, "edit-comment"},
	{TIX_ENTITY_IDENTITY, TIX_OP_IDENTITY_CREATE, "create"},
	{TIX_ENTITY_IDENTITY, TIX_OP_SET_NAME, "set-name"},
	{TIX_ENTITY_IDENTITY, TIX_OP_SET_EMAIL, "set-email"},
	{TIX_ENTITY_IDENTITY, TIX_OP_KEY_ADD, "key-add"},
	{TIX_ENTITY_IDENTITY, TIX_OP_KEY_REMOVE, "key-remove"},
};

static const op_name *name_of_kind(tix_op_kind k)
{
	for (size_t i = 0; i < sizeof(op_names) / sizeof(*op_names); i++)
		if (op_names[i].kind == k)
			return &op_names[i];
	return NULL;
}

const char *tix_op_name(tix_op_kind kind)
{
	const op_name *n = name_of_kind(kind);
	return n ? n->name : "unknown";
}

static const op_name *kind_of_name(tix_entity_type t, const char *name)
{
	for (size_t i = 0; i < sizeof(op_names) / sizeof(*op_names); i++)
		if (op_names[i].type == t && !strcmp(op_names[i].name, name))
			return &op_names[i];
	return NULL;
}

/* ---- field validation (shared by encode and decode) ------------------- */

static bool has_control(const char *s, bool allow_newline_tab)
{
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (allow_newline_tab && (*p == '\n' || *p == '\t'))
			continue;
		if (*p < 0x20 || *p == 0x7f)
			return true;
	}
	return false;
}

static tix_status check_str(const char *what, const char *s, size_t min, size_t max, bool multiline,
			    tix_error *err)
{
	if (!s)
		return tix_error_set(err, TIX_ERR_INVALID, "%s missing", what);
	size_t n = strlen(s);
	if (n < min || n > max)
		return tix_error_set(err, TIX_ERR_INVALID, "%s length %zu outside %zu..%zu", what, n, min, max);
	if (!tix_utf8_valid(s, n))
		return tix_error_set(err, TIX_ERR_INVALID, "%s is not valid UTF-8", what);
	if (has_control(s, multiline))
		return tix_error_set(err, TIX_ERR_INVALID, "%s contains control characters", what);
	return TIX_OK;
}

static tix_status check_token(const char *what, const char *s, size_t max, tix_error *err)
{
	tix_status st = check_str(what, s, 1, max, false, err);
	if (st)
		return st;
	if (strpbrk(s, " ,"))
		return tix_error_set(err, TIX_ERR_INVALID, "%s '%s' contains space or comma", what, s);
	return TIX_OK;
}

static tix_status check_labels(const char *what, const tix_strs *s, tix_error *err)
{
	if (s->n > TIX_LABELS_PER_OP)
		return tix_error_set(err, TIX_ERR_INVALID, "too many %s labels (%zu)", what, s->n);
	for (size_t i = 0; i < s->n; i++) {
		tix_status st = check_token("label", s->v[i], TIX_LABEL_MAX, err);
		if (st)
			return st;
		if (i && strcmp(s->v[i - 1], s->v[i]) >= 0)
			return tix_error_set(err, TIX_ERR_INVALID, "%s labels not sorted/unique", what);
	}
	return TIX_OK;
}

static bool printable_ascii(const char *s)
{
	for (; *s; s++)
		if (*s < 0x21 || *s > 0x7e)
			return false;
	return true;
}

static tix_status check_nonce(const char *s, tix_error *err)
{
	if (!s || strlen(s) != TIX_NONCE_HEX)
		return tix_error_set(err, TIX_ERR_INVALID, "nonce must be %d hex chars", TIX_NONCE_HEX);
	for (; *s; s++)
		if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f')))
			return tix_error_set(err, TIX_ERR_INVALID, "nonce must be lowercase hex");
	return TIX_OK;
}

static tix_status check_op(tix_entity_type type, const tix_op *op, tix_error *err)
{
	const op_name *n = name_of_kind(op->kind);
	if (op->kind == TIX_OP_UNKNOWN || !n)
		return tix_error_set(err, TIX_ERR_INVALID, "cannot encode unknown op");
	if (n->type != type)
		return tix_error_set(err, TIX_ERR_INVALID, "op '%s' not valid for %s", n->name,
				     tix_entity_type_name(type));
	tix_status st = TIX_OK;
	switch (op->kind) {
	case TIX_OP_TICKET_CREATE:
		if ((st = check_nonce(op->u.ticket_create.nonce, err)) ||
		    (st = check_str("title", op->u.ticket_create.title, 1, TIX_TITLE_MAX, false, err)) ||
		    (st = check_str("body", op->u.ticket_create.body, 0, TIX_TEXT_MAX, true, err)))
			return st;
		return check_labels("create", &op->u.ticket_create.labels, err);
	case TIX_OP_SET_TITLE:
		return check_str("title", op->u.text.value, 1, TIX_TITLE_MAX, false, err);
	case TIX_OP_SET_BODY:
		return check_str("body", op->u.text.value, 0, TIX_TEXT_MAX, true, err);
	case TIX_OP_COMMENT:
		return check_str("comment", op->u.text.value, 1, TIX_TEXT_MAX, true, err);
	case TIX_OP_SET_STATUS:
		if ((unsigned)op->u.set_status.status > TIX_TICKET_CANCELLED)
			return tix_error_set(err, TIX_ERR_INVALID, "bad status");
		return TIX_OK;
	case TIX_OP_LABEL:
		if (!op->u.label.add.n && !op->u.label.remove.n)
			return tix_error_set(err, TIX_ERR_INVALID, "empty label op");
		if ((st = check_labels("add", &op->u.label.add, err)) ||
		    (st = check_labels("remove", &op->u.label.remove, err)))
			return st;
		for (size_t i = 0; i < op->u.label.add.n; i++)
			if (tix_strs_contains(&op->u.label.remove, op->u.label.add.v[i]))
				return tix_error_set(err, TIX_ERR_INVALID, "label '%s' both added and removed",
						     op->u.label.add.v[i]);
		return TIX_OK;
	case TIX_OP_EDIT_COMMENT:
		if (tix_oid_is_null(&op->u.edit_comment.target.commit) || op->u.edit_comment.target.line < 2)
			return tix_error_set(err, TIX_ERR_INVALID, "bad edit-comment target");
		return check_str("comment", op->u.edit_comment.body, 1, TIX_TEXT_MAX, true, err);
	case TIX_OP_IDENTITY_CREATE:
		if ((st = check_nonce(op->u.identity_create.nonce, err)) ||
		    (st = check_str("name", op->u.identity_create.name, 1, TIX_NAME_MAX, false, err)))
			return st;
		return check_token("email", op->u.identity_create.email, TIX_NAME_MAX, err);
	case TIX_OP_SET_NAME:
		return check_str("name", op->u.text.value, 1, TIX_NAME_MAX, false, err);
	case TIX_OP_SET_EMAIL:
		return check_token("email", op->u.text.value, TIX_NAME_MAX, err);
	case TIX_OP_KEY_ADD:
		if (!op->u.key_add.kind ||
		    (strcmp(op->u.key_add.kind, "ssh") && strcmp(op->u.key_add.kind, "gpg")))
			return tix_error_set(err, TIX_ERR_INVALID, "key kind must be ssh or gpg");
		if ((st = check_str("fingerprint", op->u.key_add.fingerprint, 1, TIX_FPR_MAX, false, err)))
			return st;
		if (!printable_ascii(op->u.key_add.fingerprint))
			return tix_error_set(err, TIX_ERR_INVALID, "fingerprint must be printable ASCII");
		return check_str("pubkey", op->u.key_add.pubkey, 1, TIX_PUBKEY_MAX, true, err);
	case TIX_OP_KEY_REMOVE:
		if ((st = check_str("fingerprint", op->u.key_remove.fingerprint, 1, TIX_FPR_MAX, false, err)))
			return st;
		if (!printable_ascii(op->u.key_remove.fingerprint))
			return tix_error_set(err, TIX_ERR_INVALID, "fingerprint must be printable ASCII");
		return TIX_OK;
	case TIX_OP_UNKNOWN:
		break;
	}
	return tix_error_set(err, TIX_ERR_INVALID, "unhandled op kind");
}

static tix_status check_header(const tix_pack *p, tix_error *err)
{
	if (p->type != TIX_ENTITY_TICKET && p->type != TIX_ENTITY_IDENTITY)
		return tix_error_set(err, TIX_ERR_INVALID, "bad entity type");
	if (p->lamport < 1 || p->lamport > TIX_LAMPORT_MAX)
		return tix_error_set(err, TIX_ERR_INVALID, "lamport out of range");
	if (p->time < 0 || (uint64_t)p->time > TIX_LAMPORT_MAX)
		return tix_error_set(err, TIX_ERR_INVALID, "time out of range");
	if (tix_oid_is_null(&p->author) && p->type != TIX_ENTITY_IDENTITY)
		return tix_error_set(err, TIX_ERR_INVALID, "%s pack needs an author", tix_entity_type_name(p->type));
	if (p->nops > TIX_PACK_MAX_OPS)
		return tix_error_set(err, TIX_ERR_INVALID, "too many ops (%zu)", p->nops);
	return TIX_OK;
}

/* ---- encoding ----------------------------------------------------------- */

static void put_strs(tix_jw *w, const tix_strs *s)
{
	tix_jw_arr_begin(w);
	for (size_t i = 0; i < s->n; i++)
		tix_jw_str(w, s->v[i]);
	tix_jw_arr_end(w);
}

static void put_kv(tix_jw *w, const char *k, const char *v)
{
	tix_jw_key(w, k);
	tix_jw_str(w, v);
}

static void encode_header(const tix_pack *p, tix_buf *out)
{
	char hex[TIX_OID_MAX_HEX + 1];
	tix_jw w;
	tix_jw_init(&w, out);
	tix_jw_obj_begin(&w);
	tix_jw_key(&w, "tix");
	tix_jw_int(&w, 1);
	put_kv(&w, "type", tix_entity_type_name(p->type));
	tix_jw_key(&w, "author");
	if (tix_oid_is_null(&p->author))
		tix_jw_null(&w);
	else
		tix_jw_str(&w, tix_oid_hex(&p->author, hex));
	tix_jw_key(&w, "lamport");
	tix_jw_int(&w, (int64_t)p->lamport);
	tix_jw_key(&w, "time");
	tix_jw_int(&w, p->time);
	tix_jw_obj_end(&w);
}

/* REQUIRES: check_op passed. */
static void encode_op(const tix_op *op, tix_buf *out)
{
	char id[TIX_OID_MAX_HEX + 12];
	tix_jw w;
	tix_jw_init(&w, out);
	tix_jw_obj_begin(&w);
	put_kv(&w, "op", name_of_kind(op->kind)->name);
	switch (op->kind) {
	case TIX_OP_TICKET_CREATE:
		put_kv(&w, "nonce", op->u.ticket_create.nonce);
		put_kv(&w, "title", op->u.ticket_create.title);
		put_kv(&w, "body", op->u.ticket_create.body);
		tix_jw_key(&w, "labels");
		put_strs(&w, &op->u.ticket_create.labels);
		break;
	case TIX_OP_SET_TITLE: put_kv(&w, "title", op->u.text.value); break;
	case TIX_OP_SET_BODY: put_kv(&w, "body", op->u.text.value); break;
	case TIX_OP_COMMENT: put_kv(&w, "body", op->u.text.value); break;
	case TIX_OP_SET_STATUS: put_kv(&w, "status", tix_ticket_status_name(op->u.set_status.status)); break;
	case TIX_OP_LABEL:
		tix_jw_key(&w, "add");
		put_strs(&w, &op->u.label.add);
		tix_jw_key(&w, "remove");
		put_strs(&w, &op->u.label.remove);
		break;
	case TIX_OP_EDIT_COMMENT:
		put_kv(&w, "target", tix_opid_str(&op->u.edit_comment.target, id));
		put_kv(&w, "body", op->u.edit_comment.body);
		break;
	case TIX_OP_IDENTITY_CREATE:
		put_kv(&w, "nonce", op->u.identity_create.nonce);
		put_kv(&w, "name", op->u.identity_create.name);
		put_kv(&w, "email", op->u.identity_create.email);
		break;
	case TIX_OP_SET_NAME: put_kv(&w, "name", op->u.text.value); break;
	case TIX_OP_SET_EMAIL: put_kv(&w, "email", op->u.text.value); break;
	case TIX_OP_KEY_ADD:
		put_kv(&w, "kind", op->u.key_add.kind);
		put_kv(&w, "fingerprint", op->u.key_add.fingerprint);
		put_kv(&w, "pubkey", op->u.key_add.pubkey);
		break;
	case TIX_OP_KEY_REMOVE: put_kv(&w, "fingerprint", op->u.key_remove.fingerprint); break;
	case TIX_OP_UNKNOWN: TIX_REQUIRE(!"unknown op"); break;
	}
	tix_jw_obj_end(&w);
}

tix_status tix_pack_encode(const tix_pack *p, tix_buf *out, tix_error *err)
{
	tix_status st = check_header(p, err);
	if (st)
		return st;
	for (size_t i = 0; i < p->nops; i++) {
		if (!tix_oid_is_null(&p->author) && p->ops[i].kind == TIX_OP_EDIT_COMMENT &&
		    p->ops[i].u.edit_comment.target.commit.algo != p->author.algo)
			return tix_error_set(err, TIX_ERR_INVALID, "op %zu: target hash algorithm mismatch", i);
		if ((st = check_op(p->type, &p->ops[i], err))) {
			tix_error_prefix(err, "op %zu", i);
			return st;
		}
	}
	tix_buf tmp = TIX_BUF_INIT;
	encode_header(p, &tmp);
	tix_buf_addc(&tmp, '\n');
	for (size_t i = 0; i < p->nops; i++) {
		encode_op(&p->ops[i], &tmp);
		tix_buf_addc(&tmp, '\n');
	}
	if (tmp.len > TIX_PACK_MAX_BYTES) {
		tix_buf_free(&tmp);
		return tix_error_set(err, TIX_ERR_INVALID, "pack exceeds %u bytes", TIX_PACK_MAX_BYTES);
	}
	tix_buf_add(out, tmp.data, tmp.len);
	tix_buf_free(&tmp);
	return TIX_OK;
}

/* ---- decoding ----------------------------------------------------------- */

void tix_pack_free(tix_pack *p)
{
	for (size_t i = 0; i < p->nops; i++)
		tix_op_free(&p->ops[i]);
	free(p->ops);
	memset(p, 0, sizeof(*p));
}

static cJSON *parse_obj(const char *line, size_t len, tix_error *err)
{
	const char *end = NULL;
	cJSON *j = cJSON_ParseWithLengthOpts(line, len, &end, false);
	if (!j || !cJSON_IsObject(j) || end != line + len) {
		cJSON_Delete(j);
		tix_error_set(err, TIX_ERR_INVALID, "not a single JSON object");
		return NULL;
	}
	return j;
}

static bool get_int(const cJSON *o, const char *key, int64_t min, int64_t max, int64_t *out)
{
	const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
	if (!cJSON_IsNumber(it))
		return false;
	double d = it->valuedouble;
	if (!(d >= (double)min && d <= (double)max) || floor(d) != d)
		return false;
	*out = (int64_t)d;
	return true;
}

static const char *get_str(const cJSON *o, const char *key)
{
	const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
	return cJSON_IsString(it) ? it->valuestring : NULL;
}

static char *dup_str(const cJSON *o, const char *key)
{
	const char *s = get_str(o, key);
	return s ? tix_xstrdup(s) : NULL;
}

/* Adds every string of array `key`; canonical re-encoding catches order/dups. */
static bool get_strs(const cJSON *o, const char *key, tix_strs *out)
{
	const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, key), *it;
	if (!cJSON_IsArray(a))
		return false;
	size_t n = 0;
	cJSON_ArrayForEach(it, a)
	{
		if (!cJSON_IsString(it) || ++n > TIX_LABELS_PER_OP)
			return false;
		tix_strs_add(out, it->valuestring);
	}
	return true;
}

static tix_status canonical_check(const tix_buf *enc, const char *line, size_t len, tix_error *err)
{
	if (enc->len != len || memcmp(enc->data, line, len))
		return tix_error_set(err, TIX_ERR_INVALID, "not in canonical form");
	return TIX_OK;
}

static tix_status decode_header(const char *line, size_t len, tix_hash_algo algo, tix_pack *p,
				tix_error *err)
{
	cJSON *j = parse_obj(line, len, err);
	if (!j)
		return TIX_ERR_INVALID;
	tix_status st = TIX_ERR_INVALID;
	int64_t ver, lamport, time;
	const char *type = get_str(j, "type");
	const cJSON *author = cJSON_GetObjectItemCaseSensitive(j, "author");

	if (!get_int(j, "tix", 0, 1000, &ver)) {
		tix_error_set(err, TIX_ERR_INVALID, "missing format version");
		goto out;
	}
	if (ver != 1) {
		st = tix_error_set(err, TIX_ERR_UNSUPPORTED, "format version %lld", (long long)ver);
		goto out;
	}
	if (!type || (strcmp(type, "ticket") && strcmp(type, "identity"))) {
		tix_error_set(err, TIX_ERR_INVALID, "bad entity type");
		goto out;
	}
	p->type = !strcmp(type, "ticket") ? TIX_ENTITY_TICKET : TIX_ENTITY_IDENTITY;
	memset(&p->author, 0, sizeof(p->author));
	if (cJSON_IsString(author)) {
		if (!tix_oid_from_hex(&p->author, author->valuestring, strlen(author->valuestring)) ||
		    p->author.algo != algo) {
			tix_error_set(err, TIX_ERR_INVALID, "bad author id");
			goto out;
		}
	} else if (!cJSON_IsNull(author)) {
		tix_error_set(err, TIX_ERR_INVALID, "author must be an id or null");
		goto out;
	}
	if (!get_int(j, "lamport", 1, (int64_t)TIX_LAMPORT_MAX, &lamport) ||
	    !get_int(j, "time", 0, (int64_t)TIX_LAMPORT_MAX, &time)) {
		tix_error_set(err, TIX_ERR_INVALID, "bad lamport or time");
		goto out;
	}
	p->lamport = (uint64_t)lamport;
	p->time = time;
	if ((st = check_header(p, err)))
		goto out;

	tix_buf enc = TIX_BUF_INIT;
	encode_header(p, &enc);
	st = canonical_check(&enc, line, len, err);
	tix_buf_free(&enc);
out:
	cJSON_Delete(j);
	return st;
}

static tix_status fill_op(const cJSON *j, tix_op *op, tix_hash_algo algo, tix_error *err)
{
	const char *s;
	switch (op->kind) {
	case TIX_OP_TICKET_CREATE:
		op->u.ticket_create.nonce = dup_str(j, "nonce");
		op->u.ticket_create.title = dup_str(j, "title");
		op->u.ticket_create.body = dup_str(j, "body");
		if (!get_strs(j, "labels", &op->u.ticket_create.labels))
			return tix_error_set(err, TIX_ERR_INVALID, "bad labels");
		return TIX_OK;
	case TIX_OP_SET_TITLE: op->u.text.value = dup_str(j, "title"); return TIX_OK;
	case TIX_OP_SET_BODY:
	case TIX_OP_COMMENT: op->u.text.value = dup_str(j, "body"); return TIX_OK;
	case TIX_OP_SET_NAME: op->u.text.value = dup_str(j, "name"); return TIX_OK;
	case TIX_OP_SET_EMAIL: op->u.text.value = dup_str(j, "email"); return TIX_OK;
	case TIX_OP_SET_STATUS:
		s = get_str(j, "status");
		if (!s || !tix_ticket_status_parse(s, &op->u.set_status.status))
			return tix_error_set(err, TIX_ERR_INVALID, "bad status");
		return TIX_OK;
	case TIX_OP_LABEL:
		if (!get_strs(j, "add", &op->u.label.add) || !get_strs(j, "remove", &op->u.label.remove))
			return tix_error_set(err, TIX_ERR_INVALID, "bad label lists");
		return TIX_OK;
	case TIX_OP_EDIT_COMMENT:
		s = get_str(j, "target");
		if (!s || !tix_opid_parse(&op->u.edit_comment.target, s) ||
		    op->u.edit_comment.target.commit.algo != algo)
			return tix_error_set(err, TIX_ERR_INVALID, "bad target");
		op->u.edit_comment.body = dup_str(j, "body");
		return TIX_OK;
	case TIX_OP_IDENTITY_CREATE:
		op->u.identity_create.nonce = dup_str(j, "nonce");
		op->u.identity_create.name = dup_str(j, "name");
		op->u.identity_create.email = dup_str(j, "email");
		return TIX_OK;
	case TIX_OP_KEY_ADD:
		op->u.key_add.kind = dup_str(j, "kind");
		op->u.key_add.fingerprint = dup_str(j, "fingerprint");
		op->u.key_add.pubkey = dup_str(j, "pubkey");
		return TIX_OK;
	case TIX_OP_KEY_REMOVE: op->u.key_remove.fingerprint = dup_str(j, "fingerprint"); return TIX_OK;
	case TIX_OP_UNKNOWN: break;
	}
	return tix_error_set(err, TIX_ERR_INVALID, "unhandled op");
}

static tix_status decode_op(const char *line, size_t len, tix_entity_type type, tix_hash_algo algo,
			    tix_op *op, tix_error *err)
{
	memset(op, 0, sizeof(*op));
	cJSON *j = parse_obj(line, len, err);
	if (!j)
		return TIX_ERR_INVALID;
	tix_status st = TIX_ERR_INVALID;
	const cJSON *first = j->child;
	if (!first || strcmp(first->string, "op") || !cJSON_IsString(first) || !first->valuestring[0]) {
		tix_error_set(err, TIX_ERR_INVALID, "first key must be \"op\"");
		goto out;
	}
	const op_name *n = kind_of_name(type, first->valuestring);
	if (!n) { /* forward compatibility: keep verbatim, ignore */
		op->kind = TIX_OP_UNKNOWN;
		op->u.unknown.name = tix_xstrdup(first->valuestring);
		op->u.unknown.raw = tix_xstrndup(line, len);
		st = TIX_OK;
		goto out;
	}
	op->kind = n->kind;
	if ((st = fill_op(j, op, algo, err)) || (st = check_op(type, op, err)))
		goto out;
	tix_buf enc = TIX_BUF_INIT;
	encode_op(op, &enc);
	st = canonical_check(&enc, line, len, err);
	tix_buf_free(&enc);
out:
	cJSON_Delete(j);
	if (st)
		tix_op_free(op);
	return st;
}

tix_status tix_pack_decode(const char *data, size_t len, tix_hash_algo algo, tix_pack *out,
			   tix_error *err)
{
	memset(out, 0, sizeof(*out));
	TIX_REQUIRE(algo == TIX_HASH_SHA1 || algo == TIX_HASH_SHA256);
	if (len == 0 || len > TIX_PACK_MAX_BYTES)
		return tix_error_set(err, TIX_ERR_INVALID, "pack size %zu out of range", len);
	if (!tix_utf8_valid(data, len))
		return tix_error_set(err, TIX_ERR_INVALID, "pack is not valid UTF-8");
	if (data[len - 1] != '\n')
		return tix_error_set(err, TIX_ERR_INVALID, "pack must end with a newline");

	TIX_VEC(tix_op) ops = {0};
	tix_status st = TIX_OK;
	size_t pos = 0;
	uint32_t lineno = 0;
	while (pos < len) {
		const char *line = data + pos;
		const char *nl = memchr(line, '\n', len - pos);
		size_t n = (size_t)(nl - line);
		lineno++;
		if (n == 0) {
			st = tix_error_set(err, TIX_ERR_INVALID, "empty line");
		} else if (lineno == 1) {
			st = decode_header(line, n, algo, out, err);
		} else if (ops.n >= TIX_PACK_MAX_OPS) {
			st = tix_error_set(err, TIX_ERR_INVALID, "too many ops");
		} else {
			tix_op op;
			st = decode_op(line, n, out->type, algo, &op, err);
			if (!st)
				TIX_VEC_PUSH(&ops, op);
		}
		if (st) {
			tix_error_prefix(err, "line %u", (unsigned)lineno);
			break;
		}
		pos += n + 1;
	}
	out->ops = ops.v;
	out->nops = ops.n;
	if (st)
		tix_pack_free(out);
	return st;
}

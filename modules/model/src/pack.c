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
	{TIX_ENTITY_TICKET, TIX_OP_DEP_ADD, "dep-add"},
	{TIX_ENTITY_TICKET, TIX_OP_DEP_REMOVE, "dep-remove"},
	{TIX_ENTITY_TICKET, TIX_OP_ASSIGN, "assign"},
	{TIX_ENTITY_TICKET, TIX_OP_UNASSIGN, "unassign"},
	{TIX_ENTITY_RESOURCE, TIX_OP_RESOURCE_CREATE, "create"},
	{TIX_ENTITY_CALENDAR, TIX_OP_CALENDAR_CREATE, "create"},
	{TIX_ENTITY_CALENDAR, TIX_OP_EXCEPTION_ADD, "exception-add"},
	{TIX_ENTITY_CALENDAR, TIX_OP_EXCEPTION_REMOVE, "exception-remove"},
	{TIX_ENTITY_BASELINE, TIX_OP_BASELINE_CREATE, "create"},
	{TIX_ENTITY_BASELINE, TIX_OP_BASELINE_SET_NAME, "set-name"},
	{TIX_ENTITY_BASELINE, TIX_OP_BASELINE_TASK, "task"},
	{TIX_ENTITY_TICKET, TIX_OP_SET_STATE, "set-state"},
	{TIX_ENTITY_TICKET, TIX_OP_STAGE_ADD, "stage-add"},
	{TIX_ENTITY_TICKET, TIX_OP_STAGE_REMOVE, "stage-remove"},
};

/* Set-field ops, by (type, name). */
static bool field_of_name(tix_entity_type t, const char *name, tix_field *out)
{
	for (unsigned i = 0; i < TIX_F_COUNT; i++) {
		const tix_field_info *fi = tix_field_get((tix_field)i);
		if (fi->type == t && !strcmp(fi->op, name)) {
			*out = (tix_field)i;
			return true;
		}
	}
	return false;
}

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

static bool is_upper3(const char *s)
{
	for (int i = 0; i < 3; i++)
		if (s[i] < 'A' || s[i] > 'Z')
			return false;
	return s[3] == '\0';
}

static tix_status check_value(tix_field f, const tix_value *v, tix_error *err)
{
	const tix_field_info *fi = tix_field_get(f);
	if (v->kind == TIX_V_NULL)
		return fi->nullable ? TIX_OK : tix_error_set(err, TIX_ERR_INVALID, "%s cannot be null", fi->key);
	if (v->kind != fi->kind)
		return tix_error_set(err, TIX_ERR_INVALID, "%s has the wrong type", fi->key);
	tix_status st;
	switch (v->kind) {
	case TIX_V_INT:
		if (v->i < fi->min || v->i > fi->max)
			return tix_error_set(err, TIX_ERR_INVALID, "%s %lld outside %lld..%lld", fi->key, (long long)v->i,
					     (long long)fi->min, (long long)fi->max);
		return TIX_OK;
	case TIX_V_BOOL:
		return v->i == 0 || v->i == 1 ? TIX_OK : tix_error_set(err, TIX_ERR_INVALID, "%s not a bool", fi->key);
	case TIX_V_DATE:
		return tix_date_valid((tix_date)v->i) && v->i == (tix_date)v->i
			       ? TIX_OK
			       : tix_error_set(err, TIX_ERR_INVALID, "%s is not a date in range", fi->key);
	case TIX_V_ENUM: {
		int64_t n = 0;
		while (fi->enums[n])
			n++;
		return v->i >= 0 && v->i < n ? TIX_OK : tix_error_set(err, TIX_ERR_INVALID, "bad %s", fi->key);
	}
	case TIX_V_STR:
		if ((st = check_str(fi->key, v->s, (size_t)fi->min, (size_t)fi->max, false, err)))
			return st;
		if (f == TIX_F_RANK && !tix_rank_valid(v->s))
			return tix_error_set(err, TIX_ERR_INVALID, "rank must be [0-9a-z], not ending in 0");
		if (f == TIX_F_CURRENCY && !is_upper3(v->s))
			return tix_error_set(err, TIX_ERR_INVALID, "currency must be 3 letters A-Z");
		return TIX_OK;
	case TIX_V_ID:
		return tix_oid_is_null(&v->id) ? tix_error_set(err, TIX_ERR_INVALID, "%s: null id", fi->key) : TIX_OK;
	case TIX_V_NULL:
		break;
	}
	return tix_error_set(err, TIX_ERR_INVALID, "bad value");
}

static tix_status check_id(const char *what, const tix_oid *id, tix_error *err)
{
	return tix_oid_is_null(id) ? tix_error_set(err, TIX_ERR_INVALID, "%s: null id", what) : TIX_OK;
}

static tix_status check_op(tix_entity_type type, const tix_op *op, tix_error *err)
{
	if (op->kind == TIX_OP_SET_FIELD) {
		if ((unsigned)op->u.set.field >= TIX_F_COUNT || tix_field_get(op->u.set.field)->type != type)
			return tix_error_set(err, TIX_ERR_INVALID, "field not valid for %s", tix_entity_type_name(type));
		return check_value(op->u.set.field, &op->u.set.v, err);
	}
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
	case TIX_OP_DEP_ADD:
		if ((unsigned)op->u.dep_add.kind > TIX_DEP_SF)
			return tix_error_set(err, TIX_ERR_INVALID, "bad dependency kind");
		if (op->u.dep_add.lag < -TIX_LAG_MAX || op->u.dep_add.lag > TIX_LAG_MAX)
			return tix_error_set(err, TIX_ERR_INVALID, "lag outside +-%d days", TIX_LAG_MAX);
		return check_id("pred", &op->u.dep_add.pred, err);
	case TIX_OP_DEP_REMOVE:
		return check_id("pred", &op->u.unref.target, err);
	case TIX_OP_UNASSIGN:
		return check_id("resource", &op->u.unref.target, err);
	case TIX_OP_ASSIGN:
		if (op->u.assign.units < 1 || op->u.assign.units > TIX_UNITS_MAX)
			return tix_error_set(err, TIX_ERR_INVALID, "units outside 1..%d", TIX_UNITS_MAX);
		if (op->u.assign.effort < -1 || op->u.assign.effort > TIX_EFFORT_MAX)
			return tix_error_set(err, TIX_ERR_INVALID, "bad assignment effort");
		return check_id("resource", &op->u.assign.resource, err);
	case TIX_OP_RESOURCE_CREATE:
		if ((st = check_nonce(op->u.resource_create.nonce, err)) ||
		    (st = check_str("name", op->u.resource_create.name, 1, TIX_NAME_MAX, false, err)))
			return st;
		return (unsigned)op->u.resource_create.kind <= TIX_RES_EQUIPMENT
			       ? TIX_OK
			       : tix_error_set(err, TIX_ERR_INVALID, "bad resource kind");
	case TIX_OP_CALENDAR_CREATE:
		if ((st = check_nonce(op->u.calendar_create.nonce, err)))
			return st;
		return check_str("name", op->u.calendar_create.name, 1, TIX_NAME_MAX, false, err);
	case TIX_OP_EXCEPTION_ADD:
	case TIX_OP_EXCEPTION_REMOVE: {
		tix_date from = op->kind == TIX_OP_EXCEPTION_ADD ? op->u.exception_add.from : op->u.exception_remove.from;
		tix_date to = op->kind == TIX_OP_EXCEPTION_ADD ? op->u.exception_add.to : op->u.exception_remove.to;
		if (!tix_date_valid(from) || !tix_date_valid(to) || from > to || to - from >= TIX_EXCEPTION_MAX_DAYS)
			return tix_error_set(err, TIX_ERR_INVALID, "exception range must be 1..%d days, from <= to",
					     TIX_EXCEPTION_MAX_DAYS);
		if (op->kind == TIX_OP_EXCEPTION_ADD)
			return check_str("note", op->u.exception_add.note, 0, TIX_TITLE_MAX, false, err);
		return TIX_OK;
	}
	case TIX_OP_BASELINE_CREATE:
		if ((st = check_nonce(op->u.baseline_create.nonce, err)) ||
		    (st = check_str("name", op->u.baseline_create.name, 1, TIX_NAME_MAX, false, err)))
			return st;
		if (!tix_date_valid(op->u.baseline_create.taken))
			return tix_error_set(err, TIX_ERR_INVALID, "bad baseline date");
		return TIX_OK;
	case TIX_OP_BASELINE_SET_NAME:
		return check_str("name", op->u.text.value, 1, TIX_NAME_MAX, false, err);
	case TIX_OP_BASELINE_TASK: {
		const tix_baseline_task_op *b = &op->u.baseline_task;
		if ((st = check_id("ticket", &b->ticket, err)))
			return st;
		if ((b->start != TIX_DATE_NONE && !tix_date_valid(b->start)) ||
		    (b->finish != TIX_DATE_NONE && !tix_date_valid(b->finish)) ||
		    (b->start == TIX_DATE_NONE) != (b->finish == TIX_DATE_NONE) ||
		    (b->start != TIX_DATE_NONE && b->finish < b->start))
			return tix_error_set(err, TIX_ERR_INVALID, "baseline task dates must both be set, start <= finish");
		if (b->duration < -1 || b->duration > TIX_DURATION_MAX || b->effort < -1 || b->effort > TIX_EFFORT_MAX ||
		    b->cost < -1 || b->cost > TIX_COST_MAX)
			return tix_error_set(err, TIX_ERR_INVALID, "baseline task number out of range");
		if (b->currency) {
			size_t len = strlen(b->currency);
			if (!len || len > TIX_CURRENCY_MAX)
				return tix_error_set(err, TIX_ERR_INVALID, "bad currency");
			for (size_t i = 0; i < len; i++)
				if (!((b->currency[i] >= 'A' && b->currency[i] <= 'Z') ||
				      (b->currency[i] >= 'a' && b->currency[i] <= 'z')))
					return tix_error_set(err, TIX_ERR_INVALID, "bad currency");
		}
		if ((b->cost < 0) != !b->currency)
			return tix_error_set(err, TIX_ERR_INVALID, "a baseline cost needs a currency, and only a cost");
		return TIX_OK;
	}
	case TIX_OP_SET_STATE:
		if ((unsigned)op->u.set_state.status > TIX_TICKET_CANCELLED)
			return tix_error_set(err, TIX_ERR_INVALID, "bad status");
		if (op->u.set_state.stage && !tix_stage_valid(op->u.set_state.stage))
			return tix_error_set(err, TIX_ERR_INVALID, "stage must be [a-z][a-z0-9-]{0,%d}", TIX_STAGE_MAX - 1);
		return TIX_OK;
	case TIX_OP_STAGE_ADD:
		if (!tix_stage_valid(op->u.stage_add.name))
			return tix_error_set(err, TIX_ERR_INVALID, "stage must be [a-z][a-z0-9-]{0,%d}", TIX_STAGE_MAX - 1);
		if ((unsigned)op->u.stage_add.phase > TIX_TICKET_CANCELLED)
			return tix_error_set(err, TIX_ERR_INVALID, "bad phase");
		if (op->u.stage_add.rank < 0 || op->u.stage_add.rank > TIX_STAGE_RANK_MAX)
			return tix_error_set(err, TIX_ERR_INVALID, "stage rank outside 0..%d", TIX_STAGE_RANK_MAX);
		return TIX_OK;
	case TIX_OP_STAGE_REMOVE:
		if (!tix_stage_valid(op->u.stage_remove.name))
			return tix_error_set(err, TIX_ERR_INVALID, "bad stage name");
		return TIX_OK;
	case TIX_OP_SET_FIELD:
	case TIX_OP_UNKNOWN:
		break;
	}
	return tix_error_set(err, TIX_ERR_INVALID, "unhandled op kind");
}

static tix_status check_header(const tix_pack *p, tix_error *err)
{
	if (p->type < TIX_ENTITY_IDENTITY || p->type > TIX_ENTITY_BASELINE)
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

static void put_id(tix_jw *w, const char *k, const tix_oid *id)
{
	char hex[TIX_OID_MAX_HEX + 1];
	put_kv(w, k, tix_oid_hex(id, hex));
}

static void put_date(tix_jw *w, const char *k, tix_date d)
{
	char s[TIX_DATE_STR];
	put_kv(w, k, tix_date_str(d, s));
}

static void put_id_or_null(tix_jw *w, const char *k, const tix_oid *id)
{
	if (tix_oid_is_null(id)) {
		tix_jw_key(w, k);
		tix_jw_null(w);
	} else {
		put_id(w, k, id);
	}
}

static void put_date_or_null(tix_jw *w, const char *k, tix_date d)
{
	if (d == TIX_DATE_NONE) {
		tix_jw_key(w, k);
		tix_jw_null(w);
	} else {
		put_date(w, k, d);
	}
}

static void put_int_or_null(tix_jw *w, const char *k, int64_t v)
{
	tix_jw_key(w, k);
	if (v < 0)
		tix_jw_null(w);
	else
		tix_jw_int(w, v);
}

static void put_value(tix_jw *w, tix_field f, const tix_value *v)
{
	const tix_field_info *fi = tix_field_get(f);
	switch (v->kind) {
	case TIX_V_NULL: tix_jw_key(w, fi->key); tix_jw_null(w); break;
	case TIX_V_INT: tix_jw_key(w, fi->key); tix_jw_int(w, v->i); break;
	case TIX_V_BOOL: tix_jw_key(w, fi->key); tix_jw_bool(w, v->i != 0); break;
	case TIX_V_DATE: put_date(w, fi->key, (tix_date)v->i); break;
	case TIX_V_ENUM: put_kv(w, fi->key, fi->enums[v->i]); break;
	case TIX_V_STR: put_kv(w, fi->key, v->s); break;
	case TIX_V_ID: put_id(w, fi->key, &v->id); break;
	}
}

/* REQUIRES: check_op passed. */
static void encode_op(const tix_op *op, tix_buf *out)
{
	char id[TIX_OID_MAX_HEX + 12];
	tix_jw w;
	tix_jw_init(&w, out);
	tix_jw_obj_begin(&w);
	put_kv(&w, "op", tix_op_wire_name(op));
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
	case TIX_OP_SET_FIELD: put_value(&w, op->u.set.field, &op->u.set.v); break;
	case TIX_OP_DEP_ADD:
		put_id(&w, "pred", &op->u.dep_add.pred);
		put_kv(&w, "kind", tix_dep_kind_name(op->u.dep_add.kind));
		tix_jw_key(&w, "lag");
		tix_jw_int(&w, op->u.dep_add.lag);
		break;
	case TIX_OP_DEP_REMOVE: put_id(&w, "pred", &op->u.unref.target); break;
	case TIX_OP_ASSIGN:
		put_id(&w, "resource", &op->u.assign.resource);
		tix_jw_key(&w, "units");
		tix_jw_int(&w, op->u.assign.units);
		tix_jw_key(&w, "effort");
		if (op->u.assign.effort < 0)
			tix_jw_null(&w);
		else
			tix_jw_int(&w, op->u.assign.effort);
		break;
	case TIX_OP_UNASSIGN: put_id(&w, "resource", &op->u.unref.target); break;
	case TIX_OP_RESOURCE_CREATE:
		put_kv(&w, "nonce", op->u.resource_create.nonce);
		put_kv(&w, "name", op->u.resource_create.name);
		put_kv(&w, "kind", tix_field_get(TIX_F_RES_KIND)->enums[op->u.resource_create.kind]);
		break;
	case TIX_OP_CALENDAR_CREATE:
		put_kv(&w, "nonce", op->u.calendar_create.nonce);
		put_kv(&w, "name", op->u.calendar_create.name);
		break;
	case TIX_OP_EXCEPTION_ADD:
		put_date(&w, "from", op->u.exception_add.from);
		put_date(&w, "to", op->u.exception_add.to);
		tix_jw_key(&w, "working");
		tix_jw_bool(&w, op->u.exception_add.working);
		put_kv(&w, "note", op->u.exception_add.note);
		break;
	case TIX_OP_EXCEPTION_REMOVE:
		put_date(&w, "from", op->u.exception_remove.from);
		put_date(&w, "to", op->u.exception_remove.to);
		break;
	case TIX_OP_BASELINE_CREATE:
		put_kv(&w, "nonce", op->u.baseline_create.nonce);
		put_kv(&w, "name", op->u.baseline_create.name);
		put_date(&w, "taken", op->u.baseline_create.taken);
		put_id_or_null(&w, "root", &op->u.baseline_create.root);
		break;
	case TIX_OP_BASELINE_SET_NAME: put_kv(&w, "name", op->u.text.value); break;
	case TIX_OP_BASELINE_TASK:
		put_id(&w, "ticket", &op->u.baseline_task.ticket);
		put_date_or_null(&w, "start", op->u.baseline_task.start);
		put_date_or_null(&w, "finish", op->u.baseline_task.finish);
		put_int_or_null(&w, "duration", op->u.baseline_task.duration);
		put_int_or_null(&w, "effort", op->u.baseline_task.effort);
		put_int_or_null(&w, "cost", op->u.baseline_task.cost);
		tix_jw_key(&w, "currency");
		if (op->u.baseline_task.currency)
			tix_jw_str(&w, op->u.baseline_task.currency);
		else
			tix_jw_null(&w);
		break;
	case TIX_OP_SET_STATE:
		put_kv(&w, "status", tix_ticket_status_name(op->u.set_state.status));
		tix_jw_key(&w, "stage");
		if (op->u.set_state.stage)
			tix_jw_str(&w, op->u.set_state.stage);
		else
			tix_jw_null(&w);
		break;
	case TIX_OP_STAGE_ADD:
		put_kv(&w, "name", op->u.stage_add.name);
		put_kv(&w, "phase", tix_ticket_status_name(op->u.stage_add.phase));
		tix_jw_key(&w, "human");
		tix_jw_bool(&w, op->u.stage_add.human);
		tix_jw_key(&w, "rank");
		tix_jw_int(&w, op->u.stage_add.rank);
		break;
	case TIX_OP_STAGE_REMOVE: put_kv(&w, "name", op->u.stage_remove.name); break;
	case TIX_OP_UNKNOWN: TIX_REQUIRE(!"unknown op"); break;
	}
	tix_jw_obj_end(&w);
}

/* The entity ids an op embeds (at most one). */
static size_t op_ids(const tix_op *op, const tix_oid *ids[1])
{
	switch (op->kind) {
	case TIX_OP_EDIT_COMMENT: ids[0] = &op->u.edit_comment.target.commit; return 1;
	case TIX_OP_DEP_ADD: ids[0] = &op->u.dep_add.pred; return 1;
	case TIX_OP_DEP_REMOVE:
	case TIX_OP_UNASSIGN: ids[0] = &op->u.unref.target; return 1;
	case TIX_OP_ASSIGN: ids[0] = &op->u.assign.resource; return 1;
	case TIX_OP_SET_FIELD:
		if (op->u.set.v.kind != TIX_V_ID)
			return 0;
		ids[0] = &op->u.set.v.id;
		return 1;
	default: return 0;
	}
}

tix_status tix_pack_encode(const tix_pack *p, tix_buf *out, tix_error *err)
{
	tix_status st = check_header(p, err);
	if (st)
		return st;
	for (size_t i = 0; i < p->nops; i++) {
		const tix_oid *ids[1];
		size_t nids = op_ids(&p->ops[i], ids);
		for (size_t k = 0; k < nids && !tix_oid_is_null(&p->author); k++)
			if (!tix_oid_is_null(ids[k]) && ids[k]->algo != p->author.algo)
				return tix_error_set(err, TIX_ERR_INVALID, "op %zu: id hash algorithm mismatch", i);
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

static bool get_id(const cJSON *o, const char *key, tix_hash_algo algo, tix_oid *out)
{
	const char *s = get_str(o, key);
	return s && tix_oid_from_hex(out, s, strlen(s)) && out->algo == algo;
}

static bool get_date(const cJSON *o, const char *key, tix_date *out)
{
	const char *s = get_str(o, key);
	return s && tix_date_parse(s, out);
}

/* null -> null oid / TIX_DATE_NONE / -1; anything else must parse. */
static bool get_id_or_null(const cJSON *o, const char *key, tix_hash_algo algo, tix_oid *out)
{
	if (cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(o, key))) {
		memset(out, 0, sizeof(*out));
		return true;
	}
	return get_id(o, key, algo, out);
}

static bool get_date_or_null(const cJSON *o, const char *key, tix_date *out)
{
	if (cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(o, key))) {
		*out = TIX_DATE_NONE;
		return true;
	}
	return get_date(o, key, out);
}

static bool get_int_or_null(const cJSON *o, const char *key, int64_t max, int64_t *out)
{
	if (cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(o, key))) {
		*out = -1;
		return true;
	}
	return get_int(o, key, 0, max, out);
}

/* Reads field f's value; the canonical check afterwards rejects any other form. */
static tix_status get_value(const cJSON *o, tix_field f, tix_hash_algo algo, tix_value *v, tix_error *err)
{
	const tix_field_info *fi = tix_field_get(f);
	const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, fi->key);
	*v = tix_value_null();
	if (!it || cJSON_IsNull(it))
		return it ? TIX_OK : tix_error_set(err, TIX_ERR_INVALID, "%s missing", fi->key);
	v->kind = fi->kind;
	switch (fi->kind) {
	case TIX_V_INT:
		if (!get_int(o, fi->key, INT64_MIN / 2, INT64_MAX / 2, &v->i))
			return tix_error_set(err, TIX_ERR_INVALID, "%s must be an integer", fi->key);
		return TIX_OK;
	case TIX_V_BOOL:
		if (!cJSON_IsBool(it))
			return tix_error_set(err, TIX_ERR_INVALID, "%s must be a bool", fi->key);
		v->i = cJSON_IsTrue(it);
		return TIX_OK;
	case TIX_V_DATE: {
		tix_date d;
		if (!get_date(o, fi->key, &d))
			return tix_error_set(err, TIX_ERR_INVALID, "%s must be a YYYY-MM-DD date", fi->key);
		v->i = d;
		return TIX_OK;
	}
	case TIX_V_ENUM: {
		const char *s = get_str(o, fi->key);
		int k = s ? tix_field_enum(f, s) : -1;
		if (k < 0)
			return tix_error_set(err, TIX_ERR_INVALID, "bad %s", fi->key);
		v->i = k;
		return TIX_OK;
	}
	case TIX_V_STR:
		v->s = dup_str(o, fi->key);
		return v->s ? TIX_OK : tix_error_set(err, TIX_ERR_INVALID, "%s must be a string", fi->key);
	case TIX_V_ID:
		if (!get_id(o, fi->key, algo, &v->id))
			return tix_error_set(err, TIX_ERR_INVALID, "bad %s id", fi->key);
		return TIX_OK;
	case TIX_V_NULL:
		break;
	}
	return tix_error_set(err, TIX_ERR_INVALID, "bad field");
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
	p->type = 0;
	for (size_t i = 0; type && i < TIX_ENTITY_NTYPES; i++)
		if (!strcmp(type, tix_entity_type_name(tix_entity_types[i])))
			p->type = tix_entity_types[i];
	if (!p->type) {
		tix_error_set(err, TIX_ERR_INVALID, "bad entity type");
		goto out;
	}
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
	case TIX_OP_SET_FIELD: return get_value(j, op->u.set.field, algo, &op->u.set.v, err);
	case TIX_OP_DEP_ADD: {
		int64_t lag;
		s = get_str(j, "kind");
		if (!get_id(j, "pred", algo, &op->u.dep_add.pred) || !s || !tix_dep_kind_parse(s, &op->u.dep_add.kind) ||
		    !get_int(j, "lag", -TIX_LAG_MAX, TIX_LAG_MAX, &lag))
			return tix_error_set(err, TIX_ERR_INVALID, "bad dep-add");
		op->u.dep_add.lag = (int32_t)lag;
		return TIX_OK;
	}
	case TIX_OP_DEP_REMOVE:
		return get_id(j, "pred", algo, &op->u.unref.target) ? TIX_OK
								     : tix_error_set(err, TIX_ERR_INVALID, "bad pred");
	case TIX_OP_UNASSIGN:
		return get_id(j, "resource", algo, &op->u.unref.target)
			       ? TIX_OK
			       : tix_error_set(err, TIX_ERR_INVALID, "bad resource");
	case TIX_OP_ASSIGN: {
		int64_t units;
		const cJSON *ef = cJSON_GetObjectItemCaseSensitive(j, "effort");
		if (!get_id(j, "resource", algo, &op->u.assign.resource) ||
		    !get_int(j, "units", 1, TIX_UNITS_MAX, &units))
			return tix_error_set(err, TIX_ERR_INVALID, "bad assign");
		op->u.assign.units = (int32_t)units;
		if (cJSON_IsNull(ef))
			op->u.assign.effort = -1;
		else if (!get_int(j, "effort", 0, TIX_EFFORT_MAX, &op->u.assign.effort))
			return tix_error_set(err, TIX_ERR_INVALID, "bad assignment effort");
		return TIX_OK;
	}
	case TIX_OP_RESOURCE_CREATE: {
		int k;
		op->u.resource_create.nonce = dup_str(j, "nonce");
		op->u.resource_create.name = dup_str(j, "name");
		s = get_str(j, "kind");
		if (!s || (k = tix_field_enum(TIX_F_RES_KIND, s)) < 0)
			return tix_error_set(err, TIX_ERR_INVALID, "bad resource kind");
		op->u.resource_create.kind = (tix_resource_kind)k;
		return TIX_OK;
	}
	case TIX_OP_CALENDAR_CREATE:
		op->u.calendar_create.nonce = dup_str(j, "nonce");
		op->u.calendar_create.name = dup_str(j, "name");
		return TIX_OK;
	case TIX_OP_EXCEPTION_ADD: {
		const cJSON *wk = cJSON_GetObjectItemCaseSensitive(j, "working");
		if (!get_date(j, "from", &op->u.exception_add.from) || !get_date(j, "to", &op->u.exception_add.to) ||
		    !cJSON_IsBool(wk))
			return tix_error_set(err, TIX_ERR_INVALID, "bad exception");
		op->u.exception_add.working = cJSON_IsTrue(wk);
		op->u.exception_add.note = dup_str(j, "note");
		return TIX_OK;
	}
	case TIX_OP_EXCEPTION_REMOVE:
		if (!get_date(j, "from", &op->u.exception_remove.from) || !get_date(j, "to", &op->u.exception_remove.to))
			return tix_error_set(err, TIX_ERR_INVALID, "bad exception");
		return TIX_OK;
	case TIX_OP_BASELINE_CREATE:
		op->u.baseline_create.nonce = dup_str(j, "nonce");
		op->u.baseline_create.name = dup_str(j, "name");
		if (!get_date(j, "taken", &op->u.baseline_create.taken) ||
		    !get_id_or_null(j, "root", algo, &op->u.baseline_create.root))
			return tix_error_set(err, TIX_ERR_INVALID, "bad baseline");
		return TIX_OK;
	case TIX_OP_BASELINE_SET_NAME: op->u.text.value = dup_str(j, "name"); return TIX_OK;
	case TIX_OP_BASELINE_TASK: {
		tix_baseline_task_op *b = &op->u.baseline_task;
		const cJSON *cur = cJSON_GetObjectItemCaseSensitive(j, "currency");
		if (!get_id(j, "ticket", algo, &b->ticket) || !get_date_or_null(j, "start", &b->start) ||
		    !get_date_or_null(j, "finish", &b->finish) ||
		    !get_int_or_null(j, "duration", TIX_DURATION_MAX, &b->duration) ||
		    !get_int_or_null(j, "effort", TIX_EFFORT_MAX, &b->effort) ||
		    !get_int_or_null(j, "cost", TIX_COST_MAX, &b->cost) || !(cJSON_IsNull(cur) || cJSON_IsString(cur)))
			return tix_error_set(err, TIX_ERR_INVALID, "bad baseline task");
		b->currency = cJSON_IsString(cur) ? tix_xstrdup(cur->valuestring) : NULL;
		return TIX_OK;
	}
	case TIX_OP_SET_STATE: {
		const cJSON *sg = cJSON_GetObjectItemCaseSensitive(j, "stage");
		s = get_str(j, "status");
		if (!s || !tix_ticket_status_parse(s, &op->u.set_state.status) || !(cJSON_IsNull(sg) || cJSON_IsString(sg)))
			return tix_error_set(err, TIX_ERR_INVALID, "bad set-state");
		op->u.set_state.stage = cJSON_IsString(sg) ? tix_xstrdup(sg->valuestring) : NULL;
		return TIX_OK;
	}
	case TIX_OP_STAGE_ADD: {
		int64_t rank;
		const cJSON *hu = cJSON_GetObjectItemCaseSensitive(j, "human");
		op->u.stage_add.name = dup_str(j, "name");
		s = get_str(j, "phase");
		if (!s || !tix_ticket_status_parse(s, &op->u.stage_add.phase) || !cJSON_IsBool(hu) ||
		    !get_int(j, "rank", 0, TIX_STAGE_RANK_MAX, &rank))
			return tix_error_set(err, TIX_ERR_INVALID, "bad stage-add");
		op->u.stage_add.human = cJSON_IsTrue(hu);
		op->u.stage_add.rank = (int32_t)rank;
		return TIX_OK;
	}
	case TIX_OP_STAGE_REMOVE: op->u.stage_remove.name = dup_str(j, "name"); return TIX_OK;
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
	tix_field field;
	if (!n && field_of_name(type, first->valuestring, &field)) {
		op->kind = TIX_OP_SET_FIELD;
		op->u.set.field = field;
	} else if (!n) { /* forward compatibility: keep verbatim, ignore */
		op->kind = TIX_OP_UNKNOWN;
		op->u.unknown.name = tix_xstrdup(first->valuestring);
		op->u.unknown.raw = tix_xstrndup(line, len);
		st = TIX_OK;
		goto out;
	}
	else
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

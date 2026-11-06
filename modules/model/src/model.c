/* Names, ids and op constructors. */
#include "tix/model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *tix_entity_type_name(tix_entity_type t)
{
	switch (t) {
	case TIX_ENTITY_IDENTITY: return "identity";
	case TIX_ENTITY_TICKET: return "ticket";
	}
	TIX_REQUIRE(!"unknown entity type");
	return NULL;
}

const char *tix_entity_ref_prefix(tix_entity_type t)
{
	switch (t) {
	case TIX_ENTITY_IDENTITY: return "refs/tix/identities/";
	case TIX_ENTITY_TICKET: return "refs/tix/tickets/";
	}
	TIX_REQUIRE(!"unknown entity type");
	return NULL;
}

int tix_opid_cmp(const tix_opid *a, const tix_opid *b)
{
	int c = tix_oid_cmp(&a->commit, &b->commit);
	if (c)
		return c;
	return a->line < b->line ? -1 : a->line > b->line;
}

char *tix_opid_str(const tix_opid *id, char *out)
{
	char hex[TIX_OID_MAX_HEX + 1];
	snprintf(out, TIX_OID_MAX_HEX + 12, "%s:%u", tix_oid_hex(&id->commit, hex), (unsigned)id->line);
	return out;
}

bool tix_opid_parse(tix_opid *out, const char *s)
{
	const char *colon = strchr(s, ':');
	tix_opid id;
	if (!colon || !tix_oid_from_hex(&id.commit, s, (size_t)(colon - s)))
		return false;
	const char *p = colon + 1;
	if (*p < '1' || *p > '9')
		return false; /* no sign, no leading zero */
	uint64_t v = 0;
	for (; *p; p++) {
		if (*p < '0' || *p > '9')
			return false;
		v = v * 10 + (uint64_t)(*p - '0');
		if (v > TIX_PACK_MAX_BYTES)
			return false;
	}
	if (v < 2)
		return false; /* line 1 is the header */
	id.line = (uint32_t)v;
	*out = id;
	return true;
}

static const char *const status_names[] = {"open", "in-progress", "blocked", "done", "cancelled"};

const char *tix_ticket_status_name(tix_ticket_status s)
{
	TIX_REQUIRE((unsigned)s < sizeof(status_names) / sizeof(*status_names));
	return status_names[s];
}

bool tix_ticket_status_parse(const char *s, tix_ticket_status *out)
{
	for (unsigned i = 0; i < sizeof(status_names) / sizeof(*status_names); i++) {
		if (!strcmp(s, status_names[i])) {
			*out = (tix_ticket_status)i;
			return true;
		}
	}
	return false;
}

void tix_op_free(tix_op *op)
{
	switch (op->kind) {
	case TIX_OP_TICKET_CREATE:
		free(op->u.ticket_create.nonce);
		free(op->u.ticket_create.title);
		free(op->u.ticket_create.body);
		tix_strs_free(&op->u.ticket_create.labels);
		break;
	case TIX_OP_IDENTITY_CREATE:
		free(op->u.identity_create.nonce);
		free(op->u.identity_create.name);
		free(op->u.identity_create.email);
		break;
	case TIX_OP_SET_TITLE:
	case TIX_OP_SET_BODY:
	case TIX_OP_COMMENT:
	case TIX_OP_SET_NAME:
	case TIX_OP_SET_EMAIL:
		free(op->u.text.value);
		break;
	case TIX_OP_LABEL:
		tix_strs_free(&op->u.label.add);
		tix_strs_free(&op->u.label.remove);
		break;
	case TIX_OP_EDIT_COMMENT:
		free(op->u.edit_comment.body);
		break;
	case TIX_OP_KEY_ADD:
		free(op->u.key_add.kind);
		free(op->u.key_add.fingerprint);
		free(op->u.key_add.pubkey);
		break;
	case TIX_OP_KEY_REMOVE:
		free(op->u.key_remove.fingerprint);
		break;
	case TIX_OP_UNKNOWN:
		free(op->u.unknown.name);
		free(op->u.unknown.raw);
		break;
	case TIX_OP_SET_STATUS:
		break;
	}
	memset(op, 0, sizeof(*op));
}

static tix_op op_of(tix_op_kind k)
{
	tix_op op;
	memset(&op, 0, sizeof(op));
	op.kind = k;
	return op;
}

tix_op tix_op_ticket_create(const char *nonce, const char *title, const char *body, const tix_strs *labels)
{
	tix_op op = op_of(TIX_OP_TICKET_CREATE);
	op.u.ticket_create.nonce = tix_xstrdup(nonce);
	op.u.ticket_create.title = tix_xstrdup(title);
	op.u.ticket_create.body = tix_xstrdup(body ? body : "");
	if (labels)
		tix_strs_copy(&op.u.ticket_create.labels, labels);
	return op;
}

tix_op tix_op_identity_create(const char *nonce, const char *name, const char *email)
{
	tix_op op = op_of(TIX_OP_IDENTITY_CREATE);
	op.u.identity_create.nonce = tix_xstrdup(nonce);
	op.u.identity_create.name = tix_xstrdup(name);
	op.u.identity_create.email = tix_xstrdup(email);
	return op;
}

tix_op tix_op_text(tix_op_kind kind, const char *value)
{
	TIX_REQUIRE(kind == TIX_OP_SET_TITLE || kind == TIX_OP_SET_BODY || kind == TIX_OP_COMMENT ||
		    kind == TIX_OP_SET_NAME || kind == TIX_OP_SET_EMAIL);
	tix_op op = op_of(kind);
	op.u.text.value = tix_xstrdup(value);
	return op;
}

tix_op tix_op_set_status(tix_ticket_status s)
{
	tix_op op = op_of(TIX_OP_SET_STATUS);
	op.u.set_status.status = s;
	return op;
}

tix_op tix_op_label(const tix_strs *add, const tix_strs *remove)
{
	tix_op op = op_of(TIX_OP_LABEL);
	if (add)
		tix_strs_copy(&op.u.label.add, add);
	if (remove)
		tix_strs_copy(&op.u.label.remove, remove);
	return op;
}

tix_op tix_op_edit_comment(const tix_opid *target, const char *body)
{
	tix_op op = op_of(TIX_OP_EDIT_COMMENT);
	op.u.edit_comment.target = *target;
	op.u.edit_comment.body = tix_xstrdup(body);
	return op;
}

tix_op tix_op_key_add(const char *kind, const char *fingerprint, const char *pubkey)
{
	tix_op op = op_of(TIX_OP_KEY_ADD);
	op.u.key_add.kind = tix_xstrdup(kind);
	op.u.key_add.fingerprint = tix_xstrdup(fingerprint);
	op.u.key_add.pubkey = tix_xstrdup(pubkey);
	return op;
}

tix_op tix_op_key_remove(const char *fingerprint)
{
	tix_op op = op_of(TIX_OP_KEY_REMOVE);
	op.u.key_remove.fingerprint = tix_xstrdup(fingerprint);
	return op;
}

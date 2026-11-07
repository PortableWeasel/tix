/* Activity feeds: ticket ops in total order (architecture §15.6). */
#include "core_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tix_activity_list_free(tix_activity_list *l)
{
	for (size_t i = 0; i < l->n; i++) {
		free(l->v[i].title);
		free(l->v[i].op);
		free(l->v[i].summary);
	}
	free(l->v);
	memset(l, 0, sizeof(*l));
}

/* The first line of s, at most 72 bytes (cut on a UTF-8 boundary). */
static void first_line(tix_buf *b, const char *s)
{
	size_t n = strcspn(s, "\n");
	if (n > 72) {
		n = 72;
		while (n && ((unsigned char)s[n] & 0xc0) == 0x80)
			n--;
	}
	tix_buf_add(b, s, n);
	if (s[n] && s[n] != '\n')
		tix_buf_adds(b, "...");
}

static char *summarize(const tix_op *op)
{
	tix_buf b = TIX_BUF_INIT;
	char st[TIX_STATE_STR], hex[TIX_OID_MAX_HEX + 1];
	switch (op->kind) {
	case TIX_OP_TICKET_CREATE:
		tix_buf_adds(&b, "created: ");
		first_line(&b, op->u.ticket_create.title);
		break;
	case TIX_OP_SET_TITLE:
		tix_buf_adds(&b, "title: ");
		first_line(&b, op->u.text.value);
		break;
	case TIX_OP_SET_BODY: tix_buf_adds(&b, "description changed"); break;
	case TIX_OP_SET_STATUS:
		tix_buf_addf(&b, "status %s", tix_ticket_status_name(op->u.set_status.status));
		break;
	case TIX_OP_SET_STATE:
		tix_buf_addf(&b, "status %s", tix_state_str(op->u.set_state.status, op->u.set_state.stage, st));
		break;
	case TIX_OP_LABEL:
		for (size_t i = 0; i < op->u.label.add.n; i++)
			tix_buf_addf(&b, "%s+%s", b.len ? " " : "labels ", op->u.label.add.v[i]);
		for (size_t i = 0; i < op->u.label.remove.n; i++)
			tix_buf_addf(&b, "%s-%s", b.len ? " " : "labels ", op->u.label.remove.v[i]);
		break;
	case TIX_OP_COMMENT:
		tix_buf_adds(&b, "comment: ");
		first_line(&b, op->u.text.value);
		break;
	case TIX_OP_TYPED_COMMENT:
		tix_buf_addf(&b, "[%s] ", op->u.typed_comment.kind);
		first_line(&b, op->u.typed_comment.body);
		break;
	case TIX_OP_EDIT_COMMENT: tix_buf_adds(&b, "edited a comment"); break;
	case TIX_OP_CLAIM: tix_buf_adds(&b, "claimed"); break;
	case TIX_OP_RELEASE: tix_buf_adds(&b, "released"); break;
	case TIX_OP_SET_AUTONOMY:
		tix_buf_addf(&b, "autonomy %s", tix_autonomy_mode_name(op->u.set_autonomy.mode));
		break;
	case TIX_OP_STAGE_ADD:
		tix_buf_addf(&b, "stage %s defined",
			     tix_state_str(op->u.stage_add.phase, op->u.stage_add.name, st));
		break;
	case TIX_OP_STAGE_REMOVE: tix_buf_addf(&b, "stage %s removed", op->u.stage_remove.name); break;
	case TIX_OP_DEP_ADD:
		tix_buf_addf(&b, "after %.7s", tix_oid_hex(&op->u.dep_add.pred, hex));
		break;
	case TIX_OP_DEP_REMOVE:
		tix_buf_addf(&b, "no longer after %.7s", tix_oid_hex(&op->u.unref.target, hex));
		break;
	case TIX_OP_SET_FIELD:
		if (op->u.set.field == TIX_F_PARENT && op->u.set.v.kind == TIX_V_ID)
			tix_buf_addf(&b, "moved under %.7s", tix_oid_hex(&op->u.set.v.id, hex));
		else
			tix_buf_addf(&b, "%s changed", tix_field_get(op->u.set.field)->key);
		break;
	default: tix_buf_adds(&b, tix_op_wire_name(op)); break;
	}
	return tix_buf_detach(&b);
}

static int cmp_activity(const void *pa, const void *pb)
{
	const tix_activity *a = pa, *b = pb;
	if (a->lamport != b->lamport)
		return a->lamport < b->lamport ? -1 : 1;
	return tix_opid_cmp(&a->id, &b->id);
}

static tix_status ticket_ops(tix_repo *r, const tix_entity *t, const tix_activity_query *q, tix_activity_list *out,
			     size_t *cap, tix_error *err)
{
	core_dag d = {0}, old = {0};
	core_oidset seen = {0};
	tix_status st = core_dag_load(r, &t->head, &d, err);
	if (!st && q->after && !tix_oid_is_null(q->after) && !(st = core_dag_load(r, q->after, &old, err)))
		for (size_t i = 0; i < old.commits.n; i++)
			core_oidset_add(&seen, &old.commits.v[i].oid);
	for (size_t i = 0; !st && i < d.commits.n; i++) {
		const tix_commit *c = &d.commits.v[i];
		if (core_oidset_has(&seen, &c->oid))
			continue;
		tix_pack p;
		if (tix_pack_decode(c->pack.data, c->pack.len, r->store->algo, &p, NULL))
			continue; /* the entity validated, so this doesn't happen */
		for (size_t k = 0; k < p.nops; k++) {
			if (p.time < q->since || p.ops[k].kind == TIX_OP_UNKNOWN)
				continue;
			if (out->n == *cap) {
				*cap = *cap ? *cap * 2 : 64;
				out->v = tix_xrealloc(out->v, *cap * sizeof(*out->v));
			}
			tix_activity *a = &out->v[out->n++];
			a->time = p.time;
			a->lamport = p.lamport;
			a->id.commit = c->oid;
			a->id.line = (uint32_t)(k + 2);
			a->ticket = t->id;
			a->author = p.author;
			a->title = tix_xstrdup(t->u.ticket.title);
			a->op = tix_xstrdup(tix_op_wire_name(&p.ops[k]));
			a->summary = summarize(&p.ops[k]);
		}
		tix_pack_free(&p);
	}
	core_oidset_free(&seen);
	core_dag_free(&old);
	core_dag_free(&d);
	return st;
}

tix_status tix_activity_log(tix_repo *r, const tix_activity_query *q, tix_activity_list *out, tix_error *err)
{
	TIX_REQUIRE(q && out);
	memset(out, 0, sizeof(*out));
	tix_entity_list l = {0};
	tix_status st;
	if (q->ticket) {
		tix_entity *e;
		if ((st = tix_entity_load(r, TIX_ENTITY_TICKET, q->ticket, &e, err)))
			return st;
		l.v = tix_xcalloc(1, sizeof(*l.v));
		l.v[l.n++] = e;
	} else {
		tix_ticket_query tq = {.under = q->under};
		if ((st = tix_ticket_list(r, &tq, &l, err)))
			return st;
		if (q->under) { /* the root itself too */
			tix_entity *e;
			if (!tix_entity_load(r, TIX_ENTITY_TICKET, q->under, &e, NULL)) {
				l.v = tix_xrealloc(l.v, (l.n + 1) * sizeof(*l.v));
				l.v[l.n++] = e;
			}
		}
	}
	size_t cap = 0;
	for (size_t i = 0; i < l.n && !st; i++)
		st = ticket_ops(r, l.v[i], q, out, &cap, err);
	tix_entity_list_free(&l);
	if (st) {
		tix_activity_list_free(out);
		return st;
	}
	if (out->n)
		qsort(out->v, out->n, sizeof(*out->v), cmp_activity);
	if (q->limit && out->n > q->limit) {
		size_t drop = out->n - q->limit;
		for (size_t i = 0; i < drop; i++) {
			free(out->v[i].title);
			free(out->v[i].op);
			free(out->v[i].summary);
		}
		memmove(out->v, out->v + drop, q->limit * sizeof(*out->v));
		out->n = q->limit;
	}
	return TIX_OK;
}

/*
 * /baselines: plan snapshots (architecture §14.2). Lists them with links that compare the
 * plan against each, takes a new one (POST /baselines) and renames (POST /baselines/ID).
 */
#include "app.h"

#include <stdio.h>
#include <string.h>

void page_baselines(web_ctx *x)
{
	tix_entity_list l;
	if (tix_baseline_list(x->app->cfg.repo, &l, &x->app->err)) {
		page_error(x, 500, "Repository problem", x->app->err.msg);
		return;
	}
	page_begin(x, "Baselines");
	html *h = &x->h;
	HL(h, "<p class=\"note\">A baseline is a snapshot of the plan: every task's dates, duration, effort and "
	      "cost as computed that day. It syncs like tickets. Compare the current plan with one to see what "
	      "slipped.</p>\n");
	if (!l.n) {
		HL(h, "<p>No baselines yet.</p>\n");
	} else {
		HL(h, "<table><thead><tr><th>Name</th><th>Taken</th><th class=\"num\">Tasks</th><th>Root</th><th></th>"
		      "</tr></thead><tbody>\n");
		for (size_t i = 0; i < l.n; i++) {
			const tix_entity *e = l.v[i];
			const tix_baseline *b = &e->u.baseline;
			HL(h, "<tr><td><a href=\"/plan?baseline=");
			h_id(h, &e->id);
			if (!tix_oid_is_null(&b->root)) {
				HL(h, "&amp;root=");
				h_id(h, &b->root);
			}
			HL(h, "\">");
			h_text(h, b->name);
			HL(h, "</a></td><td>");
			h_date(h, b->taken);
			HL(h, "</td><td class=\"num\">");
			h_int(h, (long long)b->ntasks);
			HL(h, "</td><td>");
			if (!tix_oid_is_null(&b->root))
				h_ticket(x, &b->root, NULL);
			else
				HL(h, "<span class=\"note\">whole plan</span>");
			HL(h, "</td><td>");
			if (web_can_write(x)) {
				HL(h, "<form class=\"inline\" method=\"post\" action=\"/baselines/");
				h_id(h, &e->id);
				HL(h, "\">");
				form_csrf(x);
				HL(h, "<input name=\"name\" size=\"18\" aria-label=\"New name\" value=\"");
				h_text(h, b->name);
				HL(h, "\"> <button>Rename</button></form>");
			}
			HL(h, "</td></tr>\n");
		}
		HL(h, "</tbody></table>\n");
	}
	if (web_can_write(x)) {
		HL(h, "<h2>Take a baseline</h2>\n<form method=\"post\" action=\"/baselines\">");
		form_csrf(x);
		HL(h, "<label>Name <input name=\"name\" required size=\"24\"></label> "
		      "<label>Root <input name=\"root\" size=\"12\" placeholder=\"whole plan\"></label> "
		      "<button>Take</button></form>\n");
	}
	page_end(x);
	tix_entity_list_free(&l);
}

static void refused(web_ctx *x, const char *msg)
{
	tix_buf_reset(&x->res->body);
	tix_buf_reset(&x->res->headers);
	x->res->status = 422;
	page_begin(x, "Not saved");
	HL(&x->h, "<p class=\"error\">");
	h_text(&x->h, msg);
	HL(&x->h, "</p>\n<p><a href=\"/baselines\">Back</a></p>\n");
	page_end(x);
}

/* POST /baselines name=... root=ID|"" */
void post_baseline_take(web_ctx *x)
{
	const char *name = web_kvs_get(&x->req->form, "name"), *root = web_kvs_get(&x->req->form, "root");
	if (!name || !name[strspn(name, " \t")]) {
		refused(x, "A baseline needs a name.");
		return;
	}
	tix_oid r, id;
	if (root && *root && tix_entity_resolve(x->app->cfg.repo, TIX_ENTITY_TICKET, root, &r, &x->app->err)) {
		refused(x, x->app->err.msg);
		return;
	}
	if (tix_baseline_take(x->app->cfg.repo, name, root && *root ? &r : NULL, page_today(x), &id, NULL,
			      &x->app->err)) {
		refused(x, x->app->err.msg);
		return;
	}
	char path[2 * TIX_OID_MAX_HEX + 40], hex[TIX_OID_MAX_HEX + 1], rhex[TIX_OID_MAX_HEX + 1];
	if (root && *root)
		snprintf(path, sizeof(path), "/plan?baseline=%s&root=%s", tix_oid_hex(&id, hex), tix_oid_hex(&r, rhex));
	else
		snprintf(path, sizeof(path), "/plan?baseline=%s", tix_oid_hex(&id, hex));
	web_redirect(x, path);
}

/* POST /baselines/ID name=... */
void post_baseline_rename(web_ctx *x)
{
	tix_oid id;
	if (!route_id(x, 1, TIX_ENTITY_BASELINE, &id))
		return;
	const char *name = web_kvs_get(&x->req->form, "name");
	if (!name || !*name) {
		refused(x, "A baseline needs a name.");
		return;
	}
	tix_op op = tix_op_text(TIX_OP_BASELINE_SET_NAME, name);
	tix_status st = tix_entity_apply(x->app->cfg.repo, TIX_ENTITY_BASELINE, &id, &op, 1, &x->app->err);
	tix_op_free(&op);
	if (st) {
		refused(x, x->app->err.msg);
		return;
	}
	web_redirect(x, "/baselines");
}

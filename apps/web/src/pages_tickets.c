/* Ticket pages. */
#include "app.h"

void page_home(web_ctx *x)
{
	tix_ticket_query q = {0};
	tix_entity_list l = {0};
	tix_status st = tix_ticket_list(x->app->cfg.repo, &q, &l, &x->app->err);
	if (st) {
		page_error(x, 500, "Repository problem", x->app->err.msg);
		return;
	}
	page_begin(x, "Tickets");
	html *h = &x->h;
	HL(h, "<table><thead><tr><th>ID</th><th>Title</th></tr></thead><tbody>\n");
	for (size_t i = 0; i < l.n; i++) {
		const tix_entity *e = l.v[i];
		HL(h, "<tr><td class=\"id\"><a href=\"/t/");
		h_id(h, &e->id);
		HL(h, "\">");
		h_short(h, &e->id);
		HL(h, "</a></td><td>");
		h_text(h, e->u.ticket.title);
		HL(h, "</td></tr>\n");
	}
	HL(h, "</tbody></table>\n");
	page_end(x);
	tix_entity_list_free(&l);
}

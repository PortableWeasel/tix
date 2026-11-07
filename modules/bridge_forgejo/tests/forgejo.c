/* The Forgejo adapter against fake_forgejo.py, which starts this binary with the URL. */
#include "tix/bridge_forgejo.h"
#include "tix/testkit.h"

#include <string.h>

int main(int argc, char **argv)
{
	TIX_REQUIRE(argc == 2);
	tix_error e = TIX_ERROR_INIT;
	tix_buf sc = TIX_BUF_INIT, h = TIX_BUF_INIT, o = TIX_BUF_INIT, r = TIX_BUF_INIT;
	TK_CHECK(tix_bridge_forgejo_parse("https://git.example.org/me/proj.git/", &sc, &h, &o, &r));
	TK_STR_EQ(tix_buf_cstr(&h), "git.example.org");
	TK_STR_EQ(tix_buf_cstr(&r), "proj");
	TK_CHECK(!tix_bridge_forgejo_parse("http://git.example.org/me/proj", &sc, &h, &o, &r)); /* plain http, not local */
	TK_CHECK(!tix_bridge_forgejo_parse("https://h/me/proj/issues", &sc, &h, &o, &r));
	TK_CHECK(!tix_bridge_forgejo_parse("https://h/../x", &sc, &h, &o, &r));
	TK_CHECK(!tix_bridge_forgejo_parse("https://h@evil/me/x", &sc, &h, &o, &r));
	TK_CHECK(!tix_bridge_forgejo_parse("file:///etc/passwd", &sc, &h, &o, &r));
	tix_buf_free(&sc);
	tix_buf_free(&h);
	tix_buf_free(&o);
	tix_buf_free(&r);
	tix_tracker *t = NULL;
	TK_STATUS(tix_bridge_forgejo_open(argv[1], "bad token", &t, &e), TIX_ERR_INVALID, &e);
	TK_OK(tix_bridge_forgejo_open(argv[1], "s3cret", &t, &e), &e);
	if (!t)
		return tk_finish();
	TK_CHECK(!strncmp(t->key, "forgejo:127.0.0.1:", 18) && strstr(t->key, "/me/proj"));

	tix_strs labels = {0};
	TK_OK(t->vt->labels(t, &labels, &e), &e);
	TK_CHECK(labels.n == 2 && tix_strs_contains(&labels, "bug"));
	tix_issues is = {0};
	TK_OK(t->vt->list(t, &is, &e), &e);
	TK_CHECK(is.n == 61); /* two pages; the pull request and the malformed entry are skipped */
	const tix_issue *first = NULL, *marked = NULL;
	for (size_t i = 0; i < is.n; i++) {
		if (is.v[i].number == 1)
			first = &is.v[i];
		if (is.v[i].marker)
			marked = &is.v[i];
	}
	TK_CHECK(first && !strcmp(first->title, "First") && tix_strs_contains(&first->labels, "bug") && !first->closed);
	TK_CHECK(marked && !strcmp(marked->marker, "0123456789abcdef") && !strcmp(marked->body, "made by tix"));

	tix_strs want = {0};
	tix_strs_add(&want, "bug");
	tix_strs_add(&want, "unknown-label");
	int64_t n = 0;
	tix_issue_update in = {.title = "New \"one\"", .body = "text", .marker = "abcdef0123456", .set_labels = true,
			       .labels = &want};
	TK_OK(t->vt->create(t, &in, &n, &e), &e);
	TK_CHECK(n == 100);
	tix_issue_update up = {.body = "edited", .set_closed = true, .closed = true, .set_labels = true, .labels = &want};
	TK_OK(t->vt->update(t, n, &up, &e), &e);
	tix_issue_comments cs = {0};
	TK_OK(t->vt->comments(t, 1, &cs, &e), &e);
	TK_CHECK(cs.n == 2 && !strcmp(cs.v[0].author, "bob") && !cs.v[0].marker && cs.v[1].marker &&
		 !strcmp(cs.v[1].marker, "abc1234:3"));
	tix_issue_comments_free(&cs);
	int64_t cid = 0;
	TK_OK(t->vt->comment(t, 1, "from tix", "abc1234:4", &cid, &e), &e);
	TK_CHECK(cid == 500);
	TK_STATUS(t->vt->update(t, 404, &up, &e), TIX_ERR_NOT_FOUND, &e);
	TK_CHECK(strstr(e.msg, "HTTP 404") && strstr(e.msg, "no such issue") && !strstr(e.msg, "s3cret"));
	TK_STATUS(t->vt->comments(t, 666, &cs, &e), TIX_ERR_IO, &e); /* the server redirects: not followed */
	TK_STATUS(t->vt->comments(t, 777, &cs, &e), TIX_ERR_INVALID, &e); /* not JSON */
	tix_issue_comments_free(&cs);
	tix_strs_free(&want);
	tix_strs_free(&labels);
	tix_issues_free(&is);
	tix_tracker_destroy(t);
	return tk_finish();
}

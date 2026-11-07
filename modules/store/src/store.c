#include "tix/store.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void tix_commit_free(tix_commit *c)
{
	free(c->parents);
	tix_buf_free(&c->pack);
	free(c->message);
	memset(c, 0, sizeof(*c));
}

void tix_refs_free(tix_refs *r)
{
	for (size_t i = 0; i < r->n; i++)
		free(r->v[i].name);
	free(r->v);
	r->v = NULL;
	r->n = 0;
}

static bool component_ok(const char *c, size_t n)
{
	if (n == 0 || c[0] == '.')
		return false;
	if (n >= 5 && !memcmp(c + n - 5, ".lock", 5))
		return false;
	for (size_t i = 0; i < n; i++) {
		char ch = c[i];
		bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
			  (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
		if (!ok || (ch == '.' && i + 1 < n && c[i + 1] == '.'))
			return false;
	}
	return true;
}

bool tix_refname_valid(const char *name)
{
	size_t len = strlen(name);
	if (len > 255 || strncmp(name, "refs/", 5))
		return false;
	const char *p = name + 5;
	for (;;) {
		const char *slash = strchr(p, '/');
		size_t n = slash ? (size_t)(slash - p) : strlen(p);
		if (!component_ok(p, n))
			return false;
		if (!slash)
			return true;
		p = slash + 1;
	}
}

bool tix_local_name_valid(const char *name)
{
	size_t n = strlen(name);
	if (n < 1 || n > 32)
		return false;
	for (; *name; name++)
		if (!((*name >= 'a' && *name <= 'z') || (*name >= '0' && *name <= '9') || *name == '-'))
			return false;
	return true;
}

void tix_remote_mirror_prefix(const char *remote, char *out, size_t n)
{
	TIX_REQUIRE(remote && out && tix_remote_name_valid(remote));
	int k = snprintf(out, n, "%s%s/", TIX_REMOTE_MIRROR_ROOT, remote);
	TIX_REQUIRE(k > 0 && (size_t)k < n);
}

bool tix_remote_name_valid(const char *name)
{
	size_t n = strlen(name);
	if (n < 1 || n > 64 || name[0] == '-' || !component_ok(name, n))
		return false;
	return true;
}

/* Embedded static files, looked up by exact name (no file system serving, section 13.6).
 * The sources are apps/web/assets/; cmake/Embed.cmake compiles them in. Names carry a
 * content hash, so they can be cached forever. */
#include "app.h"

#include <stdio.h>
#include <string.h>

extern const char tix_asset_css[], tix_asset_js[];
extern const size_t tix_asset_css_len, tix_asset_js_len;

typedef struct asset {
	const char *data;
	const size_t *len;
	const char *ext, *type;
	char name[32];
} asset;

static asset assets[] = {
	{tix_asset_css, &tix_asset_css_len, "css", "text/css; charset=utf-8", ""},
	{tix_asset_js, &tix_asset_js_len, "js", "text/javascript; charset=utf-8", ""},
};

static uint32_t fnv1a(const char *s, size_t n)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++)
		h = (h ^ (unsigned char)s[i]) * 16777619u;
	return h;
}

static const char *name_of(asset *a)
{
	if (!a->name[0])
		snprintf(a->name, sizeof(a->name), "tix-%08x.%s", fnv1a(a->data, *a->len), a->ext);
	return a->name;
}

const char *asset_css_name(void)
{
	return name_of(&assets[0]);
}

const char *asset_js_name(void)
{
	return name_of(&assets[1]);
}

bool asset_find(const char *name, const char **type, const char **body, size_t *len)
{
	for (size_t i = 0; i < sizeof(assets) / sizeof(*assets); i++)
		if (!strcmp(name, name_of(&assets[i]))) {
			*type = assets[i].type;
			*body = assets[i].data;
			*len = *assets[i].len;
			return true;
		}
	return false;
}

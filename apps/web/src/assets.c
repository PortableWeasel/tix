/* Embedded static files, looked up by exact name (no file system serving, section 13.6).
 * Names carry a content hash, so they can be cached forever. */
#include "app.h"

#include <stdio.h>
#include <string.h>

static const char css[] =
	":root{color-scheme:light dark;--fg:#1d1d1f;--bg:#fff;--muted:#6b6b70;--line:#d8d8dc;"
	"--accent:#1a5fb4;--crit:#c01c28;--ok:#26a269;--warn:#e5a50a;--soft:#f3f3f5}\n"
	"@media (prefers-color-scheme:dark){:root{--fg:#e8e8ea;--bg:#18181b;--muted:#a0a0a8;"
	"--line:#3a3a40;--accent:#78aeed;--crit:#f66151;--ok:#57e389;--warn:#f8e45c;--soft:#232328}}\n"
	"*{box-sizing:border-box}\n"
	"body{margin:0;font:15px/1.45 system-ui,-apple-system,'Segoe UI',sans-serif;color:var(--fg);"
	"background:var(--bg)}\n"
	"header{border-bottom:1px solid var(--line);padding:.5rem 1rem;display:flex;flex-wrap:wrap;"
	"gap:.5rem 1.5rem;align-items:center}\n"
	"nav a{margin-right:.8rem}\n"
	"main{padding:1rem;max-width:72rem}\n"
	"h1{font-size:1.4rem;margin:.2rem 0 1rem}\n"
	"h2{font-size:1.1rem;margin:1.5rem 0 .5rem}\n"
	"a{color:var(--accent)}\n"
	"code,.id{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:.92em}\n"
	"table{border-collapse:collapse;width:100%}\n"
	"th,td{text-align:left;padding:.3rem .5rem;border-bottom:1px solid var(--line);vertical-align:top}\n"
	"th{font-weight:600;color:var(--muted)}\n"
	"td.num,th.num{text-align:right;font-variant-numeric:tabular-nums}\n"
	"form{margin:.5rem 0}\n"
	"fieldset{border:1px solid var(--line);padding:.5rem 1rem;margin:1rem 0}\n"
	"label{display:inline-block;margin:.2rem 1rem .2rem 0}\n"
	"input,select,textarea,button{font:inherit}\n"
	"textarea{width:100%;min-height:8rem}\n"
	"button{cursor:pointer}\n"
	".note{color:var(--muted)}\n"
	".error{color:var(--crit);font-weight:600}\n"
	".ok{color:var(--ok)}\n"
	".crit{color:var(--crit)}\n"
	".status{display:inline-block;padding:0 .4rem;border-radius:.6rem;background:var(--soft);font-size:.85em}\n"
	".label{display:inline-block;padding:0 .4rem;border:1px solid var(--line);border-radius:.6rem;"
	"font-size:.85em;margin-right:.2rem}\n"
	".comment{border-left:3px solid var(--line);padding:.2rem .8rem;margin:.8rem 0}\n"
	".body{white-space:pre-wrap}\n"
	"svg.gantt{max-width:100%;height:auto;font-size:11px}\n"
	"svg.gantt text{fill:var(--fg)}\n"
	"svg.gantt .bar{fill:var(--accent)}\n"
	"svg.gantt .bar.crit{fill:var(--crit)}\n"
	"svg.gantt .bar.sum{fill:var(--muted)}\n"
	"svg.gantt .grid{stroke:var(--line)}\n"
	"svg.gantt .arrow{stroke:var(--muted);fill:none}\n"
	"@media print{header{display:none}}\n";

static char css_name[32];

static uint32_t fnv1a(const char *s, size_t n)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++)
		h = (h ^ (unsigned char)s[i]) * 16777619u;
	return h;
}

const char *asset_css_name(void)
{
	if (!css_name[0])
		snprintf(css_name, sizeof(css_name), "tix-%08x.css", fnv1a(css, sizeof(css) - 1));
	return css_name;
}

bool asset_find(const char *name, const char **type, const char **body, size_t *len)
{
	if (!strcmp(name, asset_css_name())) {
		*type = "text/css; charset=utf-8";
		*body = css;
		*len = sizeof(css) - 1;
		return true;
	}
	return false;
}

/* $EDITOR round trip for titles, bodies and comments. */
#include "cli.h"
#include "editor.h"

#include <stdlib.h>

static void editor_cmd(cli *c, tix_buf *out)
{
	const char *e = getenv("TIX_EDITOR");
	if (e && *e) {
		tix_buf_adds(out, e);
		return;
	}
	if (!tix_store_libgit_config_get(c->store, "core.editor", out, NULL) && out->len)
		return;
	tix_buf_reset(out);
	if (((e = getenv("VISUAL")) && *e) || ((e = getenv("EDITOR")) && *e))
		tix_buf_adds(out, e);
	else
		tix_buf_adds(out, "vi");
}

int cli_edit_text(cli *c, const char *template_text, tix_buf *out)
{
	tix_buf cmd = TIX_BUF_INIT;
	char why[256];
	editor_cmd(c, &cmd);
	int rc = tix_edit_text(cmd.data, template_text, out, why, sizeof(why));
	if (rc)
		fprintf(stderr, "tix: %s\n", why);
	tix_buf_free(&cmd);
	return rc ? EXIT_ERR : EXIT_OK;
}

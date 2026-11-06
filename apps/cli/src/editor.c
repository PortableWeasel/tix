/* $EDITOR round trip for titles, bodies and comments. */
#include "cli.h"

#include <errno.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

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

static int read_file(const char *path, tix_buf *out)
{
	FILE *f = fopen(path, "rb");
	char chunk[4096];
	size_t n;
	if (!f)
		return -1;
	while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
		tix_buf_add(out, chunk, n);
	int bad = ferror(f);
	fclose(f);
	return bad ? -1 : 0;
}

int cli_edit_text(cli *c, const char *template_text, tix_buf *out)
{
	const char *tmpdir = getenv("TMPDIR");
	tix_buf path = TIX_BUF_INIT, cmd = TIX_BUF_INIT, raw = TIX_BUF_INIT;
	int rc = EXIT_ERR;

	tix_buf_addf(&path, "%s/tix-edit-XXXXXX.md", tmpdir && *tmpdir ? tmpdir : "/tmp");
	int fd = mkstemps(path.data, 3);
	if (fd < 0) {
		fprintf(stderr, "tix: cannot create temp file: %s\n", strerror(errno));
		goto out;
	}
	size_t len = strlen(template_text);
	if (write(fd, template_text, len) != (ssize_t)len) {
		close(fd);
		fprintf(stderr, "tix: cannot write temp file\n");
		goto out_unlink;
	}
	close(fd);

	/* Like git: the editor setting is a shell snippet from the user's own config. */
	editor_cmd(c, &cmd);
	tix_buf_adds(&cmd, " \"$@\"");
	char *args[] = {"sh", "-c", cmd.data, "tix-editor", path.data, NULL};
	pid_t pid;
	int status;
	if (posix_spawn(&pid, "/bin/sh", NULL, NULL, args, environ) ||
	    waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) {
		fprintf(stderr, "tix: editor failed; nothing saved\n");
		goto out_unlink;
	}
	if (read_file(path.data, &raw)) {
		fprintf(stderr, "tix: cannot read back %s\n", path.data);
		goto out_unlink;
	}
	/* drop comment lines */
	tix_buf_reset(out);
	for (size_t i = 0; i < raw.len;) {
		const char *nl = memchr(raw.data + i, '\n', raw.len - i);
		size_t n = nl ? (size_t)(nl - (raw.data + i)) + 1 : raw.len - i;
		if (raw.data[i] != '#')
			tix_buf_add(out, raw.data + i, n);
		i += n;
	}
	rc = EXIT_OK;
out_unlink:
	unlink(path.data);
out:
	tix_buf_free(&path);
	tix_buf_free(&cmd);
	tix_buf_free(&raw);
	return rc;
}

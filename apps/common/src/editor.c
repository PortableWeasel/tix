#include "editor.h"

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

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

int tix_edit_text(const char *editor, const char *template_text, tix_buf *out, char *why, size_t why_len)
{
	const char *tmpdir = getenv("TMPDIR");
	tix_buf path = TIX_BUF_INIT, cmd = TIX_BUF_INIT, raw = TIX_BUF_INIT;
	int rc = -1;
	tix_buf_addf(&path, "%s/tix-edit-XXXXXX.md", tmpdir && *tmpdir ? tmpdir : "/tmp");
	int fd = mkstemps(path.data, 3);
	if (fd < 0) {
		snprintf(why, why_len, "cannot create temp file: %s", strerror(errno));
		goto out;
	}
	size_t len = strlen(template_text);
	if (write(fd, template_text, len) != (ssize_t)len) {
		close(fd);
		snprintf(why, why_len, "cannot write temp file");
		goto out_unlink;
	}
	close(fd);
	/* Like git: the editor setting is a shell snippet from the user's own config. */
	tix_buf_adds(&cmd, editor);
	tix_buf_adds(&cmd, " \"$@\"");
	char *args[] = {"sh", "-c", cmd.data, "tix-editor", path.data, NULL};
	pid_t pid;
	int status;
	if (posix_spawn(&pid, "/bin/sh", NULL, NULL, args, environ) || waitpid(pid, &status, 0) < 0 ||
	    !WIFEXITED(status) || WEXITSTATUS(status)) {
		snprintf(why, why_len, "editor failed; nothing saved");
		goto out_unlink;
	}
	if (read_file(path.data, &raw)) {
		snprintf(why, why_len, "cannot read back %s", path.data);
		goto out_unlink;
	}
	tix_buf_reset(out); /* drop comment lines */
	for (size_t i = 0; i < raw.len;) {
		const char *nl = memchr(raw.data + i, '\n', raw.len - i);
		size_t n = nl ? (size_t)(nl - (raw.data + i)) + 1 : raw.len - i;
		if (raw.data[i] != '#')
			tix_buf_add(out, raw.data + i, n);
		i += n;
	}
	rc = 0;
out_unlink:
	unlink(path.data);
out:
	tix_buf_free(&path);
	tix_buf_free(&cmd);
	tix_buf_free(&raw);
	return rc;
}

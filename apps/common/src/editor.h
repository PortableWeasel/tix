/* The $EDITOR round trip shared by the frontends (CLI, TUI). */
#ifndef TIX_APPS_EDITOR_H
#define TIX_APPS_EDITOR_H

#include "tix/base_buf.h"

/*
 * Writes template_text to a temp file, runs `cmd` on it (a shell snippet from the user's own
 * configuration, as git does: sh -c '<cmd> "$@"' with the file as $1), and reads it back
 * without lines starting with '#'. RETURNS 0, or -1 with a message in why.
 */
int tix_edit_text(const char *cmd, const char *template_text, tix_buf *out, char *why, size_t why_len);

#endif

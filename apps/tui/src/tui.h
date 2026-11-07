/* tix-tui (architecture §14.5): the terminal UI on top of core. */
#ifndef TIX_TUI_H
#define TIX_TUI_H

#include "tix/core.h"
#include "tix/input.h"

typedef struct tui_config {
	tix_repo *repo;       /* BORROWS */
	const char *editor;   /* shell snippet for $EDITOR round trips */
	tix_date (*today)(void);
} tui_config;

/* Runs until the user quits. RETURNS the process exit code. */
int tui_run(const tui_config *cfg);

#endif

/*
 * The TUI's view of the terminal (architecture §14.5). Only term_curses.c includes curses.
 * Text is UTF-8; it is clipped to display cells, and control characters and invalid bytes
 * are drawn as '?', so data from other clones can't send escape sequences to the terminal.
 */
#ifndef TIX_TUI_TERM_H
#define TIX_TUI_TERM_H

#include <stdbool.h>

enum {
	K_TIMEOUT = -1,
	K_UP = 0x110000, K_DOWN, K_LEFT, K_RIGHT, K_PGUP, K_PGDN, K_HOME, K_END,
	K_ENTER, K_ESC, K_BACKSPACE, K_RESIZE, K_TAB,
};

typedef enum { T_NORMAL, T_BOLD, T_SELECT, T_DIM, T_BAR, T_ERR, T_OK, T_CRIT, T_SUM } tattr;

bool term_init(void);
void term_end(void);
void term_size(int *rows, int *cols);
void term_clear(void);
/* RETURNS the cells written. */
int term_text(int row, int col, int width, tattr a, const char *utf8);
void term_fill(int row, int col, int width, tattr a, char ch);
void term_cursor(int row, int col, bool visible);
void term_flush(void);
/* A Unicode code point, a K_* key, or K_TIMEOUT after timeout_ms (-1 = wait). */
int term_key(int timeout_ms);
/* Around running $EDITOR. */
void term_suspend(void);
void term_resume(void);
/* Display cells of a UTF-8 string (same rules as term_text). */
int term_width(const char *utf8);

#endif

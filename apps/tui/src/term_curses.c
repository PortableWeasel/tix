/* The only file that includes curses (ncursesw). */
#define _XOPEN_SOURCE_EXTENDED 1
#include "term.h"

#include <curses.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static bool colors;

bool term_init(void)
{
	setlocale(LC_ALL, "");
	if (!initscr())
		return false;
	cbreak();
	noecho();
	nonl();
	keypad(stdscr, TRUE);
	set_escdelay(25);
	typeahead(-1); /* always finish a redraw: with keys arriving in a burst (typing fast, pasting)
			  ncurses would otherwise abandon it and leave a half-drawn screen */
	curs_set(0);
	if (has_colors()) {
		start_color();
		use_default_colors();
		init_pair(1, COLOR_BLACK, COLOR_CYAN);  /* bars */
		init_pair(2, COLOR_RED, -1);            /* errors, critical */
		init_pair(3, COLOR_GREEN, -1);          /* ok */
		init_pair(4, COLOR_BLUE, -1);           /* summary bars */
		colors = true;
	}
	return true;
}

void term_end(void)
{
	endwin();
}

void term_size(int *rows, int *cols)
{
	getmaxyx(stdscr, *rows, *cols);
}

void term_clear(void)
{
	erase();
}

static attr_t attr_of(tattr a)
{
	switch (a) {
	case T_BOLD: return A_BOLD;
	case T_SELECT: return A_REVERSE;
	case T_DIM: return A_DIM;
	case T_BAR: return colors ? COLOR_PAIR(1) : A_REVERSE;
	case T_ERR: return colors ? COLOR_PAIR(2) | A_BOLD : A_BOLD;
	case T_OK: return colors ? COLOR_PAIR(3) : A_NORMAL;
	case T_CRIT: return colors ? COLOR_PAIR(2) : A_BOLD;
	case T_SUM: return colors ? COLOR_PAIR(4) | A_BOLD : A_BOLD;
	default: return A_NORMAL;
	}
}

/* Next code point; invalid sequences and control characters come back as '?'. */
static wchar_t next_char(const char **s)
{
	mbstate_t st;
	memset(&st, 0, sizeof(st));
	wchar_t w;
	size_t n = mbrtowc(&w, *s, MB_CUR_MAX, &st);
	if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
		(*s)++;
		return L'?';
	}
	*s += n;
	if (w < 0x20 || w == 0x7f || (w >= 0x80 && w < 0xa0) || w == 0x2028 || w == 0x2029 ||
	    (w >= 0x202a && w <= 0x202e) || (w >= 0x2066 && w <= 0x2069))
		return L'?'; /* controls, line separators and bidi overrides */
	return w;
}

int term_width(const char *utf8)
{
	int cells = 0;
	for (const char *s = utf8 ? utf8 : ""; *s;) {
		int w = wcwidth(next_char(&s));
		cells += w < 0 ? 1 : w;
	}
	return cells;
}

int term_text(int row, int col, int width, tattr a, const char *utf8)
{
	int used = 0;
	attr_set(attr_of(a), 0, NULL);
	for (const char *s = utf8 ? utf8 : ""; *s && used < width;) {
		wchar_t w = next_char(&s);
		int cw = wcwidth(w);
		if (cw < 0) {
			w = L'?';
			cw = 1;
		}
		if (used + cw > width)
			break;
		wchar_t one[2] = {w, 0};
		mvaddnwstr(row, col + used, one, 1);
		used += cw;
	}
	attr_set(A_NORMAL, 0, NULL);
	return used;
}

void term_fill(int row, int col, int width, tattr a, char ch)
{
	attr_set(attr_of(a), 0, NULL);
	for (int i = 0; i < width; i++)
		mvaddch(row, col + i, (chtype)(unsigned char)ch);
	attr_set(A_NORMAL, 0, NULL);
}

void term_cursor(int row, int col, bool visible)
{
	curs_set(visible ? 1 : 0);
	if (visible)
		move(row, col);
}

void term_flush(void)
{
	refresh();
}

int term_key(int timeout_ms)
{
	timeout(timeout_ms);
	wint_t ch;
	int r = get_wch(&ch);
	if (r == ERR)
		return K_TIMEOUT;
	if (r == KEY_CODE_YES) {
		switch (ch) {
		case KEY_UP: return K_UP;
		case KEY_DOWN: return K_DOWN;
		case KEY_LEFT: return K_LEFT;
		case KEY_RIGHT: return K_RIGHT;
		case KEY_PPAGE: return K_PGUP;
		case KEY_NPAGE: return K_PGDN;
		case KEY_HOME: return K_HOME;
		case KEY_END: return K_END;
		case KEY_ENTER: return K_ENTER;
		case KEY_BACKSPACE: return K_BACKSPACE;
		case KEY_RESIZE: return K_RESIZE;
		default: return K_TIMEOUT;
		}
	}
	switch (ch) {
	case '\r':
	case '\n': return K_ENTER;
	case 27: return K_ESC;
	case 127:
	case 8: return K_BACKSPACE;
	case '\t': return K_TAB;
	default: return (int)ch;
	}
}

void term_suspend(void)
{
	def_prog_mode();
	endwin();
}

void term_resume(void)
{
	reset_prog_mode();
	refresh();
}

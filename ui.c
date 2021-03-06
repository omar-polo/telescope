/*
 * Copyright (c) 2021 Omar Polo <op@omarpolo.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Ncurses UI for telescope.
 *
 * Text scrolling
 * ==============
 *
 * ncurses allows you to scroll a window, but when a line goes out of
 * the visible area it's forgotten.  We keep a list of formatted lines
 * (``visual lines'') that we know fits in the window, and draw them.
 *
 * This means that on every resize we have to clear our list of lines
 * and re-render everything.  A clever approach would be to do this
 * ``on-demand'', but it's still missing.
 *
 */

#include "compat.h"

#include <assert.h>
#include <curses.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "defaults.h"
#include "minibuffer.h"
#include "session.h"
#include "telescope.h"
#include "ui.h"
#include "utf8.h"

static struct event	stdioev, winchev;

static void		 restore_curs_x(struct buffer *);

static int		 readkey(void);
static void		 dispatch_stdio(int, short, void*);
static void		 handle_resize(int, short, void*);
static void		 handle_resize_nodelay(int, short, void*);
static void		 rearrange_windows(void);
static void		 line_prefix_and_text(struct vline *, char *, size_t, const char **, const char **);
static void		 print_vline(int, int, WINDOW*, struct vline*);
static void		 redraw_tabline(void);
static void		 redraw_window(WINDOW*, int, int, int, struct buffer*);
static void		 redraw_help(void);
static void		 redraw_body(struct tab*);
static void		 redraw_modeline(struct tab*);
static void		 redraw_minibuffer(void);
static void		 do_redraw_echoarea(void);
static void		 do_redraw_minibuffer(void);
static void		 do_redraw_minibuffer_compl(void);
static void		 place_cursor(int);
static void		 redraw_tab(struct tab*);
static void		 update_loading_anim(int, short, void*);
static void		 stop_loading_anim(struct tab*);

static int		 should_rearrange_windows;
static int		 show_tab_bar;
static int		 too_small;
static int		 x_offset;

struct thiskey	 thiskey;
struct tab	*current_tab;

static struct event	resizeev;
static struct timeval	resize_timer = { 0, 250000 };

static WINDOW	*tabline, *body, *modeline, *echoarea, *minibuffer;

int			 body_lines, body_cols;

static WINDOW		*help;
/* not static so we can see them from help.c */
struct buffer		 helpwin;
int			 help_lines, help_cols;

static int		 side_window;
static int		 in_side_window;

static struct timeval	loadingev_timer = { 0, 250000 };

static char	keybuf[64];

/* XXX: don't forget to init these in main() */
struct kmap global_map,
	minibuffer_map,
	*current_map,
	*base_map;

static inline void
update_x_offset(void)
{
	if (olivetti_mode && fill_column < body_cols)
		x_offset = (body_cols - fill_column)/2;
	else
		x_offset = 0;
}

void
save_excursion(struct excursion *place, struct buffer *buffer)
{
	place->curs_x = buffer->curs_x;
	place->curs_y = buffer->curs_y;
	place->line_off = buffer->line_off;
	place->top_line = buffer->top_line;
	place->current_line = buffer->current_line;
	place->cpoff = buffer->cpoff;
}

void
restore_excursion(struct excursion *place, struct buffer *buffer)
{
	buffer->curs_x = place->curs_x;
	buffer->curs_y = place->curs_y;
	buffer->line_off = place->line_off;
	buffer->top_line = place->top_line;
	buffer->current_line = place->current_line;
	buffer->cpoff = place->cpoff;
}

static void
restore_curs_x(struct buffer *buffer)
{
	struct vline	*vl;
	const char	*prfx, *text;

	vl = buffer->current_line;
	if (vl == NULL || vl->line == NULL)
		buffer->curs_x = buffer->cpoff = 0;
	else if (vl->parent->data != NULL) {
		text = vl->parent->data;
		buffer->curs_x = utf8_snwidth(text + 1, buffer->cpoff) + 1;
	} else
		buffer->curs_x = utf8_snwidth(vl->line, buffer->cpoff);

	buffer->curs_x += x_offset;

	if (vl == NULL)
		return;

	if (vl->parent->data != NULL)
		buffer->curs_x += utf8_swidth_between(vl->parent->line,
		    vl->parent->data);
	else {
		prfx = line_prefixes[vl->parent->type].prfx1;
		buffer->curs_x += utf8_swidth(prfx);
	}
}

void
global_key_unbound(void)
{
	message("%s is undefined", keybuf);
}

struct buffer *
current_buffer(void)
{
	if (in_minibuffer)
		return &ministate.buffer;
	if (in_side_window)
		return &helpwin;
	return &current_tab->buffer;
}

static int
readkey(void)
{
	uint32_t state = 0;

	if ((thiskey.key = wgetch(body)) == ERR)
		return 0;

	thiskey.meta = thiskey.key == 27;
	if (thiskey.meta) {
		thiskey.key = wgetch(body);
		if (thiskey.key == ERR || thiskey.key == 27) {
			thiskey.meta = 0;
			thiskey.key = 27;
		}
	}

	thiskey.cp = 0;

	if ((unsigned int)thiskey.key >= UINT8_MAX)
		return 1;

	while (1) {
		if (!utf8_decode(&state, &thiskey.cp, (uint8_t)thiskey.key))
			break;
		if ((thiskey.key = wgetch(body)) == ERR) {
			message("Error decoding user input");
			return 0;
		}
	}

	return 1;
}

static void
dispatch_stdio(int fd, short ev, void *d)
{
	struct keymap	*k;
	const char	*keyname;
	char		 tmp[5] = {0};

	/* TODO: schedule a redraw? */
	if (too_small)
		return;

	if (!readkey())
		return;

	if (keybuf[0] != '\0')
		strlcat(keybuf, " ", sizeof(keybuf));
	if (thiskey.meta)
		strlcat(keybuf, "M-", sizeof(keybuf));
	if (thiskey.cp != 0) {
		utf8_encode(thiskey.cp, tmp);
		strlcat(keybuf, tmp, sizeof(keybuf));
	} else if ((keyname = unkbd(thiskey.key)) != NULL) {
		strlcat(keybuf, keyname, sizeof(keybuf));
	} else {
		tmp[0] = thiskey.key;
		strlcat(keybuf, tmp, sizeof(keybuf));
	}

	TAILQ_FOREACH(k, &current_map->m, keymaps) {
		if (k->meta == thiskey.meta &&
		    k->key == thiskey.key) {
			if (k->fn == NULL)
				current_map = &k->map;
			else {
				current_map = base_map;
				strlcpy(keybuf, "", sizeof(keybuf));
				k->fn(current_buffer());
			}
			goto done;
		}
	}

	if (current_map->unhandled_input != NULL)
		current_map->unhandled_input();
	else
		global_key_unbound();

	strlcpy(keybuf, "", sizeof(keybuf));
	current_map = base_map;

done:
	if (side_window)
		recompute_help();

	if (should_rearrange_windows)
		rearrange_windows();
	redraw_tab(current_tab);
}

static void
handle_resize(int sig, short ev, void *d)
{
	if (event_pending(&resizeev, EV_TIMEOUT, NULL)) {
		event_del(&resizeev);
	}
	evtimer_set(&resizeev, handle_resize_nodelay, NULL);
	evtimer_add(&resizeev, &resize_timer);
}

static void
handle_resize_nodelay(int s, short ev, void *d)
{
	endwin();
	refresh();
	clear();

	rearrange_windows();
}

static inline int
should_show_tab_bar(void)
{
	if (tab_bar_show == -1)
		return 0;
	if (tab_bar_show == 0)
		return 1;

	return TAILQ_NEXT(TAILQ_FIRST(&tabshead), tabs) != NULL;
}

static void
rearrange_windows(void)
{
	int		 lines;

	should_rearrange_windows = 0;
	show_tab_bar = should_show_tab_bar();

	lines = LINES;

	if ((too_small = lines < 15)) {
		erase();
		printw("Window too small.");
		refresh();
		return;
	}

	/* move and resize the windows, in reverse order! */

	if (in_minibuffer == MB_COMPREAD) {
		mvwin(minibuffer, lines-10, 0);
		wresize(minibuffer, 10, COLS);
		lines -= 10;

		wrap_page(&ministate.compl.buffer, COLS);
	}

	mvwin(echoarea, --lines, 0);
	wresize(echoarea, 1, COLS);

	mvwin(modeline, --lines, 0);
	wresize(modeline, 1, COLS);

	body_lines = show_tab_bar ? --lines : lines;
	body_cols = COLS;

	/*
	 * Here we make the assumption that show_tab_bar is either 0
	 * or 1, and reuse that as argument to mvwin.
	 */
	if (side_window) {
		help_cols = 0.3 * COLS;
		help_lines = lines;
		mvwin(help, show_tab_bar, 0);
		wresize(help, help_lines, help_cols);

		wrap_page(&helpwin, help_cols);

		body_cols = COLS - help_cols - 1;
		mvwin(body, show_tab_bar, help_cols);
	} else
		mvwin(body, show_tab_bar, 0);

	update_x_offset();
	wresize(body, body_lines, body_cols);

	if (show_tab_bar)
		wresize(tabline, 1, COLS);

	wrap_page(&current_tab->buffer, body_cols);
	redraw_tab(current_tab);
}

static void
line_prefix_and_text(struct vline *vl, char *buf, size_t len,
    const char **prfx_ret, const char **text_ret)
{
	int type, i, cont, width;
	char *space, *t;

	if ((*text_ret = vl->line) == NULL)
		*text_ret = "";

	cont = vl->flags & L_CONTINUATION;
	type = vl->parent->type;
	if (!cont)
		*prfx_ret = line_prefixes[type].prfx1;
	else
		*prfx_ret = line_prefixes[type].prfx2;

	space = vl->parent->data;
	if (!emojify_link || type != LINE_LINK || space == NULL)
		return;

	if (cont) {
		memset(buf, 0, len);
		width = utf8_swidth_between(vl->parent->line, space);
		for (i = 0; i < width + 1; ++i)
			strlcat(buf, " ", len);
	} else {
		strlcpy(buf, *text_ret, len);
		if ((t = strchr(buf, ' ')) != NULL)
			*t = '\0';
		strlcat(buf, " ", len);

		/* skip the emoji */
		*text_ret += (space - vl->parent->line) + 1;
	}

	*prfx_ret = buf;
}

static inline void
print_vline_descr(int width, WINDOW *window, struct vline *vl)
{
	int x, y, goal;

	if (vl->parent->type != LINE_COMPL &&
	    vl->parent->type != LINE_COMPL_CURRENT &&
	    vl->parent->type != LINE_HELP)
		return;

	if (vl->parent->alt == NULL)
		return;

	(void)y;
	getyx(window, y, x);

	if (vl->parent->type == LINE_HELP)
		goal = 8;
	else
		goal = width/2;

	if (goal <= x)
		wprintw(window, " ");
	for (; goal > x; ++x)
		wprintw(window, " ");

	wprintw(window, "%s", vl->parent->alt);
}

/*
 * Core part of the rendering.  It prints a vline starting from the
 * current cursor position.  Printing a vline consists of skipping
 * `off' columns (for olivetti-mode), print the correct prefix (which
 * may be the emoji in case of emojified links-lines), printing the
 * text itself, filling until width - off and filling off columns
 * again.
 */
static void
print_vline(int off, int width, WINDOW *window, struct vline *vl)
{
	/*
	 * Believe me or not, I've seen emoji ten code points long!
	 * That means, to stay large, 4*10 bytes + NUL.
	 */
	char emojibuf[41] = {0};
	const char *text, *prfx;
	struct line_face *f;
	int i, left, x, y;

	f = &line_faces[vl->parent->type];

	/* unused, set by getyx */
	(void)y;

	line_prefix_and_text(vl, emojibuf, sizeof(emojibuf), &prfx, &text);

	wattr_on(window, body_face.left, NULL);
	for (i = 0; i < off; i++)
		waddch(window, ' ');
	wattr_off(window, body_face.left, NULL);

	wattr_on(window, f->prefix, NULL);
	wprintw(window, "%s", prfx);
	wattr_off(window, f->prefix, NULL);

	wattr_on(window, f->text, NULL);
	wprintw(window, "%s", text);
	print_vline_descr(width, window, vl);
	wattr_off(window, f->text, NULL);

	getyx(window, y, x);

	left = width - x;

	wattr_on(window, f->trail, NULL);
	for (i = 0; i < left - off; ++i)
		waddch(window, ' ');
	wattr_off(window, f->trail, NULL);

	wattr_on(window, body_face.right, NULL);
	for (i = 0; i < off; i++)
		waddch(window, ' ');
	wattr_off(window, body_face.right, NULL);

}

static void
redraw_tabline(void)
{
	struct tab	*tab;
	size_t		 toskip, ots, tabwidth, space, x;
	int		 current, y, truncated, pair;
	const char	*title;
	char		 buf[25];

	x = 0;

	/* unused, but setted by a getyx */
	(void)y;

	tabwidth = sizeof(buf)+1;
	space = COLS-2;

	toskip = 0;
	TAILQ_FOREACH(tab, &tabshead, tabs) {
		toskip++;
		if (tab == current_tab)
			break;
	}

	if (toskip * tabwidth < space)
		toskip = 0;
	else {
		ots = toskip;
		toskip--;
		while (toskip != 0 &&
		    (ots - toskip+1) * tabwidth < space)
			toskip--;
	}

	werase(tabline);
	wattr_on(tabline, tab_face.background, NULL);
	wprintw(tabline, toskip == 0 ? " " : "<");
	wattr_off(tabline, tab_face.background, NULL);

	truncated = 0;
	TAILQ_FOREACH(tab, &tabshead, tabs) {
		if (truncated)
			break;
		if (toskip != 0) {
			toskip--;
			continue;
		}

		getyx(tabline, y, x);
		if (x + sizeof(buf)+2 >= (size_t)COLS)
			truncated = 1;

		current = tab == current_tab;

		if (*(title = tab->buffer.page.title) == '\0')
			title = tab->hist_cur->h;

		if (tab->flags & TAB_URGENT)
			strlcpy(buf, "!", sizeof(buf));
		else
			strlcpy(buf, " ", sizeof(buf));

		if (strlcat(buf, title, sizeof(buf)) >= sizeof(buf)) {
			/* truncation happens */
			strlcpy(&buf[sizeof(buf)-4], "...", 4);
		} else {
			/* pad with spaces */
			while (strlcat(buf, "    ", sizeof(buf)) < sizeof(buf))
				/* nop */ ;
		}

		pair = current ? tab_face.current : tab_face.tab;
		wattr_on(tabline, pair, NULL);
		wprintw(tabline, "%s", buf);
		wattr_off(tabline, pair, NULL);

		wattr_on(tabline, tab_face.background, NULL);
		if (TAILQ_NEXT(tab, tabs) != NULL)
			wprintw(tabline, "???");
		wattr_off(tabline, tab_face.background, NULL);
	}

	wattr_on(tabline, tab_face.background, NULL);
	for (; x < (size_t)COLS; ++x)
		waddch(tabline, ' ');
	if (truncated)
		mvwprintw(tabline, 0, COLS-1, ">");
	wattr_off(tabline, tab_face.background, NULL);
}

/*
 * Compute the first visible line around vl.  Try to search forward
 * until the end of the buffer; if a visible line is not found, search
 * backward.  Return NULL if no viable line was found.
 */
struct vline *
adjust_line(struct vline *vl, struct buffer *buffer)
{
	struct vline *t;

	if (vl == NULL)
		return NULL;

	if (!(vl->parent->flags & L_HIDDEN))
		return vl;

	/* search forward */
	for (t = vl;
	     t != NULL && t->parent->flags & L_HIDDEN;
	     t = TAILQ_NEXT(t, vlines))
		;		/* nop */

	if (t != NULL)
		return t;

	/* search backward */
	for (t = vl;
	     t != NULL && t->parent->flags & L_HIDDEN;
	     t = TAILQ_PREV(t, vhead, vlines))
		;		/* nop */

	return t;
}

static void
redraw_window(WINDOW *win, int off, int height, int width,
    struct buffer *buffer)
{
	struct vline	*vl;
	int		 l, onscreen;

	restore_curs_x(buffer);

	/*
	 * TODO: ignoring buffer->force_update and always
	 * re-rendering.  In theory we can recompute the y position
	 * without a re-render, and optimize here.  It's not the only
	 * optimisation possible here, wscrl wolud also be an
	 * interesting one.
	 */

again:
	werase(win);
	buffer->curs_y = 0;

	if (TAILQ_EMPTY(&buffer->head))
		goto end;

	if (buffer->top_line == NULL)
		buffer->top_line = TAILQ_FIRST(&buffer->head);

	buffer->top_line = adjust_line(buffer->top_line, buffer);
	if (buffer->top_line == NULL)
		goto end;

	buffer->current_line = adjust_line(buffer->current_line, buffer);

	l = 0;
	onscreen = 0;
	for (vl = buffer->top_line; vl != NULL; vl = TAILQ_NEXT(vl, vlines)) {
		if (vl->parent->flags & L_HIDDEN)
			continue;

		wmove(win, l, 0);
		print_vline(off, width, win, vl);

		if (vl == buffer->current_line)
			onscreen = 1;

		if (!onscreen)
			buffer->curs_y++;

		l++;
		if (l == height)
			break;
	}

	if (!onscreen) {
		for (; vl != NULL; vl = TAILQ_NEXT(vl, vlines)) {
			if (vl == buffer->current_line)
				break;
			if (vl->parent->flags & L_HIDDEN)
				continue;
			buffer->line_off++;
			buffer->top_line = TAILQ_NEXT(buffer->top_line, vlines);
		}

		if (vl != NULL)
			goto again;
	}

	buffer->last_line_off = buffer->line_off;
	buffer->force_redraw = 0;
end:
	wmove(win, buffer->curs_y, buffer->curs_x);
}

static void
redraw_help(void)
{
	redraw_window(help, 0, help_lines, help_cols, &helpwin);
}

static void
redraw_body(struct tab *tab)
{
	static struct tab *last_tab;

	if (last_tab != tab)
		tab->buffer.force_redraw =1;
	last_tab = tab;

	redraw_window(body, x_offset, body_lines, body_cols, &tab->buffer);
}

static inline char
trust_status_char(enum trust_state ts)
{
	switch (ts) {
	case TS_UNKNOWN:	return '-';
	case TS_UNTRUSTED:	return '!';
	case TS_TEMP_TRUSTED:	return '!';
	case TS_TRUSTED:	return 'v';
	case TS_VERIFIED:	return 'V';
	default:		return 'X';
	}
}

static void
redraw_modeline(struct tab *tab)
{
	struct buffer	*buffer;
	double		 pct;
	int		 x, y, max_x, max_y;
	const char	*mode;
	const char	*spin = "-\\|/";

	buffer = current_buffer();
	mode = buffer->page.name;

	werase(modeline);
	wattr_on(modeline, modeline_face.background, NULL);
	wmove(modeline, 0, 0);

	wprintw(modeline, "-%c%c %s ",
	    spin[tab->loading_anim_step],
	    trust_status_char(tab->trust),
	    mode == NULL ? "(none)" : mode);

	pct = (buffer->line_off + buffer->curs_y) * 100.0
		/ buffer->line_max;

	if (buffer->line_max <= (size_t)body_lines)
		wprintw(modeline, "All ");
	else if (buffer->line_off == 0)
		wprintw(modeline, "Top ");
	else if (buffer->line_off + body_lines >= buffer->line_max)
		wprintw(modeline, "Bottom ");
	else
		wprintw(modeline, "%.0f%% ", pct);

	wprintw(modeline, "%d/%d %s ",
	    buffer->line_off + buffer->curs_y,
	    buffer->line_max,
	    tab->hist_cur->h);

	getyx(modeline, y, x);
	getmaxyx(modeline, max_y, max_x);

	(void)y;
	(void)max_y;

	for (; x < max_x; ++x)
		waddstr(modeline, "-");

	wattr_off(modeline, modeline_face.background, NULL);
}

static void
redraw_minibuffer(void)
{
	wattr_on(echoarea, minibuffer_face.background, NULL);
	werase(echoarea);

	if (in_minibuffer)
		do_redraw_minibuffer();
	else
		do_redraw_echoarea();

	if (in_minibuffer == MB_COMPREAD)
		do_redraw_minibuffer_compl();

	wattr_off(echoarea, minibuffer_face.background, NULL);
}

static void
do_redraw_echoarea(void)
{
	struct vline *vl;

	if (ministate.curmesg != NULL)
		wprintw(echoarea, "%s", ministate.curmesg);
	else if (*keybuf != '\0')
		waddstr(echoarea, keybuf);
	else {
		/* If nothing else, show the URL at point */
		vl = current_tab->buffer.current_line;
		if (vl != NULL && vl->parent->type == LINE_LINK)
			waddstr(echoarea, vl->parent->alt);
	}
}

static void
do_redraw_minibuffer(void)
{
	struct buffer	*cmplbuf, *buffer;
	size_t		 off_y, off_x = 0;
	const char	*start, *c;

	cmplbuf = &ministate.compl.buffer;
	buffer = &ministate.buffer;
	(void)off_y;		/* unused, set by getyx */

	wmove(echoarea, 0, 0);

	if (in_minibuffer == MB_COMPREAD)
		wprintw(echoarea, "(%2d) ",
		    cmplbuf->line_max);

	wprintw(echoarea, "%s", ministate.prompt);
	if (ministate.hist_cur != NULL)
		wprintw(echoarea, "(%zu/%zu) ",
		    ministate.hist_off + 1,
		    ministate.history->len);

	getyx(echoarea, off_y, off_x);

	start = ministate.hist_cur != NULL
		? ministate.hist_cur->h
		: ministate.buf;
	c = utf8_nth(buffer->current_line->line, buffer->cpoff);
	while (utf8_swidth_between(start, c) > (size_t)COLS/2) {
		start = utf8_next_cp(start);
	}

	waddstr(echoarea, start);

	if (ministate.curmesg != NULL)
		wprintw(echoarea, " [%s]", ministate.curmesg);

	wmove(echoarea, 0, off_x + utf8_swidth_between(start, c));
}

static void
do_redraw_minibuffer_compl(void)
{
	redraw_window(minibuffer, 0, 10, COLS,
	    &ministate.compl.buffer);
}

/*
 * Place the cursor in the right ncurses window.  If soft is 1, use
 * wnoutrefresh (which shouldn't cause any I/O); otherwise use
 * wrefresh.
 */
static void
place_cursor(int soft)
{
	int (*touch)(WINDOW *);

	if (soft)
		touch = wnoutrefresh;
	else
		touch = wrefresh;

	if (in_minibuffer) {
		if (side_window)
			touch(help);
		touch(body);
		touch(echoarea);
	} else if (in_side_window) {
		touch(body);
		touch(echoarea);
		touch(help);
	} else {
		if (side_window)
			touch(help);
		touch(echoarea);
		touch(body);
	}
}

static void
redraw_tab(struct tab *tab)
{
	if (too_small)
		return;

	if (side_window) {
		redraw_help();
		wnoutrefresh(help);
	}

	if (show_tab_bar)
		redraw_tabline();

	redraw_body(tab);
	redraw_modeline(tab);
	redraw_minibuffer();

	wnoutrefresh(tabline);
	wnoutrefresh(modeline);

	if (in_minibuffer == MB_COMPREAD)
		wnoutrefresh(minibuffer);

	place_cursor(1);

	doupdate();

	if (set_title)
		dprintf(1, "\033]2;%s - Telescope\a",
		    current_tab->buffer.page.title);
}

void
start_loading_anim(struct tab *tab)
{
	if (tab->loading_anim)
		return;
	tab->loading_anim = 1;
	evtimer_set(&tab->loadingev, update_loading_anim, tab);
	evtimer_add(&tab->loadingev, &loadingev_timer);
}

static void
update_loading_anim(int fd, short ev, void *d)
{
	struct tab	*tab = d;

	tab->loading_anim_step = (tab->loading_anim_step+1)%4;

	if (tab == current_tab) {
		redraw_modeline(tab);
		wrefresh(modeline);
		wrefresh(body);
		if (in_minibuffer)
			wrefresh(echoarea);
	}

	evtimer_add(&tab->loadingev, &loadingev_timer);
}

static void
stop_loading_anim(struct tab *tab)
{
	if (!tab->loading_anim)
		return;
	evtimer_del(&tab->loadingev);
	tab->loading_anim = 0;
	tab->loading_anim_step = 0;

	if (tab != current_tab)
		return;

	redraw_modeline(tab);

	wrefresh(modeline);
	wrefresh(body);
	if (in_minibuffer)
		wrefresh(echoarea);
}

int
ui_print_colors(void)
{
	int colors = 0, pairs = 0, can_change = 0;
	int columns = 16, lines, color, i, j;

	initscr();
	if (has_colors()) {
		start_color();
		use_default_colors();

		colors = COLORS;
		pairs = COLOR_PAIRS;
		can_change = can_change_color();
	}
	endwin();

	printf("Term info:\n");
	printf("TERM=%s COLORS=%d COLOR_PAIRS=%d can_change_colors=%d\n",
	    getenv("TERM"), colors, pairs, can_change);
	printf("\n");

	if (colors == 0) {
		printf("No color support\n");
		return 0;
	}

	printf("Available colors:\n\n");
	lines = (colors - 1) / columns + 1;
	color = 0;
	for (i = 0; i < lines; ++i) {
		for (j = 0; j < columns; ++j, ++color) {
			printf("\033[0;38;5;%dm %03d", color, color);
		}
		printf("\n");
	}

	printf("\033[0m");
	fflush(stdout);
	return 0;
}

int
ui_init()
{
	setlocale(LC_ALL, "");

	if (TAILQ_EMPTY(&global_map.m)) {
		fprintf(stderr, "no keys defined!\n");
		return 0;
	}

	minibuffer_init();

	/* initialize help window */
	TAILQ_INIT(&helpwin.head);
	TAILQ_INIT(&helpwin.page.head);

	base_map = &global_map;
	current_map = &global_map;

	initscr();

	if (enable_colors) {
		if (has_colors()) {
			start_color();
			use_default_colors();
		} else
			enable_colors = 0;
	}

	config_apply_style();

	raw();
	noecho();
	nonl();
	intrflush(stdscr, FALSE);

	if ((tabline = newwin(1, 1, 0, 0)) == NULL)
		return 0;
	if ((body = newwin(1, 1, 0, 0)) == NULL)
		return 0;
	if ((modeline = newwin(1, 1, 0, 0)) == NULL)
		return 0;
	if ((echoarea = newwin(1, 1, 0, 0)) == NULL)
		return 0;
	if ((minibuffer = newwin(1, 1, 0, 0)) == NULL)
		return 0;
	if ((help = newwin(1, 1, 0, 0)) == NULL)
		return 0;

	wbkgd(body, body_face.body);
	wbkgd(echoarea, minibuffer_face.background);

	update_x_offset();

	keypad(body, TRUE);
	scrollok(body, FALSE);

	/* non-blocking input */
	wtimeout(body, 0);
	wtimeout(help, 0);

	mvwprintw(body, 0, 0, "");

	evtimer_set(&resizeev, handle_resize, NULL);

	event_set(&stdioev, 0, EV_READ | EV_PERSIST, dispatch_stdio, NULL);
	event_add(&stdioev, NULL);

	signal_set(&winchev, SIGWINCH, handle_resize, NULL);
	signal_add(&winchev, NULL);

	return 1;
}

void
ui_main_loop(void)
{
	switch_to_tab(current_tab);
	rearrange_windows();

	event_dispatch();
}

void
ui_on_tab_loaded(struct tab *tab)
{
	stop_loading_anim(tab);
	message("Loaded %s", tab->hist_cur->h);

	if (show_tab_bar)
		redraw_tabline();

	wrefresh(tabline);
	place_cursor(0);
}

void
ui_on_tab_refresh(struct tab *tab)
{
	wrap_page(&tab->buffer, body_cols);
	if (tab == current_tab)
		redraw_tab(tab);
	else
		tab->flags |= TAB_URGENT;
}

const char *
ui_keyname(int k)
{
	return keyname(k);
}

void
ui_toggle_side_window(void)
{
	side_window = !side_window;
	if (side_window)
		recompute_help();
	else
		in_side_window = 0;

	/*
	 * ugly hack, but otherwise the window doesn't get updated
	 * until I call rearrange_windows a second time (e.g. via
	 * C-l).  I will be happy to know why something like this is
	 * needed.
	 */
	rearrange_windows();
	rearrange_windows();
}

void
ui_schedule_redraw(void)
{
	should_rearrange_windows = 1;
}

void
ui_require_input(struct tab *tab, int hide, int proto)
{
	void (*fn)(void);

	if (proto == PROTO_GEMINI)
		fn = ir_select_gemini;
	else if (proto == PROTO_GOPHER)
		fn = ir_select_gopher;
	else
		abort();

	/* TODO: hard-switching to another tab is ugly */
	switch_to_tab(tab);

	enter_minibuffer(sensible_self_insert, fn, exit_minibuffer,
	    &ir_history, NULL, NULL);
	strlcpy(ministate.prompt, "Input required: ",
	    sizeof(ministate.prompt));
	redraw_tab(tab);
}

void
ui_after_message_hook(void)
{
	redraw_minibuffer();
	place_cursor(0);
}

void
ui_yornp(const char *prompt, void (*fn)(int, struct tab *),
    struct tab *data)
{
	yornp(prompt, fn, data);
	redraw_tab(current_tab);
}

void
ui_read(const char *prompt, void (*fn)(const char*, struct tab *),
    struct tab *data)
{
	minibuffer_read(prompt, fn, data);
	redraw_tab(current_tab);
}

void
ui_other_window(void)
{
	if (side_window)
		in_side_window = !in_side_window;
	else
		message("No other window to select");
}

void
ui_suspend(void)
{
	endwin();

	kill(getpid(), SIGSTOP);

	refresh();
	clear();
	rearrange_windows();
}

void
ui_end(void)
{
	endwin();
}

/*
 * Copyright (c) 2015  Imre Vadasz.  All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <err.h>
#include <errno.h>
#ifdef __linux__
#include <pty.h>
#endif
#include <libgen.h>
#include <libutil.h>
#include <pthread.h>
#include <termios.h>

#include <event2/event.h>
#include <xkbcommon/xkbcommon.h>
#include <waywin.h>
#include "../src/fbdraw.h"
#include "../libteken/teken.h"

struct bufent {
	teken_char_t ch;
	teken_attr_t attr;
	int16_t cursor;
	int16_t dirty;
};

void	fbteken_bell(void *thunk);
void	fbteken_cursor(void *thunk, const teken_pos_t *pos);
void	fbteken_putchar(void *thunk, const teken_pos_t *pos, teken_char_t ch,
	    const teken_attr_t *attr);
void	fbteken_fill(void *thunk, const teken_rect_t *rect, teken_char_t ch,
	    const teken_attr_t *attr);
void	fbteken_copy(void *thunk, const teken_rect_t *rect,
	    const teken_pos_t *pos);
void	fbteken_param(void *thunk, int param, unsigned int val);
void	fbteken_respond(void *thunk, const void *arg, size_t sz);

static int	handle_term_special_keysym(struct xkb_state *state,
					   xkb_keysym_t sym, uint8_t *buf,
					   size_t len);

struct ww_base *base;
struct ww_window *window;

struct rop_obj *rop;
int fnwidth, fnheight;

teken_funcs_t tek_funcs = {
	fbteken_bell,
	fbteken_cursor,
	fbteken_putchar,
	fbteken_fill,
	fbteken_copy,
	fbteken_param,
	fbteken_respond,
};

uint32_t colormap[TC_NCOLORS * 2] = {
	[TC_BLACK] = 0x00000000,
	[TC_RED] = 0x00800000,
	[TC_GREEN] = 0x00008000,
	[TC_BROWN] = 0x00808000,
	[TC_BLUE] = 0x000000c0,
	[TC_MAGENTA] = 0x00800080,
	[TC_CYAN] = 0x00008080,
	[TC_WHITE] = 0x00c0c0c0,
	[TC_BLACK + TC_NCOLORS] = 0x00808080,
	[TC_RED + TC_NCOLORS] = 0x00ff0000,
	[TC_GREEN + TC_NCOLORS] = 0x0000ff00,
	[TC_BROWN + TC_NCOLORS] = 0x00ffff00,
	[TC_BLUE + TC_NCOLORS] = 0x000000ff,
	[TC_MAGENTA + TC_NCOLORS] = 0x00ff00ff,
	[TC_CYAN + TC_NCOLORS] = 0x0000ffff,
	[TC_WHITE + TC_NCOLORS] = 0x00ffffff,
};

struct terminal {
	teken_t tek;
	struct bufent *buf;
	teken_pos_t cursorpos;
	int keypad, showcursor;
	struct winsize winsz;
	int amaster;
	pid_t child;
};

struct terminal *curterm;

/*
 * XXX organize termbuf as a linear array of pointers to rows, to reduce the
 *     cost of scrolling operations.
 */
struct bufent *oldbuf;
uint32_t *dirtybuf, dirtycount = 0;
int dirtyflag = 0;
teken_attr_t defattr = {
	ta_format : 0,
	ta_fgcolor : TC_WHITE,
	ta_bgcolor : TC_BLACK,
};
teken_attr_t white_defattr = {
	ta_format : 0,
	ta_fgcolor : TC_BLACK,
	ta_bgcolor : TC_WHITE,
};

static void
dirty_cell_slow(struct terminal *t, uint16_t col, uint16_t row)
{
	if (!dirtyflag && !t->buf[row * t->winsz.ws_col + col].dirty) {
		t->buf[row * t->winsz.ws_col + col].dirty = 1;
		dirtybuf[dirtycount] = row * t->winsz.ws_col + col;
		dirtycount++;
	}
}

static void
dirty_cell_fast(struct terminal *t __unused, uint16_t col __unused,
    uint16_t row __unused)
{
	dirtyflag = 1;
}

static void
render_cell(struct terminal *t, uint16_t col, uint16_t row)
{
	struct bufent *cell;
	teken_attr_t *attr;
	teken_char_t ch;
	uint16_t sx, sy;
	uint32_t bg, fg, val;
	int cursor, flags = 0;

	cell = &t->buf[row * t->winsz.ws_col + col];
	attr = &cell->attr;
	cursor = cell->cursor;
	ch = cell->ch;

	sx = col * fnwidth;
	sy = row * fnheight;
	if (attr->ta_format & TF_REVERSE) {
		fg = attr->ta_bgcolor;
		bg = attr->ta_fgcolor;
	} else {
		fg = attr->ta_fgcolor;
		bg = attr->ta_bgcolor;
	}
	if (fg < TC_NCOLORS) {
		if (attr->ta_format & TF_BOLD)
			fg = colormap[fg + TC_NCOLORS];
		else
			fg = colormap[fg];
	} else {
		fg = colormap[TC_WHITE];
		err(1, "color out of range: %d\n", attr->ta_fgcolor);
	}
	if (bg < TC_NCOLORS) {
		bg = colormap[bg];
	} else {
		bg = colormap[TC_BLACK];
		err(1, "color out of range: %d\n", bg);
	}
	if (t->showcursor && cursor) {
		val = fg;
		fg = bg;
		bg = val;
	}
	if (attr->ta_format & TF_UNDERLINE)
		flags |= 1;
	if (attr->ta_format & TF_BOLD)
		flags |= 2;
	rop32_rect(rop, (point){sx, sy},
	    (dimension){fnwidth, fnheight}, bg);
	if (ch != ' ')
		rop32_char(rop, (point){sx, sy}, fg, bg, ch, flags);
}

static void
set_cell_slow(struct terminal *t, uint16_t col, uint16_t row, teken_char_t ch,
    const teken_attr_t *attr)
{
	teken_char_t oldch;
	teken_attr_t oattr;

	oldch = t->buf[row * t->winsz.ws_col + col].ch;
	if (ch == oldch) {
		oattr = t->buf[row * t->winsz.ws_col + col].attr;
		if (oattr.ta_format == attr->ta_format &&
		    oattr.ta_fgcolor == attr->ta_fgcolor &&
		    oattr.ta_bgcolor == attr->ta_bgcolor)
			return;
	}
	t->buf[row * t->winsz.ws_col + col].ch = ch;
	t->buf[row * t->winsz.ws_col + col].attr = *attr;
	dirty_cell_slow(t, col, row);
}

static void
set_cell_medium(struct terminal *t, uint16_t col, uint16_t row, teken_char_t ch,
    const teken_attr_t *attr)
{
	teken_char_t oldch;
	teken_attr_t oattr;

	oldch = t->buf[row * t->winsz.ws_col + col].ch;
	if (ch == oldch) {
		oattr = t->buf[row * t->winsz.ws_col + col].attr;
		if (oattr.ta_format == attr->ta_format &&
		    oattr.ta_fgcolor == attr->ta_fgcolor &&
		    oattr.ta_bgcolor == attr->ta_bgcolor)
			return;
	}
	t->buf[row * t->winsz.ws_col + col].ch = ch;
	t->buf[row * t->winsz.ws_col + col].attr = *attr;
	dirty_cell_fast(t, col, row);
}

void
fbteken_bell(void *thunk __unused)
{
	/* XXX */
}

void
fbteken_cursor(void *thunk, const teken_pos_t *pos)
{
	struct terminal *t = (struct terminal *)thunk;

	if (t->cursorpos.tp_col == pos->tp_col &&
	    t->cursorpos.tp_row == pos->tp_row)
		return;
	t->cursorpos = *pos;
}

void
fbteken_putchar(void *thunk, const teken_pos_t *pos, teken_char_t ch,
    const teken_attr_t *attr)
{
	struct terminal *t = (struct terminal *)thunk;

	set_cell_slow(t, pos->tp_col, pos->tp_row, ch, attr);
}

void
fbteken_fill(void *thunk, const teken_rect_t *rect, teken_char_t ch,
    const teken_attr_t *attr)
{
	struct terminal *t = (struct terminal *)thunk;
	teken_unit_t a, b;

	for (a = rect->tr_begin.tp_row; a < rect->tr_end.tp_row; a++) {
		for (b = rect->tr_begin.tp_col; b < rect->tr_end.tp_col; b++) {
			set_cell_medium(t, b, a, ch, attr);
		}
	}
}

void
fbteken_copy(void *thunk, const teken_rect_t *rect, const teken_pos_t *pos)
{
	struct terminal *t = (struct terminal *)thunk;
	teken_unit_t w, h;
	teken_unit_t scol, srow, tcol, trow;
	int a;

	scol = rect->tr_begin.tp_col;
	srow = rect->tr_begin.tp_row;
	tcol = pos->tp_col;
	trow = pos->tp_row;
	w = rect->tr_end.tp_col - rect->tr_begin.tp_col;
	h = rect->tr_end.tp_row - rect->tr_begin.tp_row;

	if (srow < trow) {
		for (a = h - 1; a >= 0; a--) {
			memmove(&t->buf[(trow + a) * t->winsz.ws_col + tcol],
			    &t->buf[(srow + a) * t->winsz.ws_col + scol],
			    w * sizeof(*t->buf));
		}
	} else {
		for (a = 0; a < h; a++) {
			memmove(&t->buf[(trow + a) * t->winsz.ws_col + tcol],
			    &t->buf[(srow + a) * t->winsz.ws_col + scol],
			    w * sizeof(*t->buf));
		}
	}
	dirtyflag = 1;
}

void
fbteken_param(void *thunk, int param, unsigned int val)
{
	struct terminal *t = (struct terminal *)thunk;

//	fprintf(stderr, "fbteken_param param=%d val=%u\n", param, val);
	switch (param) {
	case 0:
		if (val)
			t->showcursor = 1;
		else
			t->showcursor = 0;
		break;
	case 1:
		if (val)
			t->keypad = 1;
		else
			t->keypad = 0;
		break;
	case 6:
		/* XXX */
		break;
	default:
		break;
	}
}

void
fbteken_respond(void *thunk __unused, const void *arg __unused,
    size_t sz __unused)
{
	/* XXX */
}

static void
set_nonblocking(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1) {
		warn("fcntl");
		return;
	}
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
		warn("fcntl");
}

static void
rdmaster(evutil_socket_t fd __unused, short events __unused, void *arg)
{
	struct terminal *t = (struct terminal *)arg;
	char s[0x1000];
	teken_pos_t oc;
	int val;

	val = read(t->amaster, s, 0x1000);
	if (val > 0) {
		oc = t->cursorpos;
		t->buf[oc.tp_row * t->winsz.ws_col + oc.tp_col].cursor = 0;
		teken_input(&t->tek, s, val);
		if (oc.tp_col != t->cursorpos.tp_col ||
		    oc.tp_row != t->cursorpos.tp_row) {
			t->buf[oc.tp_row * t->winsz.ws_col + oc.tp_col].cursor = 0;
			t->buf[t->cursorpos.tp_row * t->winsz.ws_col + t->cursorpos.tp_col].cursor = 1;
			dirty_cell_slow(t, oc.tp_col, oc.tp_row);
			dirty_cell_slow(t, t->cursorpos.tp_col, t->cursorpos.tp_row);
		} else {
			t->buf[oc.tp_row * t->winsz.ws_col + oc.tp_col].cursor = 1;
		}
		ww_window_dirty(window);
	} else if (val == 0 || errno != EAGAIN) {
		ww_base_loopbreak(base);
	}
}

static int
fbteken_key_get_utf8(struct xkb_state *state, xkb_keycode_t code,
    uint8_t *buf, int len)
{
	int n = 0;

	n += xkb_state_key_get_utf8(state, code, buf, len);

	/* XXX Add a command line flag to toggle this behaviour */
	if (xkb_state_mod_name_is_active(state,
	    "Mod1", XKB_STATE_MODS_EFFECTIVE)) {
		if (n > 0 && n + 1 < len) {
			memmove(buf + 1, buf, n);
			buf[0] = 0x1b;
			n++;
			buf[n] = 0;
		}
	}

	return n;
}

/*
 * XXX All the Ctl-Alt-XXX sequences are also handled specially in xterm
 *     (except for the F-Keys which trigger Xorg's vt-switching with Ctl-Alt).
 */
static int
handle_term_special_keysym(struct xkb_state *state, xkb_keysym_t sym,
    uint8_t *buf, size_t len)
{
	struct {
		xkb_keysym_t sym;
		unsigned int t;
		unsigned int ctlt;
		unsigned int altt;
	} sym_to_seq[] = {
		{ XKB_KEY_Up, TKEY_UP, TKEY_CTL_UP, TKEY_ALT_UP },
		{ XKB_KEY_Down, TKEY_DOWN, TKEY_CTL_DOWN, TKEY_ALT_DOWN },
		{ XKB_KEY_Left, TKEY_LEFT, TKEY_CTL_LEFT, TKEY_ALT_LEFT },
		{ XKB_KEY_Right, TKEY_RIGHT, TKEY_CTL_RIGHT, TKEY_ALT_RIGHT },
		{ XKB_KEY_Home, TKEY_HOME, TKEY_CTL_HOME, TKEY_ALT_HOME },
		{ XKB_KEY_End, TKEY_END, TKEY_CTL_END, TKEY_ALT_END },
		{ XKB_KEY_Insert, TKEY_INSERT, TKEY_CTL_INSERT, TKEY_ALT_INSERT },
		{ XKB_KEY_Delete, TKEY_DELETE, TKEY_CTL_DELETE, TKEY_ALT_DELETE },
		{ XKB_KEY_Page_Up, TKEY_PAGE_UP, TKEY_CTL_PAGE_UP, TKEY_ALT_PAGE_UP },
		{ XKB_KEY_Page_Down, TKEY_PAGE_DOWN, TKEY_CTL_PAGE_DOWN, TKEY_ALT_PAGE_DOWN },
		{ XKB_KEY_F1, TKEY_F1, TKEY_CTL_F1, TKEY_ALT_F1 },
		{ XKB_KEY_F2, TKEY_F2, TKEY_CTL_F2, TKEY_ALT_F2 },
		{ XKB_KEY_F3, TKEY_F3, TKEY_CTL_F3, TKEY_ALT_F3 },
		{ XKB_KEY_F4, TKEY_F4, TKEY_CTL_F4, TKEY_ALT_F4 },
		{ XKB_KEY_F5, TKEY_F5, TKEY_CTL_F5, TKEY_ALT_F5 },
		{ XKB_KEY_F6, TKEY_F6, TKEY_CTL_F6, TKEY_ALT_F6 },
		{ XKB_KEY_F7, TKEY_F7, TKEY_CTL_F7, TKEY_ALT_F7 },
		{ XKB_KEY_F8, TKEY_F8, TKEY_CTL_F8, TKEY_ALT_F8 },
		{ XKB_KEY_F9, TKEY_F9, TKEY_CTL_F9, TKEY_ALT_F9 },
		{ XKB_KEY_F10, TKEY_F10, TKEY_CTL_F10, TKEY_ALT_F10 },
		{ XKB_KEY_F11, TKEY_F11, TKEY_CTL_F11, TKEY_ALT_F11 },
		{ XKB_KEY_F12, TKEY_F12, TKEY_CTL_F12, TKEY_ALT_F12 }
	};
	const char *str = NULL;
	unsigned int i;

	for (i = 0; i < NELEM(sym_to_seq); i++) {
		if (sym_to_seq[i].sym == sym) {
			if (xkb_state_mod_name_is_active(state,
			    "Mod1", XKB_STATE_MODS_EFFECTIVE) &&
			    sym_to_seq[i].altt != 0) {
				str = teken_get_sequence(&curterm->tek,
				    sym_to_seq[i].altt);
			} else if (xkb_state_mod_name_is_active(state,
			    "Control", XKB_STATE_MODS_EFFECTIVE) &&
			    sym_to_seq[i].ctlt != 0) {
				str = teken_get_sequence(&curterm->tek,
				    sym_to_seq[i].ctlt);
			} else {
				str = teken_get_sequence(&curterm->tek,
				    sym_to_seq[i].t);
			}
			if (str != NULL)
				return snprintf(buf, len, "%s", str);
		}
	}

	return 0;
}

static int
handle_keypress(struct xkb_state *state, xkb_keycode_t code, xkb_keysym_t sym,
    uint8_t *buf, int len)
{
	int cnt = 0;

	cnt = handle_term_special_keysym(state, sym, buf, len);
	if (cnt > 0)
		return cnt;

	/*
	 * XXX In X the left Alt key is an additional modifier key,
	 *     we might want to optionally emulate that behaviour.
	 */
	/* XXX handle composition (e.g. accents) */
	return fbteken_key_get_utf8(state, code, buf, len);
}

#if 0
static int
cmp_cells(struct terminal *t, int i)
{
	teken_attr_t a, b;

	a = t->buf[i].attr;
	b = oldbuf[i].attr;

	if (t->buf[i].ch != oldbuf[i].ch ||
	    t->buf[i].cursor != oldbuf[i].cursor ||
	    a.ta_format != b.ta_format ||
	    a.ta_fgcolor != b.ta_fgcolor ||
	    a.ta_bgcolor != b.ta_bgcolor) {
		return 1;
	}

	return 0;
}
#endif

static void
redraw_term(struct terminal *t)
{
//	int idx;
	unsigned int i, cols, rows;

	cols = t->winsz.ws_col;
	rows = t->winsz.ws_row;
//	if (dirtyflag) {
		for (i = 0; i < cols * rows; i++) {
//			if (cmp_cells(t, i)) {
				t->buf[i].dirty = 0;
				render_cell(t, i % cols, i / cols);
//			}
		}
//	}
//	for (i = 0; i < dirtycount; i++) {
//		idx = dirtybuf[i];
//		if (t->buf[idx].dirty) {
//			t->buf[idx].dirty = 0;
//			render_cell(t, idx % cols, idx / cols);
//		}
//	}

	memcpy(oldbuf, t->buf, cols * rows * sizeof(*t->buf));

	dirtycount = 0;
	dirtyflag = 0;
}

static void
handleterm(evutil_socket_t fd __unused, short events __unused,
    void *arg __unused)
{
	ww_base_loopbreak(base);
}

static void
draw(struct ww_window *win __unused, void *arg __unused, uint8_t *mem,
    uint16_t width, uint16_t height __unused)
{
	rop32_setcontext(rop, mem, width);
	redraw_term(curterm);
}

static void
kbdfun(struct ww_window *win __unused, struct xkb_state *state,
    xkb_keycode_t code, int pressed)
{
	xkb_keysym_t keysym;
	uint8_t out[32];
	int n = 0;

	keysym = xkb_state_key_get_one_sym(state, code);
	if (pressed)
		n = handle_keypress(state, code, keysym, out, sizeof(out));
	if (n > 0) {
		/* XXX Make sure we write everything */
		write(curterm->amaster, out, n);
	}
}

static void
pointerfun(struct ww_window *win __unused, int x __unused, int y __unused)
{
//	printf("pointer x: %d y: %d\n", x, y);
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-a | -A] [-hw] [-f fontfile [-F bold_fontfile]] "
	    "[-s fontsize] [-c columns] [-r rows]\n",
	    getprogname());
	exit(1);
}

int
main(int argc, char *argv[])
{
	char *shell;
	teken_pos_t winsize;
	struct event_base *evbase;
	struct terminal term;
	char *normalfont = NULL, *boldfont = NULL;
	int i, ch;
	bool whitebg = false;
	int columns = 80, rows = 25;

	unsigned int fontheight = 16;
	bool alpha = true;

	const char *errstr;

	/* XXX handle bitmap fonts better */
	while ((ch = getopt(argc, argv, "aAhwf:F:s:c:r:")) != -1) {
		switch (ch) {
		case 'a':
			alpha = true;
			break;
		case 'A':
			alpha = false;
			break;
		case 'c':
			columns = strtonum(optarg, 10, 1024, &errstr);
			if (errstr) {
				errx(1, "terminal width is %s: %s", errstr,
				    optarg);
			}
			break;
		case 'f':
			normalfont = optarg;
			break;
		case 'F':
			boldfont = optarg;
			break;
		case 'r':
			rows = strtonum(optarg, 2, 512, &errstr);
			if (errstr) {
				errx(1, "terminal height is %s: %s", errstr,
				    optarg);
			}
			break;
		case 's':
			fontheight = strtonum(optarg, 6, 128, &errstr);
			if (errstr) {
				errx(1, "font height is %s: %s", errstr,
				    optarg);
			}
			break;
		case 'w':
			whitebg = true;
			break;
		case 'h':
		default:
			usage();
		}
	}

	/*
	 * Font settings. alpha=true can only be used for truetype fonts
	 * to enable antialiased font rendering.
	 */
#ifdef __linux__
	char default_normalfont[] =
	    "/usr/lib/X11/fonts/dejavu/DejaVuSansMono.ttf";
	char default_boldfont[] =
	    "/usr/lib/X11/fonts/dejavu/DejaVuSansMono-Bold.ttf";
#else
	char default_normalfont[] =
	    "/usr/local/share/fonts/dejavu/DejaVuSansMono.ttf";
	char default_boldfont[] =
	    "/usr/local/share/fonts/dejavu/DejaVuSansMono-Bold.ttf";
#endif

#if 0
	/* e.g. a bitmap font */
	char *normalfont = "/usr/local/lib/X11/fonts/misc/9x15.pcf.gz";
	char *boldfont = "/usr/local/lib/X11/fonts/misc/9x15.pcf.gz";
	fontheight = 15;
	alpha = false;
#endif

	if (boldfont != NULL && normalfont == NULL) {
		/* That doesn't really make any sense */
		errx(1, "Only a bold font was specified, this doesn't make "
		    "sense!\n");
	}
	if (boldfont == NULL && normalfont == NULL)
		boldfont = default_boldfont;
	if (normalfont == NULL)
		normalfont = default_normalfont;

	char termenv[] = "TERM=xterm";
	char defaultshell[] = "/bin/sh";

	memset(&term, 0, sizeof(term));
	curterm = &term;
	term.winsz = (struct winsize){
		25, 80, 8 * 25, 16 * 80
	};
	term.child = forkpty(&term.amaster, NULL, NULL, &term.winsz);
	if (term.child == -1) {
		err(EXIT_FAILURE, "forkpty");
	} else if (term.child == 0) {
		shell = getenv("SHELL");
		if (shell == NULL)
			shell = defaultshell;
		putenv(termenv);
		if (execlp(shell, basename(shell), NULL) == -1)
			err(EXIT_FAILURE, "execlp");
	}

	set_nonblocking(term.amaster);

	teken_init(&term.tek, &tek_funcs, &term);
	if (whitebg)
		teken_set_defattr(&term.tek, &white_defattr);
	else
		teken_set_defattr(&term.tek, &defattr);

	/* XXX handle errors (e.g. when invalid font paths are given) */
	rop = rop32_init(normalfont, boldfont, fontheight,
	    &fnwidth, &fnheight, alpha);

	base = ww_base_create(NULL, kbdfun, pointerfun);
	if (base == NULL)
		errx(1, "ww_base_create failed\n");
	ww_base_set_keyrepeat(base, 200, 50);
	window = ww_window_create(base, NULL, fnwidth * columns,
	    fnheight * rows, draw);
	if (window == NULL)
		errx(1, "ww_window_create failed\n");
	rop32_setclip(rop, (point){0,0},
	    (point){fnwidth*columns-1, fnheight*rows-1});

	winsize.tp_col = columns;
	winsize.tp_row = rows;
	teken_set_winsize(&term.tek, &winsize);
        term.winsz.ws_col = winsize.tp_col;
        term.winsz.ws_row = winsize.tp_row;
        term.winsz.ws_xpixel = term.winsz.ws_col * fnwidth;
        term.winsz.ws_ypixel = term.winsz.ws_row * fnheight;
	ioctl (term.amaster, TIOCSWINSZ, &term.winsz);
	term.buf = calloc(term.winsz.ws_col * term.winsz.ws_row,
	    sizeof(struct bufent));
	oldbuf = calloc(term.winsz.ws_col * term.winsz.ws_row,
	    sizeof(struct bufent));
	dirtybuf = calloc(term.winsz.ws_col * term.winsz.ws_row,
	    sizeof(uint32_t));
	term.keypad = 0;
	term.showcursor = 1;

	/* Resetting character cells to a default value */
	for (i = 0; i < term.winsz.ws_col * term.winsz.ws_row; i++) {
		term.buf[i].attr = *teken_get_defattr(&term.tek);
		term.buf[i].ch = ' ';
		term.buf[i].cursor = 0;
		term.buf[i].dirty = 0;
	}
	memcpy(oldbuf, term.buf,
	    term.winsz.ws_col * term.winsz.ws_row * sizeof(*term.buf));

	struct event *masterev, *sigintev;

	evbase = ww_base_get_evbase(base);

	/*
	 * This design tries to keep things interactive for the user.
	 *
	 * Lowest priority:  3 input from the master fd of the pty device
	 *     ||            2 output to the master fd of the pty device
	 *     vv            1 wayland communication
	 * Highest priority: 0 signal handlers
	 */
//	event_base_priority_init(evbase, 4);

	masterev = event_new(evbase, term.amaster,
	    EV_READ | EV_PERSIST, rdmaster, &term);
//	event_priority_set(masterev, 4);

	/* XXX event handler for writes to the master fd of the pty device */

	signal(SIGINT, SIG_IGN);
	sigintev = evsignal_new(evbase, SIGINT, handleterm, NULL);
//	event_priority_set(sigintev, 0);

	event_add(masterev, NULL);
	event_add(sigintev, NULL);

	ww_window_dirty(window);
	ww_base_loop(base);
	signal(SIGINT, SIG_DFL);

	event_del(sigintev);
	event_del(masterev);

	event_free(sigintev);
	event_free(masterev);

	free(term.buf);
	free(oldbuf);

	ww_window_destroy(window);
	ww_base_destroy(base);
}

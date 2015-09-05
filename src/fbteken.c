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

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <err.h>
#ifdef __linux__
#include <pty.h>
#endif
#include <libgen.h>
#include <libutil.h>
#include <pthread.h>
#include <termios.h>
#include <poll.h>

#include <sys/param.h>
#ifdef __linux__
#include <linux/vt.h>
#else
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/consio.h>
#include <sys/kbio.h>
#endif

#include <libkms/libkms.h>
#include <drm_fourcc.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <event2/event.h>

#include <xkbcommon/xkbcommon.h>

#include "fbdraw.h"
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
void	fbteken_copy(void *thunk, const teken_rect_t *rect, const teken_pos_t *pos);
void	fbteken_param(void *thunk, int param, unsigned int val);
void	fbteken_respond(void *thunk, const void *arg, size_t sz);

static int	handle_term_special_keysym(xkb_keysym_t sym, uint8_t *buf, size_t len);
static void	setdpms(int level);

int ttyfd;
int drmfd;

struct xkb_context *ctx;
struct xkb_keymap *keymap;
struct xkb_state *state;

int idle_timeout = 0;	/* idle timeout (in s) */

struct kms_driver *kms;
struct kms_bo *bo;
unsigned handles[4], pitches[4], offsets[4];
void *plane;
uint32_t fb_id;
drmModeCrtcPtr drmcrtc;
drmModeConnectorPtr drmconn;
int drmfbid;
int oldbuffer_id;
int vtnum;
#ifndef __linux__
int initialvtnum;
#endif
bool active = true;

struct rop_obj *rop;
int fnwidth, fnheight;
struct termios origtios;

struct winsize winsz = {
	24, 80, 8 * 24, 16 * 80
};
int amaster;
pid_t child;

teken_t tek;
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

/* XXX use double-buffering on termbuf, to better avoid unneeded redrawing */
/*
 * XXX organize termbuf as a linear array of pointers to rows, to reduce the
 *     cost of scrolling operations.
 */
struct bufent *termbuf1, *termbuf2;
teken_pos_t cursorpos;
int keypad, showcursor;
uint32_t *dirtybuf, dirtycount = 0;
int dirtyflag = 0;
teken_attr_t defattr = {
	ta_format : 0,
	ta_fgcolor : TC_WHITE,
	ta_bgcolor : TC_BLACK,
};

struct event_base *evbase;

static void
dirty_cell_slow(uint16_t col, uint16_t row)
{
	if (!dirtyflag && !termbuf1[row * winsz.ws_col + col].dirty) {
		termbuf1[row * winsz.ws_col + col].dirty = 1;
		dirtybuf[dirtycount] = row * winsz.ws_col + col;
		dirtycount++;
	}
}

static void
dirty_cell_fast(uint16_t col __unused, uint16_t row __unused)
{
	dirtyflag = 1;
}

static void
render_cell(uint16_t col, uint16_t row)
{
	struct bufent *cell;
	teken_attr_t *attr;
	teken_char_t ch;
	uint16_t sx, sy;
	uint32_t bg, fg, val;
	int cursor, flags = 0;

	cell = &termbuf1[row * winsz.ws_col + col];
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
	if (showcursor && cursor) {
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
set_cell_slow(uint16_t col, uint16_t row, teken_char_t ch,
    const teken_attr_t *attr)
{
	teken_char_t oldch;
	teken_attr_t oattr;

	oldch = termbuf1[row * winsz.ws_col + col].ch;
	if (ch == oldch) {
		oattr = termbuf1[row * winsz.ws_col + col].attr;
		if (oattr.ta_format == attr->ta_format &&
		    oattr.ta_fgcolor == attr->ta_fgcolor &&
		    oattr.ta_bgcolor == attr->ta_bgcolor)
			return;
	}
	termbuf1[row * winsz.ws_col + col].ch = ch;
	termbuf1[row * winsz.ws_col + col].attr = *attr;
	dirty_cell_slow(col, row);
}

static void
set_cell_medium(uint16_t col, uint16_t row, teken_char_t ch,
    const teken_attr_t *attr)
{
	teken_char_t oldch;
	teken_attr_t oattr;

	oldch = termbuf1[row * winsz.ws_col + col].ch;
	if (ch == oldch) {
		oattr = termbuf1[row * winsz.ws_col + col].attr;
		if (oattr.ta_format == attr->ta_format &&
		    oattr.ta_fgcolor == attr->ta_fgcolor &&
		    oattr.ta_bgcolor == attr->ta_bgcolor)
			return;
	}
	termbuf1[row * winsz.ws_col + col].ch = ch;
	termbuf1[row * winsz.ws_col + col].attr = *attr;
	dirty_cell_fast(col, row);
}

static void
set_cell_fast(uint16_t col, uint16_t row, teken_char_t ch,
    const teken_attr_t *attr)
{
	termbuf1[row * winsz.ws_col + col].ch = ch;
	termbuf1[row * winsz.ws_col + col].attr = *attr;
}

void
fbteken_bell(void *thunk __unused)
{
	/* XXX */
}

void
fbteken_cursor(void *thunk __unused, const teken_pos_t *pos)
{
	if (cursorpos.tp_col == pos->tp_col && cursorpos.tp_row == pos->tp_row)
		return;
	cursorpos = *pos;
}

void
fbteken_putchar(void *thunk __unused, const teken_pos_t *pos, teken_char_t ch,
    const teken_attr_t *attr)
{
	set_cell_slow(pos->tp_col, pos->tp_row, ch, attr);
}

void
fbteken_fill(void *thunk __unused, const teken_rect_t *rect, teken_char_t ch,
    const teken_attr_t *attr)
{
	teken_unit_t a, b;

	for (a = rect->tr_begin.tp_row; a < rect->tr_end.tp_row; a++) {
		for (b = rect->tr_begin.tp_col; b < rect->tr_end.tp_col; b++) {
			set_cell_medium(b, a, ch, attr);
		}
	}
}

void
fbteken_copy(void *thunk __unused, const teken_rect_t *rect,
    const teken_pos_t *pos)
{
	int a, b;
	teken_unit_t w, h;
	teken_unit_t scol, srow, tcol, trow;
	teken_attr_t *attr;
	teken_char_t ch;

	scol = rect->tr_begin.tp_col;
	srow = rect->tr_begin.tp_row;
	tcol = pos->tp_col;
	trow = pos->tp_row;
	w = rect->tr_end.tp_col - rect->tr_begin.tp_col;
	h = rect->tr_end.tp_row - rect->tr_begin.tp_row;
	if (scol < tcol && srow < trow) {
		for (a = h - 1; a >= 0; a--) {
			for (b = w - 1; b >= 0; b--) {
				ch = termbuf1[(srow + a) * winsz.ws_col + scol + b].ch;
				attr = &termbuf1[(srow + a) * winsz.ws_col + scol + b].attr;
				set_cell_fast(tcol + b, trow + a, ch, attr);
			}
		}
	} else if (scol >= tcol && srow < trow) {
		for (a = h - 1; a >= 0; a--) {
			for (b = 0; b < w; b++) {
				ch = termbuf1[(srow + a) * winsz.ws_col + scol + b].ch;
				attr = &termbuf1[(srow + a) * winsz.ws_col + scol + b].attr;
				set_cell_fast(tcol + b, trow + a, ch, attr);
			}
		}
	} else if (scol < tcol && srow >= trow) {
		for (a = 0; a < h; a++) {
			for (b = w - 1; b >= 0; b--) {
				ch = termbuf1[(srow + a) * winsz.ws_col + scol + b].ch;
				attr = &termbuf1[(srow + a) * winsz.ws_col + scol + b].attr;
				set_cell_fast(tcol + b, trow + a, ch, attr);
			}
		}
	} else if (scol >= tcol && srow >= trow) {
		for (a = 0; a < h; a++) {
			for (b = 0; b < w; b++) {
				ch = termbuf1[(srow + a) * winsz.ws_col + scol + b].ch;
				attr = &termbuf1[(srow + a) * winsz.ws_col + scol + b].attr;
				set_cell_fast(tcol + b, trow + a, ch, attr);
			}
		}
	}
	dirtyflag = 1;
}

void
fbteken_param(void *thunk __unused, int param, unsigned int val)
{
//	fprintf(stderr, "fbteken_param param=%d val=%u\n", param, val);
	switch (param) {
	case 0:
		if (val)
			showcursor = 1;
		else
			showcursor = 0;
		break;
	case 1:
		if (val)
			keypad = 1;
		else
			keypad = 0;
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
vtconfigure(void)
{
	int fd, ret;
	struct vt_mode m;
#ifdef __linux__
	struct vt_stat s;
#endif
	struct termios tios;
#ifndef __linux__
	int initial, vtno;
	char vtname[64];
#endif

#ifdef __linux__
	fd = open("/dev/tty", O_RDWR);
	if(fd < 0) {
		errx(1, "open /dev/tty failed\n");
	}
#else
	fd = open("/dev/ttyv0", O_RDWR);
	if(fd < 0) {
		errx(1, "open /dev/ttyv0 failed\n");
	}
	if (ioctl(fd, VT_GETACTIVE, &initial) != 0)
		initial = -1;
	initialvtnum = initial;
	printf("initial vt is %d\n", initial);
	if (ioctl(fd, VT_OPENQRY, &vtno) != 0) {
		printf("no free tty\n");
		exit(1);
	}
	printf("free vt is %d\n", vtno);
	vtnum = vtno;
	close(fd);

	snprintf(vtname, sizeof(vtname), "/dev/ttyv%01x", vtno -1);

	fd = open(vtname, O_RDWR);
	if(fd < 0) {
		warn("%s", vtname);
		exit(1);
	}
#endif
	ttyfd = fd;

#ifndef __linux__
	if (initial != vtnum) {
		if (ioctl(ttyfd, VT_ACTIVATE, vtnum) != 0)
			errx(1, "VT_ACTIVATE failed");
		if (ioctl(ttyfd, VT_WAITACTIVE, vtnum) != 0)
			errx(1, "VT_WAITACTIVE failed");
	}
#endif

	ret = ioctl(fd, VT_GETMODE, &m);
	if(ret != 0) {
		printf("ioctl VT_GETMODE failed\n");
	}
	m.mode = VT_PROCESS;
	m.relsig = SIGUSR1;
	m.acqsig = SIGUSR2;
#ifdef __DragonFly__
	m.frsig = SIGIO; /* not used, but has to be set anyway */
#endif
	ret = ioctl(fd, VT_SETMODE, &m);
	if(ret != 0) {
		printf("ioctl VT_SETMODE failed\n");
	}
#ifdef __linux__
	ret = ioctl(fd, VT_GETSTATE, &s);
	if(ret != 0) {
		printf("ioctl VT_GETSTATE failed\n");
	}
	vtnum = s.v_active;
#else
	ret = ioctl(fd, VT_GETACTIVE, &vtnum);
	if(ret != 0) {
		printf("ioctl VT_GETACTIVE failed\n");
	}
#endif
	signal(SIGUSR1, SIG_IGN);
	signal(SIGUSR2, SIG_IGN);
#ifndef __linux__
	if (ioctl(fd, KDSETMODE, KD_GRAPHICS) != 0)
		warnx("KDSETMODE failed");
#endif
	/* Putting the tty into raw mode */
	tcgetattr(fd, &tios);
	origtios = tios;
	cfmakeraw(&tios);
	tcsetattr(fd, TCSAFLUSH, &tios);

	/* Setting Keyboard mode */
#ifndef __linux__
	if (ioctl(fd, KDSKBMODE, K_CODE) != 0)
		warnx("KDSKBMODE failed");
#endif
}

static void
vtdeconf(void)
{
	int fd, ret;
	struct vt_mode m;
#ifndef __linux__
	int n;
#endif

	fd = ttyfd;
	ret = ioctl(fd, VT_GETMODE, &m);
	if(ret != 0) {
		printf("ioctl VT_GETMODE failed\n");
	}
	m.mode = VT_AUTO;
	ret = ioctl(fd, VT_SETMODE, &m);
	if(ret != 0) {
		printf("ioctl VT_SETMODE failed\n");
	}
	signal(SIGUSR1, SIG_DFL);
	signal(SIGUSR2, SIG_DFL);
	/* Set tty settings to original values */
	tcsetattr(fd, TCSAFLUSH, &origtios);

#ifndef __linux__
	if (ioctl(fd, KDSETMODE, KD_TEXT) != 0)
		warnx("KDSETMODE failed");
	if (ioctl(fd, VT_GETACTIVE, &n) == 0) {
		if (n != initialvtnum) {
			if (ioctl(fd, VT_ACTIVATE, initialvtnum) == 0) {
				ioctl(fd, VT_WAITACTIVE, initialvtnum);
			}
		}
	}
	if (ioctl(fd, KDSKBMODE, K_XLATE) != 0)
		warnx("KDSKBMODE failed");
#endif

	close(fd);
}

static void
wait_vblank(void)
{
	if (active && (dirtyflag || dirtycount > 0)) {
		drmVBlank req = {
			.request.type = _DRM_VBLANK_RELATIVE |
					_DRM_VBLANK_EVENT,
			.request.sequence = 1,
			.request.signal = 0
		};
		drmWaitVBlank(drmfd, &req);
	}
}

struct event *idleev;

/* idle timeout */
struct timeval idletv = { .tv_sec = 0, .tv_usec = 0 };

static void
handleidle(evutil_socket_t fd __unused, short events __unused,
    void *arg __unused)
{
	setdpms(DRM_MODE_DPMS_SUSPEND);

	event_add(idleev, &idletv);
}

static void
rdmaster(evutil_socket_t fd __unused, short events __unused, void *arg __unused)
{
	int val;
	char s[0x1000];
	teken_pos_t oc;
	uint32_t prevdirty, prevdirtyflag;

	val = read(amaster, s, 0x1000);
	if (val > 0) {
		prevdirty = dirtycount;
		prevdirtyflag = dirtyflag;
		oc = cursorpos;
		teken_input(&tek, s, val);
		if (oc.tp_col != cursorpos.tp_col ||
		    oc.tp_row != cursorpos.tp_row) {
			termbuf1[oc.tp_row * winsz.ws_col + oc.tp_col].cursor = 0;
			termbuf1[cursorpos.tp_row * winsz.ws_col + cursorpos.tp_col].cursor = 1;
			dirty_cell_slow(oc.tp_col, oc.tp_row);
			dirty_cell_slow(cursorpos.tp_col, cursorpos.tp_row);
		}
		if (prevdirty == 0 || prevdirtyflag == 0)
			wait_vblank();
	} else {
		event_base_loopbreak(evbase);
	}
}

static int
fbteken_key_get_utf8(xkb_keycode_t code, uint8_t *buf, int len)
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

struct event *repeatev;
xkb_keycode_t repkeycode = 0;
xkb_keysym_t repkeysym = 0;

/* 200ms delay */
struct timeval repdelay = { .tv_sec = 0, .tv_usec = 200000 };
/* 50Hz repeat rate */
struct timeval reprate = { .tv_sec = 0, .tv_usec = 20000 };

/* Key repeat handling */
static void
keyrepeat(evutil_socket_t fd __unused, short events __unused,
    void *arg __unused)
{
	uint8_t out[16];
	int n;

	if (repkeycode != 0) {
		n = handle_term_special_keysym(repkeysym, out, sizeof(out));
		if (n <= 0) {
			/* XXX handle composition (e.g. accents) */
			n = fbteken_key_get_utf8(repkeycode, out, sizeof(out));
		}
		evtimer_add(repeatev, &reprate);
		if (n > 0)
			write(amaster, out, n);
	}
}

static xkb_keycode_t
at_toxkb(uint8_t atcode)
{
	xkb_keycode_t xkc;

	if ((atcode & 0x7f) <= 0x58) {
		xkc = (atcode & 0x7f);
	} else {
		/*
		 * Translate keycodes as in sys/dev/misc/kbd/atkbd.c
		 * from DragonFly to evdev key values.
		 */
		xkb_keycode_t code_to_key[128] = {
			[0x54] = 99,	/* sysrq */
			[0x59] = 96,	/* right enter key */
			[0x5a] = 97,	/* right ctrl key */
			[0x5b] = 98,	/* keypad divide key */
			[0x5c] = 210,	/* print scrn key */
			[0x5d] = 100,	/* right alt key */
			[0x5e] = 102,	/* grey home key */
			[0x5f] = 103,	/* grey up arrow key */
			[0x60] = 104,	/* grey page up key */
			[0x61] = 105,	/* grey left arrow key */
			[0x62] = 106,	/* grey right arrow key */
			[0x63] = 107,	/* grey end key */
			[0x64] = 108,	/* grey down arrow key */
			[0x65] = 109,	/* grey page down key */
			[0x66] = 110,	/* grey insert key */
			[0x67] = 111,	/* grey delete key */
			[0x68] = 119,	/* pause */
			[0x69] = 125,	/* left Window key */
			[0x6a] = 126,	/* right Window key */
			[0x6b] = 139,	/* menu key */
			[0x6c] = 0x19b,	/* break (??) */
		};
		xkc = code_to_key[atcode & 0x7f];
	}
	/* Fixes e.g. the '<' key which lies between 0x50 and 0x58 */
	if (xkc == 0)
		xkc = (atcode & 0x7f);

	if (xkc > 0)
		return xkc + 8;
	else
		return 0;
}

static int
at_ispress(uint8_t atcode)
{
	if (atcode & 0x80)
		return 0;
	else
		return 1;
}

/* Keep track of keys for vt switching */
static int
handle_vtswitch(xkb_keysym_t sym)
{
	xkb_keysym_t sym_to_num[] = {
		XKB_KEY_XF86Switch_VT_1,
		XKB_KEY_XF86Switch_VT_2,
		XKB_KEY_XF86Switch_VT_3,
		XKB_KEY_XF86Switch_VT_4,
		XKB_KEY_XF86Switch_VT_5,
		XKB_KEY_XF86Switch_VT_6,
		XKB_KEY_XF86Switch_VT_7,
		XKB_KEY_XF86Switch_VT_8,
		XKB_KEY_XF86Switch_VT_9,
		XKB_KEY_XF86Switch_VT_10,
		XKB_KEY_XF86Switch_VT_11,
		XKB_KEY_XF86Switch_VT_12
	};
	unsigned int i;

	for (i = 0; i < NELEM(sym_to_num); i++) {
		if (sym_to_num[i] == sym) {
			warnx("switching to vt %d", i + 1);
			return i + 1;
		}
	}

	return 0;
}

/*
 * XXX All the Ctl-Alt-XXX sequences are also handled specially in xterm
 *     (except for the F-Keys which trigger Xorg's vt-switching with Ctl-Alt).
 */
static int
handle_term_special_keysym(xkb_keysym_t sym, uint8_t *buf, size_t len)
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
				str = teken_get_sequence(&tek,
				    sym_to_seq[i].altt);
			} else if (xkb_state_mod_name_is_active(state,
			    "Control", XKB_STATE_MODS_EFFECTIVE) &&
			    sym_to_seq[i].ctlt != 0) {
				str = teken_get_sequence(&tek,
				    sym_to_seq[i].ctlt);
			} else {
				str = teken_get_sequence(&tek, sym_to_seq[i].t);
			}
			if (str != NULL)
				return snprintf(buf, len, "%s", str);
		}
	}

	return 0;
}

drmModeConnectorPtr conn = NULL;

static void
setdpms(int level)
{
	int i;
	drmModePropertyPtr prop = NULL, props;

	static int mode = DRM_MODE_DPMS_ON;

	if (level == mode)
		return;

	for (i = 0; i < conn->count_props; i++) {
		props = drmModeGetProperty(drmfd, conn->props[i]);
		if (props == NULL)
			continue;

		if (strcmp(props->name, "DPMS") == 0) {
			prop = props;
			break;
		}
		drmModeFreeProperty(props);
	}

	if (prop == NULL)
		return;

	drmModeConnectorSetProperty(drmfd, conn->connector_id, prop->prop_id,
	    level);
	drmModeFreeProperty(prop);
	mode = level;
}

static int
handle_keypress(xkb_keycode_t code, xkb_keysym_t sym, uint8_t *buf, int len)
{
	int cnt = 0, switchvt;

	/* Reset idle timeout */
	if (idleev != NULL)
		event_add(idleev, &idletv);

	if (sym == XKB_KEY_Print) {
		setdpms(DRM_MODE_DPMS_SUSPEND);
		return 0;
	} else {
		setdpms(DRM_MODE_DPMS_ON);
	}

	if ((switchvt = handle_vtswitch(sym)) > 0) {
		ioctl(ttyfd, VT_ACTIVATE, switchvt);
		return 0;
	}
	cnt = handle_term_special_keysym(sym, buf, len);
	if (cnt > 0)
		return cnt;

	/*
	 * XXX In X the left Alt key is an additional modifier key,
	 *     we might want to optionally emulate that behaviour.
	 */
	/* XXX handle composition (e.g. accents) */
	return fbteken_key_get_utf8(code, buf, len);
}

/* Just track all keys for now, to avoid stuck modifiers */
uint8_t pressed[256];
int npressed = 0;

static int
ispressed(uint8_t code)
{
	int i;

	for (i = 0; i < npressed; i++)
		if (pressed[i] == code)
			return 1;

	return 0;
}

static void
press(uint8_t code)
{
	if (!ispressed(code)) {
		pressed[npressed] = code;
		npressed++;
	}
}

static void
release(uint8_t code)
{
	int i;

	for (i = 0; i < npressed; i++) {
		if (pressed[i] == code) {
			npressed--;
			memcpy(&pressed[i], &pressed[i+1], npressed-i);
		}
	}
}

uint8_t lastread_code = 0;

/* Reading keyboard input from the tty which was set into raw mode */
static void
ttyread(evutil_socket_t fd __unused, short events __unused, void *arg __unused)
{
	int val;
	uint8_t buf[128];

	val = read(ttyfd, buf, sizeof(buf));
	if (val == 0) {
		return;
	} else if (val == -1) {
		perror("read");
		event_base_loopbreak(evbase);
	}

	uint8_t out[1024];
	xkb_keycode_t keycode = 0;
	xkb_keysym_t keysym;
	int newrep = 0;
	int i, n = 0;

	for (i = 0; i < val; i++) {
		keycode = at_toxkb(buf[i]);
		if (keycode == 0) {
			warnx("ignoring atscancode 0x%02x", buf[i]);
			continue;
		}

		keysym = xkb_state_key_get_one_sym(state, keycode);
		if (keycode == repkeycode && !at_ispress(buf[i])) {
			repkeycode = 0;
			repkeysym = 0;
		}
		if (xkb_keymap_key_repeats(keymap, keycode) &&
		    keycode != repkeycode && at_ispress(buf[i])) {
			repkeycode = keycode;
			repkeysym = keysym;
			newrep = 1;
		}

		if (lastread_code == buf[i] && at_ispress(buf[i]))
			continue;

		char name[10];
		xkb_keysym_get_name(keysym, name, 10);
#if 0
		printf("scancode=0x%02x keycode=0x%02x keysym=%s\n",
		    buf[i], keycode, name);
#endif
		if (at_ispress(buf[i])) {
			n += handle_keypress(keycode, keysym, &out[n],
			    sizeof(out) - n);
		}
		if (!(at_ispress(buf[i]) && ispressed(buf[i] & 0x7f))) {
			xkb_state_update_key(state, keycode,
			    at_ispress(buf[i]) ? XKB_KEY_DOWN : XKB_KEY_UP);
		}
		if (at_ispress(buf[i]))
			press(buf[i] & 0x7f);
		else
			release(buf[i] & 0x7f);

		lastread_code = buf[i];
	}

	if (repkeycode == 0)
		evtimer_del(repeatev);
	else if (newrep)
		evtimer_add(repeatev, &repdelay);

	if (n > 0)
		write(amaster, out, n);
}

static int
cmp_cells(int i)
{
	teken_attr_t a, b;

	a = termbuf1[i].attr;
	b = termbuf2[i].attr;

	if (termbuf1[i].ch != termbuf2[i].ch ||
	    termbuf1[i].cursor != termbuf2[i].cursor ||
	    a.ta_format != b.ta_format ||
	    a.ta_fgcolor != b.ta_fgcolor ||
	    a.ta_bgcolor != b.ta_bgcolor) {
		return 1;
	}

	return 0;
}

static void
handle_vblank(int fd __unused, unsigned int sequence __unused,
    unsigned int tv_sec __unused, unsigned int tv_usec __unused,
    void *user_data __unused)
{
	struct bufent *tmp;
	int idx;
	unsigned int i;

	if (dirtyflag) {
		for (i = 0; i < (unsigned)winsz.ws_col * winsz.ws_row; i++) {
			if (cmp_cells(i)) {
				termbuf1[i].dirty = 0;
				render_cell(i % winsz.ws_col,
				    i / winsz.ws_col);
			}
		}
	}
	for (i = 0; i < dirtycount; i++) {
		idx = dirtybuf[i];
		if (termbuf1[idx].dirty) {
			termbuf1[idx].dirty = 0;
			render_cell(idx % winsz.ws_col,
			    idx / winsz.ws_col);
		}
	}

	memcpy(termbuf2, termbuf1,
	    winsz.ws_col * winsz.ws_row * sizeof(*termbuf1));
	tmp = termbuf2;
	termbuf2 = termbuf1;
	termbuf1 = tmp;

	dirtycount = 0;
	dirtyflag = 0;
}

static void
drmread(evutil_socket_t fd __unused, short events __unused, void *arg __unused)
{
	drmEventContext evctx = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.vblank_handler = handle_vblank,
		.page_flip_handler = NULL
	};

	if (drmHandleEvent(drmfd, &evctx) != 0) {
		warnx("drmHandleEvent failed");
		event_base_loopbreak(evbase);
	}
}

static void
vtrelease(evutil_socket_t fd __unused, short events __unused,
    void *arg __unused)
{
	printf("vtleave\n");

	setdpms(DRM_MODE_DPMS_ON);
	lastread_code = 0;
	repkeycode = 0;
	repkeysym = 0;
	evtimer_del(repeatev);
	npressed = 0;
	xkb_state_unref(state);
	state = NULL;
	state = xkb_state_new(keymap);
	if (state == NULL)
		errx(1, "xkb_state_new failed");

	if (drmModeSetCrtc(drmfd, drmcrtc->crtc_id, oldbuffer_id, 0, 0, &drmconn->connector_id, 1, &drmcrtc->mode) != 0)
		perror("drmModeSetCrtc");
	if (drmDropMaster(drmfd) != 0)
		perror("drmDropMaster");
	ioctl(ttyfd, VT_RELDISP, VT_TRUE);
	active = false;
}

static void
vtacquire(evutil_socket_t fd __unused, short events __unused,
    void *arg __unused)
{
	printf("vtenter\n");
	ioctl(ttyfd, VT_RELDISP, VT_ACKACQ);
	ioctl(ttyfd, VT_ACTIVATE, vtnum);
	ioctl(ttyfd, VT_WAITACTIVE, vtnum);
	if (drmSetMaster(drmfd) != 0)
		perror("drmSetMaster");
	if (drmModeSetCrtc(drmfd, drmcrtc->crtc_id, drmfbid, 0, 0, &drmconn->connector_id, 1, &drmcrtc->mode) != 0)
		perror("drmModeSetCrtc");
	active = true;

	wait_vblank();
}

static void
handleterm(evutil_socket_t fd __unused, short events __unused,
    void *arg __unused)
{
	event_base_loopbreak(evbase);
}

struct xkb_rule_names names = {
	.rules = "evdev",
	.model = "pc104",
	.layout = "us",
	.variant = "",
	.options = ""
};

static void
xkb_init(char *layout, char *options, char *variant)
{
	ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (ctx == NULL)
		errx(1, "xkb_context_new failed");

	if (layout != NULL)
		names.layout = layout;
	if (options != NULL)
		names.options = options;
	if (variant != NULL)
		names.variant = variant;

	keymap = xkb_keymap_new_from_names(ctx, &names,
	    XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (keymap == NULL)
		errx(1, "xkb_keymap_new_from_names failed");

	state = xkb_state_new(keymap);
	if (state == NULL)
		errx(1, "xkb_state_new failed");
}

static void
xkb_finish(void)
{
	if (state != NULL) {
		xkb_state_unref(state);
		state = NULL;
	}
	if (keymap != NULL) {
		xkb_keymap_unref(keymap);
		keymap = NULL;
	}
	if (ctx != NULL) {
		xkb_context_unref(ctx);
		ctx = NULL;
	}
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-a | -A] [-d delay] [-r rate] [-f fontfile "
	    "[-F bold_fontfile]] [-i idle_timeout] [-s fontsize] "
	    "[-k kbd_layout] [-o kbd_options] [-h]\n", getprogname());
	exit(1);
}

int
main(int argc, char *argv[])
{
	int fd;
	uint32_t width, height;
	drmModeResPtr res;
	drmModeCrtcPtr crtc;
	drmModeEncoderPtr enc;
	char *shell;
	teken_pos_t winsize;
	char *normalfont = NULL, *boldfont = NULL;
	int i, ch;

	unsigned int fontheight = 16;
	bool alpha = true;
	char *kbd_layout = NULL, *kbd_options = NULL, *kbd_variant = NULL;

	const char *errstr;

	unsigned int repeat_delay = 200;
	unsigned int repeat_rate = 30;

	/* XXX handle bitmap fonts better */
	while ((ch = getopt(argc, argv, "aAd:r:f:F:i:k:o:v:s:h")) != -1) {
		switch (ch) {
		case 'a':
			alpha = true;
			break;
		case 'A':
			alpha = false;
			break;
		case 'd':
			repeat_delay = strtonum(optarg, 100, 2000, &errstr);
			if (errstr) {
				errx(1, "key repeat delay is %s: %s", errstr,
				    optarg);
			}
			break;
		case 'f':
			normalfont = optarg;
			break;
		case 'F':
			boldfont = optarg;
			break;
		case 'i':
			idle_timeout = strtonum(optarg, 30, 60*60*24, &errstr);
			if (errstr) {
				errx(1, "idle timeout is %s: %s", errstr,
				    optarg);
			}
			break;
		case 'k':
			kbd_layout = optarg;
			break;
		case 'o':
			kbd_options = optarg;
			break;
		case 'r':
			repeat_rate = strtonum(optarg, 1, 50, &errstr);
			if (errstr) {
				errx(1, "key repeat rate is %s: %s", errstr,
				    optarg);
			}
			break;
		case 's':
			fontheight = strtonum(optarg, 1, 128, &errstr);
			if (errstr) {
				errx(1, "font height is %s: %s", errstr,
				    optarg);
			}
			break;
		case 'v':
			kbd_variant = optarg;
			break;
		case 'h':
		default:
			usage();
		}
	}

	repdelay.tv_sec = repeat_delay / 1000;
	repdelay.tv_usec = (repeat_delay % 1000) * 1000;

	reprate.tv_sec = ((1000*1000) / repeat_rate) / (1000*1000);
	reprate.tv_usec = ((1000*1000) / repeat_rate) % (1000*1000);

	xkb_init(kbd_layout, kbd_options, kbd_variant);

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

	child = forkpty(&amaster, NULL, NULL, &winsz);
	if (child == -1) {
		err(EXIT_FAILURE, "forkpty");
	} else if (child == 0) {
		shell = getenv("SHELL");
		if (shell == NULL)
			shell = defaultshell;
		putenv(termenv);
		if (execlp(shell, basename(shell), NULL) == -1)
			err(EXIT_FAILURE, "execlp");
	}

	teken_init(&tek, &tek_funcs, NULL);
//	teken_set_defattr(&tek, &defattr);

	/* XXX handle errors (e.g. when invalid font paths are given) */
	rop = rop32_init(normalfont, boldfont, fontheight,
	    &fnwidth, &fnheight, alpha);

	fd = drmOpen("i915", NULL);
	if (fd < 0) {
		perror("drmOpen(\"i915\", NULL)");
		fd = drmOpen("radeon", NULL);
		if (fd < 0) {
			perror("drmOpen(\"radeon\", NULL)");
			exit(1);
		}
	}
	drmfd = fd;

	res = drmModeGetResources(fd);
	if (res == NULL) {
		perror("drmModeGetResources");
		exit(1);
	}
#if 0
	printf("resources: %x\n", res);
	printf("count_fbs: %d, count_crtcs: %d, count_connectors: %d, "
	    "min_width: %u, max_width: %u, min_height: %u, max_height: %u\n",
	    res->count_fbs, res->count_crtcs, res->count_connectors,
	    res->min_width, res->max_width, res->min_height, res->max_height);
#endif

	/* First take the first display output which is connected */
	for (i = 0; i < res->count_connectors; ++i) {
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if(conn->connection == DRM_MODE_CONNECTED)
			break;
	}
	if (res->count_connectors <= 0)
		errx(1, "No Monitor connected");
#if 0
	printf("conn->mmWidth: %u\n", conn->mmWidth);
	printf("conn->mmHeight: %u\n", conn->mmHeight);
	printf("conn->connector_id = %u\n", conn->connector_id);
	printf("conn->encoder_id = %u\n", conn->encoder_id);
	printf("conn->connector_type = %u\n", conn->connector_type);
	printf("conn->connector_type_id = %u\n", conn->connector_type_id);
	printf("conn->count_modes = %u\n", conn->count_modes);
	printf("conn->count_props = %u\n", conn->count_props);
	printf("conn->count_encoders = %u\n", conn->count_encoders);
	for (i = 0; i < conn->count_encoders; i++)
		printf("conn->encoders[%d] = %u\n", i, conn->encoders[i]);
#endif

	/* Using only the first encoder in conn->encoders for now */
	if (conn->count_encoders == 0)
		errx(1, "No encoders on this conection: conn->count_encoders == 0\n");
	else if (conn->count_encoders > 1)
		printf("Using the first encoder in conn->encoders\n");
	enc = drmModeGetEncoder(fd, conn->encoders[0]);
#if 0
	printf("enc->encoder_id = %u\n", enc->encoder_id);
	printf("enc->encoder_type = %u\n", enc->encoder_type);
	printf("enc->crtc_id = %u\n", enc->crtc_id);
	printf("enc->possible_crtcs = %u\n", enc->possible_crtcs);
	printf("enc->possible_clones = %u\n", enc->possible_clones);
#endif

	/*
	 * Just use the index of the lowest bit set in enc->possible_crtcs
	 * to select our crtc from res->crtcs.
	 */
	for (i = 0; i < MAX(32, res->count_crtcs); i++) {
		if (enc->possible_crtcs & (1U << i)) {
			crtc = drmModeGetCrtc(fd, res->crtcs[i]);
			if (crtc != NULL)
				break;
		}
	}
	if (i == res->count_crtcs)
		errx(1, "No usable crtc found in enc->possible_crtcs\n");
#if 0
	printf("crtc->crtc_id = %u\n", crtc->crtc_id);
	printf("crtc->buffer_id = %u\n", crtc->buffer_id);
	printf("crtc->width/height = %ux%u\n", crtc->width, crtc->height);
	printf("crtc->mode_valid = %u\n", crtc->mode_valid);
	printf("x: %u, y: %u\n", crtc->x, crtc->y);
#endif
	oldbuffer_id = crtc->buffer_id;

	/* Just use the first display mode given in conn->modes */
	/* XXX Allow the user to override the mode via a commandline argument */
	if (conn->count_modes == 0)
		errx(1, "No display mode specified in conn->modes\n");
	crtc->mode = conn->modes[0];
#if 0
	printf("Display mode:\n");
	printf("clock: %u\n", crtc->mode.clock);
	printf("vrefresh: %u\n", crtc->mode.vrefresh);
	printf("hdisplay: %u\n", crtc->mode.hdisplay);
	printf("hsync_start: %u\n", crtc->mode.hsync_start);
	printf("hsync_end: %u\n", crtc->mode.hsync_end);
	printf("htotal: %u\n", crtc->mode.htotal);
	printf("hskew: %u\n", crtc->mode.hskew);
	printf("vdisplay: %u\n", crtc->mode.vdisplay);
	printf("vsync_start: %u\n", crtc->mode.vsync_start);
	printf("vsync_end: %u\n", crtc->mode.vsync_end);
	printf("vtotal: %u\n", crtc->mode.vtotal);
	printf("vscan: %u\n", crtc->mode.vscan);
	printf("flags: %u\n", crtc->mode.flags);
	printf("type: %u\n", crtc->mode.type);
#endif

	width = crtc->mode.hdisplay;
	height = crtc->mode.vdisplay;

	kms_create(fd, &kms);

	unsigned bo_attribs[] = {
		KMS_WIDTH,	width,
		KMS_HEIGHT,	height,
		KMS_BO_TYPE,	KMS_BO_TYPE_SCANOUT_X8R8G8B8,
		KMS_TERMINATE_PROP_LIST
	};
	kms_bo_create(kms, bo_attribs, &bo);
	kms_bo_get_prop(bo, KMS_HANDLE, &handles[0]);
	kms_bo_get_prop(bo, KMS_PITCH, &pitches[0]);
#if 0
	printf("pitches[0] = %u\n", pitches[0]);
	printf("handles[0] = %u\n", handles[0]);
#endif
	offsets[0] = 0;
	kms_bo_map(bo, &plane);
	rop32_setclip(rop, (point){0,0}, (point){width-1,height-1});
	rop32_setcontext(rop, plane, width);
	drmModeAddFB2(fd, width, height, DRM_FORMAT_XRGB8888,
	    handles, pitches, offsets, &fb_id, 0);

	drmcrtc = crtc;
	drmconn = conn;
	drmfbid = fb_id;

	vtconfigure();
	drmModeSetCrtc(fd, crtc->crtc_id, fb_id, 0, 0, &conn->connector_id, 1, &crtc->mode);

	winsize.tp_col = width / fnwidth;
	winsize.tp_row = height / fnheight;
//	winsize.tp_col = 80;
//	winsize.tp_row = 25;
	teken_set_winsize(&tek, &winsize);
        winsz.ws_col = winsize.tp_col;
        winsz.ws_row = winsize.tp_row;
        winsz.ws_xpixel = winsz.ws_col * fnwidth;
        winsz.ws_ypixel = winsz.ws_row * fnheight;
	ioctl (amaster, TIOCSWINSZ, &winsz);
	termbuf1 = calloc(winsz.ws_col * winsz.ws_row, sizeof(struct bufent));
	termbuf2 = calloc(winsz.ws_col * winsz.ws_row, sizeof(struct bufent));
	dirtybuf = calloc(winsz.ws_col * winsz.ws_row, sizeof(uint32_t));
	keypad = 0;
	showcursor = 1;

	/* Resetting character cells to a default value */
	uint32_t k;
	for (k = 0; k < width * height; k++)
		((uint32_t *)plane)[k] = colormap[defattr.ta_bgcolor];
	for (i = 0; i < winsz.ws_col * winsz.ws_row; i++) {
		termbuf1[i].attr = defattr;
		termbuf1[i].ch = ' ';
		termbuf1[i].cursor = 0;
		termbuf1[i].dirty = 0;
	}
	memcpy(termbuf2, termbuf1,
	    winsz.ws_col * winsz.ws_row * sizeof(*termbuf1));

	struct event *masterev, *ttyev, *drmev, *vtrelev, *vtacqev, *sigintev;

	evbase = event_base_new();

	/*
	 * This design tries to keep things interactive for the user.
	 *
	 * Lowest priority:  5 idle timeout timer event
	 *     ||            4 input from the master fd of the pty device
	 *     ||            3 automatic key-repeat input from the terminal
	 *     ||            2 keyboard input from the terminal
	 *     ||            2 output to the master fd of the pty device
	 *     vv            1 vblank events from the drm device
	 * Highest priority: 0 signal handlers
	 */
	event_base_priority_init(evbase, 6);

	if (idle_timeout > 0) {
		idletv.tv_sec = idle_timeout;
		idleev = evtimer_new(evbase, handleidle, NULL);
		event_priority_set(idleev, 5);
	}

	masterev = event_new(evbase, amaster,
	    EV_READ | EV_PERSIST, rdmaster, NULL);
	event_priority_set(masterev, 4);

	repeatev = evtimer_new(evbase, keyrepeat, NULL);
	event_priority_set(repeatev, 3);

	ttyev = event_new(evbase, ttyfd,
	    EV_READ | EV_PERSIST, ttyread, NULL);
	event_priority_set(ttyev, 2);

	/* XXX event handler for writes to the master fd of the pty device */

	drmev = event_new(evbase, fd,
	    EV_READ | EV_PERSIST, drmread, NULL);
	event_priority_set(drmev, 1);

	vtrelev = evsignal_new(evbase, SIGUSR1, vtrelease, NULL);
	event_priority_set(vtrelev, 0);

	vtacqev = evsignal_new(evbase, SIGUSR2, vtacquire, NULL);
	event_priority_set(vtacqev, 0);

	signal(SIGINT, SIG_IGN);
	sigintev = evsignal_new(evbase, SIGINT, handleterm, NULL);
	event_priority_set(sigintev, 0);

	if (idleev != NULL)
		event_add(idleev, &idletv);
	event_add(masterev, NULL);
	event_add(ttyev, NULL);
	event_add(drmev, NULL);
	event_add(vtrelev, NULL);
	event_add(vtacqev, NULL);
	event_add(sigintev, NULL);

	event_base_loop(evbase, 0);
	signal(SIGINT, SIG_DFL);
	setdpms(DRM_MODE_DPMS_ON);

	event_del(sigintev);
	event_del(vtacqev);
	event_del(vtrelev);
	event_del(drmev);
	event_del(ttyev);
	event_del(repeatev);
	event_del(masterev);
	if (idleev != NULL)
		event_del(idleev);

	event_free(sigintev);
	event_free(vtacqev);
	event_free(vtrelev);
	event_free(drmev);
	event_free(ttyev);
	event_free(repeatev);
	event_free(masterev);
	if (idleev != NULL)
		event_free(idleev);
	event_base_free(evbase);

	free(termbuf1);
	free(termbuf2);

	drmModeSetCrtc(fd, crtc->crtc_id, oldbuffer_id, 0, 0,
	    &conn->connector_id, 1, &crtc->mode);
	vtdeconf();
	drmModeRmFB(fd, fb_id);

	kms_bo_unmap(bo);
	kms_bo_destroy(&bo);
	kms_destroy(&kms);

	drmModeFreeConnector(conn);
	drmModeFreeCrtc(crtc);
	drmModeFreeResources(res);
	drmClose(fd);

	xkb_finish();
}

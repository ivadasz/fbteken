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
#include <teken.h>

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

int ttyfd;
int drmfd;

struct xkb_context *ctx;
struct xkb_keymap *keymap;
struct xkb_state *state;

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
struct bufent *termbuf;
teken_pos_t cursorpos;
int keypad, showcursor;
uint32_t *dirtybuf, dirtycount = 0;
teken_attr_t defattr = {
	ta_format : 0,
	ta_fgcolor : TC_WHITE,
	ta_bgcolor : TC_BLACK,
};

struct event_base *evbase;

void
dirty_cell(uint16_t col, uint16_t row)
{
	if (!termbuf[row * winsz.ws_col + col].dirty) {
		termbuf[row * winsz.ws_col + col].dirty = 1;
		dirtybuf[dirtycount] = row * winsz.ws_col + col;
		dirtycount++;
	}
}

void
render_cell(uint16_t col, uint16_t row)
{
	struct bufent *cell;
	teken_pos_t pos;
	teken_attr_t *attr;
	teken_char_t ch;
	uint16_t sx, sy;
	uint32_t bg, fg, val;
	int cursor, flags = 0;

	cell = &termbuf[row * winsz.ws_col + col];
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
	if (fg >= 0 && fg < TC_NCOLORS) {
		if (attr->ta_format & TF_BOLD)
			fg = colormap[fg + TC_NCOLORS];
		else
			fg = colormap[fg];
	} else {
		fg = colormap[TC_WHITE];
		err(1, "color out of range: %d\n", attr->ta_fgcolor);
	}
	if (bg >= 0 && bg < TC_NCOLORS) {
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

void
set_cell(uint16_t col, uint16_t row, teken_char_t ch, const teken_attr_t *attr)
{
	teken_char_t oldch;
	teken_attr_t oattr;

	oldch = termbuf[row * winsz.ws_col + col].ch;
	if (ch == oldch) {
		oattr = termbuf[row * winsz.ws_col + col].attr;
		if (oattr.ta_format == attr->ta_format &&
		    oattr.ta_fgcolor == attr->ta_fgcolor &&
		    oattr.ta_bgcolor == attr->ta_bgcolor)
			return;
	}
	termbuf[row * winsz.ws_col + col].ch = ch;
	termbuf[row * winsz.ws_col + col].attr = *attr;
	dirty_cell(col, row);
}

void
fbteken_bell(void *thunk)
{
	/* XXX */
}

void
fbteken_cursor(void *thunk, const teken_pos_t *pos)
{
	uint16_t sx, sy;

	if (cursorpos.tp_col == pos->tp_col && cursorpos.tp_row == pos->tp_row)
		return;
	cursorpos = *pos;
}

void
fbteken_putchar(void *thunk, const teken_pos_t *pos, teken_char_t ch,
    const teken_attr_t *attr)
{
	set_cell(pos->tp_col, pos->tp_row, ch, attr);
}

void
fbteken_fill(void *thunk, const teken_rect_t *rect, teken_char_t ch,
    const teken_attr_t *attr)
{
	teken_unit_t a, b;

	for (a = rect->tr_begin.tp_row; a < rect->tr_end.tp_row; a++) {
		for (b = rect->tr_begin.tp_col; b < rect->tr_end.tp_col; b++) {
			set_cell(b, a, ch, attr);
		}
	}
}

void
fbteken_copy(void *thunk, const teken_rect_t *rect, const teken_pos_t *pos)
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
				ch = termbuf[(srow + a) * winsz.ws_col + scol + b].ch;
				attr = &termbuf[(srow + a) * winsz.ws_col + scol + b].attr;
				set_cell(tcol + b, trow + a, ch, attr);
			}
		}
	} else if (scol >= tcol && srow < trow) {
		for (a = h - 1; a >= 0; a--) {
			for (b = 0; b < w; b++) {
				ch = termbuf[(srow + a) * winsz.ws_col + scol + b].ch;
				attr = &termbuf[(srow + a) * winsz.ws_col + scol + b].attr;
				set_cell(tcol + b, trow + a, ch, attr);
			}
		}
	} else if (scol < tcol && srow >= trow) {
		for (a = 0; a < h; a++) {
			for (b = w - 1; b >= 0; b--) {
				ch = termbuf[(srow + a) * winsz.ws_col + scol + b].ch;
				attr = &termbuf[(srow + a) * winsz.ws_col + scol + b].attr;
				set_cell(tcol + b, trow + a, ch, attr);
			}
		}
	} else if (scol >= tcol && srow >= trow) {
		for (a = 0; a < h; a++) {
			for (b = 0; b < w; b++) {
				ch = termbuf[(srow + a) * winsz.ws_col + scol + b].ch;
				attr = &termbuf[(srow + a) * winsz.ws_col + scol + b].attr;
				set_cell(tcol + b, trow + a, ch, attr);
			}
		}
	}
}

void
fbteken_param(void *thunk, int param, unsigned int val)
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
fbteken_respond(void *thunk, const void *arg, size_t sz)
{
	/* XXX */
}

void
vtleave(int signum)
{
	printf("vtleave\n");
	if (drmModeSetCrtc(drmfd, drmcrtc->crtc_id, oldbuffer_id, 0, 0, &drmconn->connector_id, 1, &drmcrtc->mode) != 0)
		perror("drmModeSetCrtc");
	if (drmDropMaster(drmfd) != 0)
		perror("drmDropMaster");
	ioctl(ttyfd, VT_RELDISP, VT_TRUE);
	active = false;
}

void
vtenter(int signum)
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
}

/* XXX Create a new free vty like Xorg does */
void
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
	signal(SIGUSR1, vtleave);
	signal(SIGUSR2, vtenter);
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

void
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
	signal(SIGUSR1, SIG_IGN);
	signal(SIGUSR2, SIG_IGN);
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

void
rdmaster(evutil_socket_t fd, short events, void *arg)
{
	int i, val;
	char s[0x1000];
	teken_pos_t oc;
	uint32_t prevdirty;

	val = read(amaster, s, 0x1000);
	if (val > 0) {
		prevdirty = dirtycount;
		oc = cursorpos;
		teken_input(&tek, s, val);
		if (oc.tp_col != cursorpos.tp_col ||
		    oc.tp_row != cursorpos.tp_row) {
			termbuf[oc.tp_row * winsz.ws_col + oc.tp_col].cursor = 0;
			termbuf[cursorpos.tp_row * winsz.ws_col + cursorpos.tp_col].cursor = 1;
			dirty_cell(oc.tp_col, oc.tp_row);
			dirty_cell(cursorpos.tp_col, cursorpos.tp_row);
		}
		if (dirtycount > 0 && prevdirty == 0) {
			drmVBlank req = {
				.request.type = _DRM_VBLANK_RELATIVE |
						_DRM_VBLANK_EVENT,
				.request.sequence = 1,
				.request.signal = 0
			};
			drmWaitVBlank(drmfd, &req);
		}
	} else {
		event_base_loopbreak(evbase);
	}
}

/* Reading keyboard input from the tty which was set into raw mode */
void
ttyread(evutil_socket_t fd, short events, void *arg)
{
	int val;
	uint8_t buf[128];
	const char *str;

	val = read(ttyfd, buf, sizeof(buf));
	if (val == 0) {
		return;
	} else if (val == -1) {
		perror("read");
		event_base_loopbreak(evbase);
	}

#ifndef __linux__
	int i, n = 0, sz;
	uint8_t out[1024];
	xkb_keycode_t keycode;
	xkb_keysym_t keysym;
	for (i = 0; i < val; i++) {
		keycode = buf[i];
		printf("keycode=0x%02x\n", buf[i]);
		keysym = xkb_state_key_get_one_sym(state, keycode);
		n += xkb_state_key_get_utf8(state, keycode, &out[n], sizeof(out) - n);
	}
	if (n > 0)
		write(amaster, out, n);
#endif

#ifdef __linux__
	/*
	 * XXX This was my first attempt to correctly handle the
	 *     escape codes in keyboard input (e.g. for keypad mode)
	*/
	if (val >= 2 && buf[0] == 0x1b && buf[1] == '[') {
#if 0
		printf("buf[0] = %x, buf[1] = %x, buf[2] = %x "
		    "buf[3] = %x buf[4] = %x\n",
		    buf[0], buf[1], buf[2], buf[3], buf[4]);
#endif
		if (val == 3 && buf[2] == 'A') {
			str = teken_get_sequence(&tek, TKEY_UP);
			write(amaster, str, strlen(str));
			return;
		} else if (val == 3 && buf[2] == 'B') {
			str = teken_get_sequence(&tek, TKEY_DOWN);
			write(amaster, str, strlen(str));
			return;
		} else if (val == 3 && buf[2] == 'C') {
			str = teken_get_sequence(&tek, TKEY_RIGHT);
			write(amaster, str, strlen(str));
			return;
		} else if (val == 3 && buf[2] == 'D') {
			str = teken_get_sequence(&tek, TKEY_LEFT);
			write(amaster, str, strlen(str));
			return;
		} else if (val == 4 && buf[2] == '[' &&
			   buf[3] == 'A') {
			str = teken_get_sequence(&tek, TKEY_F1);
			write(amaster, str, strlen(str));
			return;
		} else if (val == 4 && buf[2] == '[' &&
			   buf[3] == 'B') {
			str = teken_get_sequence(&tek, TKEY_F2);
			write(amaster, str, strlen(str));
			return;
		} else if (val == 4 && buf[2] == '[' &&
			   buf[3] == 'C') {
			str = teken_get_sequence(&tek, TKEY_F3);
			write(amaster, str, strlen(str));
			return;
		} else if (val == 4 && buf[2] == '[' &&
			   buf[3] == 'D') {
			str = teken_get_sequence(&tek, TKEY_F4);
			write(amaster, str, strlen(str));
			return;
		} else if (val == 4 && buf[2] == '[' &&
			   buf[3] == 'E') {
			str = teken_get_sequence(&tek, TKEY_F5);
			write(amaster, str, strlen(str));
			return;
		} else if (val == 5 && buf[2] == '1' &&
			   buf[3] == '7' && buf[4] == '~') {
			str = teken_get_sequence(&tek, TKEY_F6);
			write(amaster, str, strlen(str));
			return;
		} else if (val == 5 && buf[2] == '1' &&
			   buf[3] == '8' && buf[4] == '~') {
			str = teken_get_sequence(&tek, TKEY_F7);
			write(amaster, str, strlen(str));
			return;
		} else if (val == 5 && buf[2] == '1' &&
			   buf[3] == '9' && buf[4] == '~') {
			str = teken_get_sequence(&tek, TKEY_F8);
			write(amaster, str, strlen(str));
			return;
		} else if (val == 5 && buf[2] == '2' &&
			   buf[3] == '0' && buf[4] == '~') {
			str = teken_get_sequence(&tek, TKEY_F9);
			write(amaster, str, strlen(str));
			return;
		} else if (val == 5 && buf[2] == '2' &&
			   buf[3] == '1' && buf[4] == '~') {
			str = teken_get_sequence(&tek, TKEY_F10);
			write(amaster, str, strlen(str));
			return;
		} else if (val == 5 && buf[2] == '2' &&
			   buf[3] == '3' && buf[4] == '~') {
			str = teken_get_sequence(&tek, TKEY_F11);
			write(amaster, str, strlen(str));
			return;
		} else if (val == 5 && buf[2] == '2' &&
			   buf[3] == '4' && buf[4] == '~') {
			str = teken_get_sequence(&tek, TKEY_F12);
			write(amaster, str, strlen(str));
			return;
		}
	}
	write(amaster, buf, val);
#endif	/* defined(__linux__) */
}

void
handle_vblank(int fd, unsigned int sequence, unsigned int tv_sec,
    unsigned int tv_usec, void *user_data)
{
	int i, idx;

	for (i = 0; i < dirtycount; i++) {
		idx = dirtybuf[i];
		if (termbuf[idx].dirty) {
			termbuf[idx].dirty = 0;
			render_cell(idx % winsz.ws_col, idx / winsz.ws_col);
		}
	}
	dirtycount = 0;
}

void
drmread(evutil_socket_t fd, short events, void *arg)
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

struct xkb_rule_names names = {
	.rules = "xorg",
	.model = "pc102",
	.layout = "de",
	.variant = "",
	.options = "ctrl:nocaps"
};

void
xkb_init(void)
{
	ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (ctx == NULL)
		errx(1, "xkb_context_new failed");

	keymap = xkb_keymap_new_from_names(ctx, &names,
	    XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (keymap == NULL)
		errx(1, "xkb_keymap_new_from_names failed");

	state = xkb_state_new(keymap);
	if (state == NULL)
		errx(1, "xkb_state_new failed");
}

int
main(int argc, char *argv[])
{
	int i, ret, fd, fbid;
	uint32_t width, height;
	drmModeResPtr res;
	drmModeCrtcPtr crtc, vgacrtc;
	drmModeConnectorPtr conn, vgaconn;
	drmModeEncoderPtr enc, vgaenc;
	drmModeFBPtr fb;
	char *shell;
	teken_pos_t winsize;

	xkb_init();

	/*
	 * Font settings. alpha=true can only be used for truetype fonts
	 * to enable antialiased font rendering.
	 */
//	char *normalfont = "/usr/local/lib/X11/fonts/dejavu/DejaVuSansMono.ttf";
//	char *boldfont = "/usr/local/lib/X11/fonts/dejavu/DejaVuSansMono-Bold.ttf";
	char *normalfont = "/usr/local/share/fonts/dejavu/DejaVuSansMono.ttf";
	char *boldfont = "/usr/local/share/fonts/dejavu/DejaVuSansMono-Bold.ttf";
	unsigned int fontheight = 16;
	bool alpha = true;
#if 0
	/* e.g. a bitmap font */
	char *normalfont = "/usr/local/lib/X11/fonts/misc/9x15.pcf.gz";
	char *boldfont = "/usr/local/lib/X11/fonts/misc/9x15.pcf.gz";
	unsigned int fontheight = 15;
	bool alpha = false;
#endif

	child = forkpty(&amaster, NULL, NULL, &winsz);
	if (child == -1) {
		err(EXIT_FAILURE, "forkpty");
	} else if (child == 0) {
		shell = getenv("SHELL");
		if (shell == NULL)
			shell = "/bin/sh";
		putenv("TERM=xterm");
		if (execlp(shell, basename(shell), NULL) == -1)
			err(EXIT_FAILURE, "execlp");
	}

	teken_init(&tek, &tek_funcs, NULL);
//	teken_set_defattr(&tek, &defattr);

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
	printf("count_fbs: %d, count_crtcs: %d, count_connectors: %d, min_width: %u, max_width: %u, min_height: %u, max_height: %u\n", res->count_fbs, res->count_crtcs, res->count_connectors, res->min_width, res->max_width, res->min_height, res->max_height);
#endif

	/* First take the first display output which is connected */
	for (i = 0; i < res->count_connectors; ++i) {
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if(conn->connection == DRM_MODE_CONNECTED)
			break;
	}
	if (i == res->count_connectors)
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
	termbuf = calloc(winsz.ws_col * winsz.ws_row, sizeof(struct bufent));
	dirtybuf = calloc(winsz.ws_col * winsz.ws_row, sizeof(uint32_t));
	keypad = 0;
	showcursor = 1;

	/* Resetting character cells to a default value */
	for (i = 0; i < width * height; i++)
		((uint32_t *)plane)[i] = colormap[defattr.ta_bgcolor];
	for (i = 0; i < winsz.ws_col * winsz.ws_row; i++) {
		termbuf[i].attr = defattr;
		termbuf[i].ch = ' ';
		termbuf[i].cursor = 0;
		termbuf[i].dirty = 0;
	}

	struct event *masterev, *ttyev, *drmev;

	evbase = event_base_new();

	/*
	 * This design tries to keep things interactive for the user.
	 *
	 * Lowest priority:  3 input from the master fd of the pty device
	 *     ||            2 keyboard input from the terminal
	 *     ||            2 output to the master fd of the pty device
	 *     vv            1 vblank events from the drm device
	 * Highest priority: 0 signal handlers
	 */
	event_base_priority_init(evbase, 4),

	masterev = event_new(evbase, amaster,
	    EV_READ | EV_PERSIST, rdmaster, NULL);
	event_priority_set(masterev, 3);

	ttyev = event_new(evbase, ttyfd,
	    EV_READ | EV_PERSIST, ttyread, NULL);
	event_priority_set(ttyev, 2);

	/* XXX event handler for writes to the master fd of the pty device */

	drmev = event_new(evbase, fd,
	    EV_READ | EV_PERSIST, drmread, NULL);
	event_priority_set(drmev, 1);

	/* XXX create event for the signal handler for VT switching? */

	event_add(masterev, NULL);
	event_add(ttyev, NULL);
	event_add(drmev, NULL);

	event_base_loop(evbase, 0);

	event_del(drmev);
	event_del(ttyev);
	event_del(masterev);

	event_free(drmev);
	event_free(ttyev);
	event_free(masterev);
	event_base_free(evbase);

	drmModeSetCrtc(fd, crtc->crtc_id, oldbuffer_id, 0, 0, &conn->connector_id, 1, &crtc->mode);
	vtdeconf();
	drmModeRmFB(fd, fb_id);

	kms_bo_unmap(bo);
	kms_bo_destroy(&bo);
	kms_destroy(&kms);

	drmModeFreeConnector(conn);
	drmModeFreeCrtc(crtc);
	drmModeFreeResources(res);
	drmClose(fd);
}

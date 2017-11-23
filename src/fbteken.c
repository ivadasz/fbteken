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
#include <errno.h>
#ifdef __linux__
#include <pty.h>
#endif
#include <libgen.h>
#include <libutil.h>
#include <pthread.h>
#include <termios.h>

#include <sys/param.h>
#include <sys/stat.h>
#ifdef __linux__
#include <linux/vt.h>
#else
#include <sys/ioctl.h>
#include <sys/consio.h>
#endif

#include <libkms/libkms.h>
#include <drm_fourcc.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <event2/event.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#include <kbdev.h>
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
void	fbteken_copy(void *thunk, const teken_rect_t *rect,
	    const teken_pos_t *pos);
void	fbteken_param(void *thunk, int param, unsigned int val);
void	fbteken_respond(void *thunk, const void *arg, size_t sz);

struct drm_framebuffer {
	struct kms_bo *bo;
	unsigned handles[4], pitches[4], offsets[4];
	void *plane;
	uint32_t width, height;
	uint32_t fbid;
};

struct drm_state {
	int fd;
	struct kms_driver *kms;
	drmModeCrtcPtr crtc;
	drmModeConnectorPtr conn;
	int oldbuffer_id;
	int dpms_mode;
};

static int	handle_term_special_keysym(xkb_keysym_t sym, uint8_t *buf,
					   size_t len);
static void	xkb_reset(void);
static int	drm_backend_show(struct drm_state *dst,
				 struct drm_framebuffer *fb);
static int	drm_backend_hide(struct drm_state *dst);
static void	drm_set_dpms(struct drm_state *dst, int level);

int ttyfd;

struct kbdev_state *kbdst;

struct xkb_context *ctx;
struct xkb_keymap *keymap;
struct xkb_state *state;
struct xkb_compose_table *comptable;
struct xkb_compose_state *compstate;

int idle_timeout = 0;	/* idle timeout (in s) */

struct drm_state gfxstate;
struct drm_framebuffer framebuffer;

int vtnum;
#ifndef __linux__
int initialvtnum;
#endif
bool active = true;

struct rop_obj *rop;
int fnwidth, fnheight, fnpivot;
struct termios origtios;

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

struct event_base *evbase;
struct event *idleev;

/* idle timeout */
struct timeval idletv = { .tv_sec = 0, .tv_usec = 0 };

/* Synchronize xkbcommon keyboard LED state to the hardware keyboard */
static void
update_kbd_leds(void)
{
	int ledstate = 0;

	if (xkb_state_led_name_is_active(state, XKB_LED_NAME_CAPS))
		ledstate |= (1 << 0);
	if (xkb_state_led_name_is_active(state, XKB_LED_NAME_NUM))
		ledstate |= (1 << 1);
	if (xkb_state_led_name_is_active(state, XKB_LED_NAME_SCROLL))
		ledstate |= (1 << 2);

	kbdev_set_leds(kbdst, ledstate);
}

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

	switch (fnpivot) {
	case 0:
	default:
		sx = col * fnwidth;
		sy = row * fnheight;
		break;
	case 1:
		sx = row * fnheight;
		sy = framebuffer.height - col * fnwidth;
		break;
	case 2:
		sx = framebuffer.width  - col * fnwidth;
		sy = framebuffer.height - row * fnheight;
		break;
	case 3:
		sx = framebuffer.width  - row * fnheight;
		sy = col * fnwidth;
		break;
	}
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
	switch (fnpivot) {
	case 0:
	default:
		rop32_rect(rop, (point){sx, sy},
		    (dimension){fnwidth, fnheight}, bg);
		break;
	case 1:
		rop32_rect(rop, (point){sx, sy - fnwidth},
		    (dimension){fnheight, fnwidth}, bg);
		break;
	case 2:
		rop32_rect(rop, (point){sx - fnwidth, sy - fnheight},
		    (dimension){fnwidth, fnheight}, bg);
		break;
	case 3:
		rop32_rect(rop, (point){sx - fnheight, sy},
		    (dimension){fnheight, fnwidth}, bg);
		break;
	}
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

	snprintf(vtname, sizeof(vtname), "/dev/ttyv%01x", vtno - 1);

	fd = open(vtname, O_RDWR);
	if (fd < 0) {
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
	if (ret != 0) {
		printf("ioctl VT_SETMODE failed\n");
	}
#ifdef __linux__
	ret = ioctl(fd, VT_GETSTATE, &s);
	if (ret != 0) {
		printf("ioctl VT_GETSTATE failed\n");
	}
	vtnum = s.v_active;
#else
	ret = ioctl(fd, VT_GETACTIVE, &vtnum);
	if (ret != 0) {
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

	set_nonblocking(fd);

	/* Initialize Keyboard stuff */
	kbdst = kbdev_new_state(fd);
	if (kbdst == NULL)
		warn("kbdev_new_state");
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
#endif
	kbdev_destroy_state(kbdst);
	kbdst = NULL;

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
		drmWaitVBlank(gfxstate.fd, &req);
	}
}

static void
handleidle(evutil_socket_t fd __unused, short events __unused,
    void *arg __unused)
{
	drm_set_dpms(&gfxstate, DRM_MODE_DPMS_SUSPEND);

	if (active)
		event_add(idleev, &idletv);
}

static void
rdmaster(evutil_socket_t fd __unused, short events __unused, void *arg)
{
	struct terminal *t = (struct terminal *)arg;
	char s[0x1000];
	teken_pos_t oc;
	uint32_t prevdirty, prevdirtyflag;
	int val;

	val = read(t->amaster, s, 0x1000);
	if (val > 0) {
		prevdirty = dirtycount;
		prevdirtyflag = dirtyflag;
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
		if (prevdirty == 0 || prevdirtyflag == 0)
			wait_vblank();
	} else if (val == 0 || errno != EAGAIN) {
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

static int
do_handle_keysym(xkb_keysym_t sym, xkb_keycode_t code, uint8_t *buf, int len)
{
	enum xkb_compose_feed_result feedres;
	enum xkb_compose_status compres;
	int n;

	if (compstate == NULL)
		goto nocompose;

	feedres = xkb_compose_state_feed(compstate, sym);
	if (feedres == XKB_COMPOSE_FEED_IGNORED)
		goto nocompose;

	compres = xkb_compose_state_get_status(compstate);
	if (compres == XKB_COMPOSE_CANCELLED) {
		xkb_compose_state_reset(compstate);
		return 0;
	} else if (compres == XKB_COMPOSE_COMPOSING) {
		return 0;
	} else if (compres == XKB_COMPOSE_COMPOSED) {
		sym = xkb_compose_state_get_one_sym(compstate);
		n = handle_term_special_keysym(sym, buf, len);
		if (n > 0)
			return n;
		return xkb_compose_state_get_utf8(compstate, buf, len);
	}

nocompose:
	n = handle_term_special_keysym(sym, buf, len);
	if (n > 0)
		return n;

	/*
	 * XXX In X the left Alt key is an additional modifier key,
	 *     we might want to optionally emulate that behaviour.
	 */
	return fbteken_key_get_utf8(code, buf, len);
}

/* Key repeat handling */
static void
keyrepeat(evutil_socket_t fd __unused, short events __unused,
    void *arg __unused)
{
	uint8_t out[16];
	int n;

	if (repkeycode != 0) {
		n = do_handle_keysym(repkeysym, repkeycode, out, sizeof(out));
		evtimer_add(repeatev, &reprate);
		if (n > 0) {
			/* XXX Make sure we write everything */
			write(curterm->amaster, out, n);
		}
	}
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

static void
drm_set_dpms(struct drm_state *dst, int level)
{
	int i;
	drmModePropertyPtr prop = NULL, props;

	if (level == dst->dpms_mode)
		return;

	for (i = 0; i < dst->conn->count_props; i++) {
		props = drmModeGetProperty(dst->fd, dst->conn->props[i]);
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

	drmModeConnectorSetProperty(dst->fd, dst->conn->connector_id,
	    prop->prop_id, level);
	drmModeFreeProperty(prop);
	dst->dpms_mode = level;
}

static int
handle_keypress(xkb_keycode_t code, xkb_keysym_t sym, uint8_t *buf, int len)
{
	int switchvt;

	/* Reset idle timeout */
	if (idleev != NULL && active)
		event_add(idleev, &idletv);

	if (sym == XKB_KEY_Print) {
		drm_set_dpms(&gfxstate, DRM_MODE_DPMS_SUSPEND);
		return 0;
	} else {
		drm_set_dpms(&gfxstate, DRM_MODE_DPMS_ON);
	}

	if ((switchvt = handle_vtswitch(sym)) > 0) {
		ioctl(ttyfd, VT_ACTIVATE, switchvt);
		return 0;
	}
	return do_handle_keysym(sym, code, buf, len);
}

/* Reading keyboard input from the tty which was set into raw mode */
static void
ttyread(evutil_socket_t fd __unused, short events __unused, void *arg __unused)
{
	int val;
	struct kbdev_event evs[64];

	val = kbdev_read_events(kbdst, evs, NELEM(evs));
	if (val == 0) {
		return;
	} else if (val == -1) {
		perror("read");
		event_base_loopbreak(evbase);
	}

	uint8_t out[1024];
	xkb_keycode_t keycode = 0;
	xkb_keysym_t keysym;
	enum xkb_state_component stcomp;
	int newrep = 0;
	int i, n = 0;

	for (i = 0; i < val; i++) {
		keycode = evs[i].keycode + 8;
		if (keycode == 0)
			continue;

		keysym = xkb_state_key_get_one_sym(state, keycode);
		if (keycode == repkeycode && !evs[i].pressed) {
			repkeycode = 0;
			repkeysym = 0;
		}
		if (xkb_keymap_key_repeats(keymap, keycode) &&
		    keycode != repkeycode && evs[i].pressed) {
			repkeycode = keycode;
			repkeysym = keysym;
			newrep = 1;
		}

#if 0
		char name[32];
		xkb_keysym_get_name(keysym, name, sizeof(name));
		printf("scancode=0x%02x keycode=0x%02x keysym=%s\n",
		    buf[i], keycode, name);
#endif
		if (evs[i].pressed) {
			n += handle_keypress(keycode, keysym, &out[n],
			    sizeof(out) - n);
		}
		stcomp = xkb_state_update_key(state, keycode,
		    evs[i].pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
		if (stcomp & XKB_STATE_LEDS) {
			update_kbd_leds();
		}
	}

	if (repkeycode == 0)
		evtimer_del(repeatev);
	else if (newrep)
		evtimer_add(repeatev, &repdelay);

	if (n > 0) {
		/* XXX Make sure we write everything */
		write(curterm->amaster, out, n);
	}
}

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

static void
redraw_term(struct terminal *t)
{
	int idx;
	unsigned int i, cols, rows;

	cols = t->winsz.ws_col;
	rows = t->winsz.ws_row;
	if (dirtyflag) {
		for (i = 0; i < cols * rows; i++) {
			t->buf[i].dirty = 0;
			if (cmp_cells(t, i)) {
				render_cell(t, i % cols, i / cols);
			}
		}
	} else {
		for (i = 0; i < dirtycount; i++) {
			idx = dirtybuf[i];
//			if (t->buf[idx].dirty) {
				t->buf[idx].dirty = 0;
				render_cell(t, idx % cols, idx / cols);
//			}
		}
		for (i = 0; i < cols * rows; i++) {
			t->buf[i].dirty = 0;
		}
	}

	memcpy(oldbuf, t->buf, cols * rows * sizeof(*t->buf));

	dirtycount = 0;
	dirtyflag = 0;
}

static void
handle_vblank(int fd __unused, unsigned int sequence __unused,
    unsigned int tv_sec __unused, unsigned int tv_usec __unused,
    void *user_data __unused)
{
	redraw_term(curterm);
}

static void
drmread(evutil_socket_t fd __unused, short events __unused, void *arg __unused)
{
	drmEventContext evctx = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.vblank_handler = handle_vblank,
		.page_flip_handler = NULL
	};

	if (drmHandleEvent(gfxstate.fd, &evctx) != 0) {
		warnx("drmHandleEvent failed");
		event_base_loopbreak(evbase);
	}
}

static void
vtrelease(evutil_socket_t fd __unused, short events __unused,
    void *arg __unused)
{
	printf("vtleave\n");

	drm_set_dpms(&gfxstate, DRM_MODE_DPMS_ON);
	repkeycode = 0;
	repkeysym = 0;
	evtimer_del(repeatev);
	if (idleev != NULL)
		event_del(idleev);
	kbdev_reset_state(kbdst);
	xkb_reset();
	update_kbd_leds();

	drm_backend_hide(&gfxstate);
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
	drm_backend_show(&gfxstate, &framebuffer);
	active = true;
	if (idleev != NULL)
		event_add(idleev, &idletv);

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

	const char *locale = NULL;
	locale = getenv("LC_ALL");
	if (!locale)
		locale = getenv("LC_CTYPE");
	if (!locale)
		locale = getenv("LANG");
	if (!locale)
		locale = "C";
	comptable = xkb_compose_table_new_from_locale(ctx, locale,
	    XKB_COMPOSE_COMPILE_NO_FLAGS);
	if (comptable != NULL) {
		compstate = xkb_compose_state_new(comptable,
		    XKB_COMPOSE_STATE_NO_FLAGS);
	}
}

static void
xkb_finish(void)
{
	xkb_compose_state_unref(compstate);
	compstate = NULL;
	xkb_compose_table_unref(comptable);
	comptable = NULL;
	xkb_state_unref(state);
	state = NULL;
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
xkb_reset(void)
{
	if (state != NULL)
		xkb_state_unref(state);
	state = NULL;
	state = xkb_state_new(keymap);
	if (state == NULL)
		errx(1, "xkb_state_new failed");
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-a | -A] [-hw] [-d delay] [-r rate] [-f fontfile "
	    "[-F bold_fontfile]] [-i idle_timeout] [-s fontsize] "
	    "[-k kbd_layout] [-o kbd_options] [-v kbd_variant]\n",
	    getprogname());
	exit(1);
}

static int
drm_backend_init(struct drm_state *dst)
{
	drmModeResPtr res;
	drmModeEncoderPtr enc;
	int fd, i;

	fd = drmOpen("i915", NULL);
	if (fd < 0) {
		perror("drmOpen(\"i915\", NULL)");
		fd = drmOpen("radeon", NULL);
		if (fd < 0) {
			perror("drmOpen(\"radeon\", NULL)");
			return 1;
		}
	}

	dst->fd = fd;
	dst->dpms_mode = DRM_MODE_DPMS_ON;

	res = drmModeGetResources(fd);
	if (res == NULL) {
		warn("drmModeGetResources");
		return 1;
	}
#if 0
	printf("count_fbs: %d, count_crtcs: %d, count_connectors: %d, "
	    "min_width: %u, max_width: %u, min_height: %u, max_height: %u\n",
	    res->count_fbs, res->count_crtcs, res->count_connectors,
	    res->min_width, res->max_width, res->min_height, res->max_height);
#endif

	/* First take the first display output which is connected */
	for (i = 0; i < res->count_connectors; ++i) {
		dst->conn = drmModeGetConnector(fd, res->connectors[i]);
		if(dst->conn->connection == DRM_MODE_CONNECTED)
			break;
	}
	if (res->count_connectors <= 0) {
		warnx("No Monitor connected");
		return 1;
	}
#if 0
	printf("dst->conn->mmWidth: %u\n", dst->conn->mmWidth);
	printf("dst->conn->mmHeight: %u\n", dst->conn->mmHeight);
	printf("dst->conn->connector_id = %u\n", dst->conn->connector_id);
	printf("dst->conn->encoder_id = %u\n", dst->conn->encoder_id);
	printf("dst->conn->connector_type = %u\n", dst->conn->connector_type);
	printf("dst->conn->connector_type_id = %u\n", dst->conn->connector_type_id);
	printf("dst->conn->count_modes = %u\n", dst->conn->count_modes);
	printf("dst->conn->count_props = %u\n", dst->conn->count_props);
	printf("dst->conn->count_encoders = %u\n", dst->conn->count_encoders);
	for (i = 0; i < dst->conn->count_encoders; i++)
		printf("gfxstate.conn->encoders[%d] = %u\n", i, gfxstate.conn->encoders[i]);
#endif

	/* Using only the first encoder in gfxstate.conn->encoders for now */
	if (gfxstate.conn->count_encoders == 0) {
		warnx("No encoders on this conection\n");
		return 1;
	} else if (gfxstate.conn->count_encoders > 1) {
		printf("Using the first encoder in gfxstate.conn->encoders\n");
	}
	enc = drmModeGetEncoder(fd, gfxstate.conn->encoders[0]);
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
			gfxstate.crtc = drmModeGetCrtc(fd, res->crtcs[i]);
			if (gfxstate.crtc != NULL)
				break;
		}
	}
	if (i == res->count_crtcs) {
		warnx("No usable crtc found in enc->possible_crtcs\n");
		return 1;
	}

	drmModeFreeResources(res);
	drmModeFreeEncoder(enc);

#if 0
	printf("gfxstate.crtc->crtc_id = %u\n", gfxstate.crtc->crtc_id);
	printf("gfxstate.crtc->buffer_id = %u\n", gfxstate.crtc->buffer_id);
	printf("gfxstate.crtc->width/height = %ux%u\n", gfxstate.crtc->width, gfxstate.crtc->height);
	printf("gfxstate.crtc->mode_valid = %u\n", gfxstate.crtc->mode_valid);
	printf("x: %u, y: %u\n", crtc->x, gfxstate.crtc->y);
#endif
	gfxstate.oldbuffer_id = gfxstate.crtc->buffer_id;

	/* Just use the first display mode given in gfxstate.conn->modes */
	/* XXX Allow the user to override the mode via a commandline argument */
	if (gfxstate.conn->count_modes == 0) {
		warnx("No display mode specified in gfxstate.conn->modes\n");
		return 1;
	}
	gfxstate.crtc->mode = gfxstate.conn->modes[0];
#if 0
	printf("Display mode:\n");
	printf("clock: %u\n", gfxstate.crtc->mode.clock);
	printf("vrefresh: %u\n", gfxstate.crtc->mode.vrefresh);
	printf("hdisplay: %u\n", gfxstate.crtc->mode.hdisplay);
	printf("hsync_start: %u hsync_end: %u\n",
	    gfxstate.crtc->mode.hsync_start, gfxstate.crtc->mode.hsync_end);
	printf("htotal: %u\n", gfxstate.crtc->mode.htotal);
	printf("hskew: %u\n", gfxstate.crtc->mode.hskew);
	printf("vdisplay: %u\n", gfxstate.crtc->mode.vdisplay);
	printf("vsync_start: %u vsync_end: %u\n",
	    gfxstate.crtc->mode.vsync_start, gfxstate.crtc->mode.vsync_end);
	printf("vtotal: %u\n", gfxstate.crtc->mode.vtotal);
	printf("vscan: %u\n", gfxstate.crtc->mode.vscan);
	printf("flags: %u\n", gfxstate.crtc->mode.flags);
	printf("type: %u\n", gfxstate.crtc->mode.type);
#endif

	kms_create(dst->fd, &dst->kms);

	return 0;
}

static void
drm_backend_finish(struct drm_state *dst)
{
	kms_destroy(&dst->kms);
	drmModeFreeConnector(dst->conn);
	drmModeFreeCrtc(dst->crtc);
	drmClose(dst->fd);
}
static void
drm_backend_allocfb(struct drm_state *dst, struct drm_framebuffer *fb)
{
	fb->width = dst->crtc->mode.hdisplay;
	fb->height = dst->crtc->mode.vdisplay;

	unsigned bo_attribs[] = {
		KMS_WIDTH,	fb->width,
		KMS_HEIGHT,	fb->height,
		KMS_BO_TYPE,	KMS_BO_TYPE_SCANOUT_X8R8G8B8,
		KMS_TERMINATE_PROP_LIST
	};
	kms_bo_create(dst->kms, bo_attribs, &fb->bo);
	kms_bo_get_prop(fb->bo, KMS_HANDLE, &fb->handles[0]);
	kms_bo_get_prop(fb->bo, KMS_PITCH, &fb->pitches[0]);
#if 0
	printf("fb->pitches[0] = %u\n", fb->pitches[0]);
	printf("fb->handles[0] = %u\n", fb->handles[0]);
#endif
	fb->offsets[0] = 0;
	kms_bo_map(fb->bo, &fb->plane);
	drmModeAddFB2(dst->fd, fb->width, fb->height, DRM_FORMAT_XRGB8888,
	    fb->handles, fb->pitches, fb->offsets,
	    &fb->fbid, 0);
}

static void
drm_backend_destroyfb(struct drm_state *dst, struct drm_framebuffer *fb)
{
	drmModeRmFB(dst->fd, fb->fbid);
	kms_bo_unmap(fb->bo);
	kms_bo_destroy(&fb->bo);
}

static int
drm_backend_show(struct drm_state *dst, struct drm_framebuffer *fb)
{
	int ret = 0;

	if (drmSetMaster(gfxstate.fd) != 0) {
		perror("drmSetMaster");
		ret = 1;
	}
	if (drmModeSetCrtc(dst->fd, dst->crtc->crtc_id, fb->fbid, 0, 0,
	    &dst->conn->connector_id, 1, &dst->crtc->mode) != 0) {
		perror("drmModeSetCrtc");
		ret = 1;
	}
	return ret;
}

static int
drm_backend_hide(struct drm_state *dst)
{
	int ret = 0;

	if (drmModeSetCrtc(dst->fd, dst->crtc->crtc_id, dst->oldbuffer_id,
	    0, 0, &dst->conn->connector_id, 1, &dst->crtc->mode) != 0) {
		perror("drmModeSetCrtc");
		ret = 1;
	}
	if (drmDropMaster(dst->fd) != 0) {
		perror("drmDropMaster");
		ret = 1;
	}
	return ret;
}

int
main(int argc, char *argv[])
{
	char *shell;
	teken_pos_t winsize;
	struct terminal term;
	char *normalfont = NULL, *boldfont = NULL;
	int i, ch;
	bool whitebg = false;

	unsigned int fontheight = 16;
	bool alpha = true;
	int pivot = 0;
	char *kbd_layout = NULL, *kbd_options = NULL, *kbd_variant = NULL;

	const char *errstr;
	struct stat fontstat;

	unsigned int repeat_delay = 200;
	unsigned int repeat_rate = 30;

	/* XXX handle bitmap fonts better */
	while ((ch = getopt(argc, argv, "aAhwd:r:f:F:i:k:o:p:v:s:")) != -1) {
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
		case 'p':
			pivot = strtonum(optarg, 0, 3, &errstr);
			if (errstr) {
				errx(1, "pivot should be 0-3 (degrees / 90), but is %s: %s", errstr, optarg);
			}
			break;
		case 'r':
			repeat_rate = strtonum(optarg, 1, 50, &errstr);
			if (errstr) {
				errx(1, "key repeat rate is %s: %s", errstr,
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
		case 'v':
			kbd_variant = optarg;
			break;
		case 'w':
			whitebg = true;
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

	if (normalfont != NULL && stat(normalfont, &fontstat) != 0) {
		warn("%s", normalfont);
		normalfont = NULL;
	}

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
		24, 80, 8 * 24, 16 * 80
	};
	/*
	 * XXX This should rather come at the end of initialization because
	 *     it usually will never fail.
	 */
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
	    &fnwidth, &fnheight, alpha, pivot);
	if (rop == NULL)
		errx(1, "rop32_init failed, aborting");

	if (drm_backend_init(&gfxstate) != 0) {
		errx(1, "Failed to initialize drm backend");
	}
	fnpivot = pivot;
	drm_backend_allocfb(&gfxstate, &framebuffer);
	rop32_setclip(rop, (point){0,0},
	    (point){framebuffer.width, framebuffer.height});
	rop32_setcontext(rop, framebuffer.plane, framebuffer.width);

	vtconfigure();
	drm_backend_show(&gfxstate, &framebuffer);

	switch (pivot) {
	case 0:
	case 2:
	default:
		winsize.tp_col = framebuffer.width / fnwidth;
		winsize.tp_row = framebuffer.height / fnheight;
		break;
	case 1:
	case 3:
		winsize.tp_col = framebuffer.height / fnwidth;
		winsize.tp_row = framebuffer.width / fnheight;
		break;
	}
//	winsize.tp_col = 80;
//	winsize.tp_row = 25;
	teken_set_winsize(&term.tek, &winsize);
        term.winsz.ws_col = winsize.tp_col;
        term.winsz.ws_row = winsize.tp_row;
	/* XXX: Should ws_xpixel and ws_ypixel report the physical dimensions or the logical dimensions after pivot? */
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
	uint32_t k;
	for (k = 0; k < framebuffer.width * framebuffer.height; k++) {
		((uint32_t *)framebuffer.plane)[k] =
		    colormap[teken_get_defattr(&term.tek)->ta_bgcolor];
	}
	for (i = 0; i < term.winsz.ws_col * term.winsz.ws_row; i++) {
		term.buf[i].attr = *teken_get_defattr(&term.tek);
		term.buf[i].ch = ' ';
		term.buf[i].cursor = 0;
		term.buf[i].dirty = 0;
	}
	memcpy(oldbuf, term.buf,
	    term.winsz.ws_col * term.winsz.ws_row * sizeof(*term.buf));

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

	masterev = event_new(evbase, term.amaster,
	    EV_READ | EV_PERSIST, rdmaster, &term);
	event_priority_set(masterev, 4);

	repeatev = evtimer_new(evbase, keyrepeat, NULL);
	event_priority_set(repeatev, 3);

	ttyev = event_new(evbase, ttyfd,
	    EV_READ | EV_PERSIST, ttyread, NULL);
	event_priority_set(ttyev, 2);

	/* XXX event handler for writes to the master fd of the pty device */

	drmev = event_new(evbase, gfxstate.fd,
	    EV_READ | EV_PERSIST, drmread, NULL);
	event_priority_set(drmev, 1);

	vtrelev = evsignal_new(evbase, SIGUSR1, vtrelease, NULL);
	event_priority_set(vtrelev, 0);

	vtacqev = evsignal_new(evbase, SIGUSR2, vtacquire, NULL);
	event_priority_set(vtacqev, 0);

	signal(SIGINT, SIG_IGN);
	sigintev = evsignal_new(evbase, SIGINT, handleterm, NULL);
	event_priority_set(sigintev, 0);

	if (idleev != NULL && active)
		event_add(idleev, &idletv);
	event_add(masterev, NULL);
	event_add(ttyev, NULL);
	event_add(drmev, NULL);
	event_add(vtrelev, NULL);
	event_add(vtacqev, NULL);
	event_add(sigintev, NULL);

	event_base_loop(evbase, 0);
	signal(SIGINT, SIG_DFL);
	drm_set_dpms(&gfxstate, DRM_MODE_DPMS_ON);

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

	free(term.buf);
	free(oldbuf);

	drm_backend_hide(&gfxstate);
	vtdeconf();
	drm_backend_destroyfb(&gfxstate, &framebuffer);
	drm_backend_finish(&gfxstate);

	xkb_finish();
}

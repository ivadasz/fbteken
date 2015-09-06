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
#include <sys/ioctl.h>
#include <sys/kbio.h>

#include <stdlib.h>
#include <string.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>

#include "kbd.h"

struct kbd_state {
	int kbdfd;

	uint8_t lastread_code;

	/* Just track all keys for now, to avoid stuck modifiers */
	uint8_t pressed[256];
	int npressed;
};

static struct kbd_event atcode_to_event(uint8_t atcode);
static int ispressed(struct kbd_state *state, uint8_t code);
static void press(struct kbd_state *state, uint8_t code);
static void release(struct kbd_state *state, uint8_t code);

static struct kbd_event
atcode_to_event(uint8_t atcode)
{
	struct kbd_event ev;
	int code;

	if ((atcode & 0x7f) <= 0x58) {
		code = (atcode & 0x7f);
	} else {
		/*
		 * Translate keycodes as in sys/dev/misc/kbd/atkbd.c
		 * from DragonFly to evdev key values.
		 */
		int code_to_key[128] = {
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
		code = code_to_key[atcode & 0x7f];
	}
	/* Fixes e.g. the '<' key which lies between 0x50 and 0x58 */
	if (code == 0)
		code = (atcode & 0x7f);

	ev.keycode = code;
	ev.pressed = (atcode & 0x80) ? 0 : 1;

	return ev;
}

static int
ispressed(struct kbd_state *state, uint8_t code)
{
	int i;

	for (i = 0; i < state->npressed; i++)
		if (state->pressed[i] == code)
			return 1;

	return 0;
}

static void
press(struct kbd_state *state, uint8_t code)
{
	if (!ispressed(state, code)) {
		state->pressed[state->npressed] = code;
		state->npressed++;
	}
}

static void
release(struct kbd_state *state, uint8_t code)
{
	int i;

	for (i = 0; i < state->npressed; i++) {
		if (state->pressed[i] == code) {
			state->npressed--;
			memcpy(&state->pressed[i], &state->pressed[i+1],
			    state->npressed-i);
		}
	}
}

int
kbd_set_leds(struct kbd_state *state, int mask)
{
	return ioctl(state->kbdfd, KDSETLED, mask);
}

int
kbd_get_leds(struct kbd_state *state, int *mask)
{
	return ioctl(state->kbdfd, KDGETLED, &mask);
}

struct kbd_state *
kbd_new_state(int fd)
{
	struct kbd_state *state;

	state = calloc(1, sizeof(*state));
	if (state == NULL)
		return NULL;

	state->kbdfd = fd;
	if (ioctl(state->kbdfd, KDSKBMODE, K_CODE) != 0) {
		warn("KDSKBMODE");
		free(state);
		return NULL;
	}

	return state;
}

void
kbd_destroy_state(struct kbd_state *state)
{
	if (ioctl(state->kbdfd, KDSKBMODE, K_XLATE) != 0) {
		warn("KDSKBMODE");
	}
	free(state);
}

void
kbd_reset_state(struct kbd_state *state)
{
	state->lastread_code = 0;
	state->npressed = 0;
}

/* Returns -1 on error, 0 on end-of-file, number-of-events read otherwise */
int
kbd_read_events(struct kbd_state *state, struct kbd_event *out, int count)
{
	uint8_t buf[128];
	struct kbd_event ev;
	int n = 0, val, i;

	while (n < count) {
		val = read(state->kbdfd, buf,
		    MIN((unsigned)count - n, sizeof(buf)));
		if (val == 0) {
			return n;
		} else if (val < 0) {
			if (errno == EAGAIN)
				return n;
			else
				return val;
		}

		for (i = 0; i < val; i++) {
			if (state->lastread_code == buf[i] &&
			    !(buf[i] & 0x80)) {
				continue;
			}

			ev = atcode_to_event(buf[i]);
			if (ev.keycode == 0)
				continue;

			state->lastread_code = buf[i];

			/*
			 * XXX Debug this issue
			 *     (might be Latitude E5450 specific)
			 */
			if (ev.pressed && ispressed(state, buf[i] & 0x7f))
				warnx("atcode 0x%02x keycode 0x%02x was "
				    "already pressed", buf[i], ev.keycode);

			if (ev.pressed)
				press(state, buf[i] & 0x7f);
			else
				release(state, buf[i] & 0x7f);

			out[n] = ev;
			n++;
		}
	}

	return n;
}

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

/*
 * Terminal buffer operations.
 */

#include <sys/param.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>

struct bufent {
	teken_char_t ch;
	teken_attr_t attr;
	int16_t cursor;
	int16_t dirty;
};

struct buffer {
	struct bufent **buf;
	struct bufent *mem;
	uint32_t *dirtybuf;
	unsigned int dirtycount;
	int dirtyflag;
	int width, height;
};

#define BUFFER_CELL_XY(b,x,y) (&(b)->buf[(y)][(x)])
#define BUFFER_CELL_I(b,i) BUFFER_CELL_XY(b, (i)%(b)->width, (i)/(b)->width)

static struct buffer *bufent_buffer_create(int width, int height);
static void bufent_buffer_destroy(struct buffer *b);
static void bufent_buffer_set(struct buffer *b, int x, int y, teken_char_t ch,
			      const teken_attr_t *attr);
static void bufent_buffer_fill(struct buffer *b, const teken_rect_t *rect,
			       teken_char_t ch, const teken_attr_t *attr);
static void bufent_buffer_copy(struct buffer *b, const teken_rect_t *rect,
			       const teken_pos_t *pos);
static void bufent_buffer_save(struct buffer *b, struct bufent *buf);
static void bufent_buffer_dirtycell_xy(struct buffer *b, int x, int y);

static inline struct buffer *
bufent_buffer_create(int width, int height)
{
	struct buffer *b;
	int i;

	b = calloc(1, sizeof(*b));
	if (b == NULL) {
		warn("calloc");
		return NULL;
	}

	b->width = width;
	b->height = height;
	b->dirtycount = 0;
	b->dirtyflag = 1;
	b->buf = calloc(height, sizeof(*b->buf));
	if (b->buf == NULL) {
		warn("calloc");
		goto fail1;
	}
	b->mem = calloc(width * height, sizeof(*b->mem));
	if (b->mem == NULL) {
		warn("calloc");
		goto fail2;
	}

	b->dirtybuf = calloc(width * height, sizeof(*b->dirtybuf));
	if (b->dirtybuf == NULL) {
		warn("calloc");
		goto fail3;
	}

	for (i = 0; i < height; i++)
		b->buf[i] = &b->mem[i*width];

	return b;

fail3:
	free(b->mem);
fail2:
	free(b->buf);
fail1:
	free(b);
	return NULL;
}

static inline void
bufent_buffer_destroy(struct buffer *b)
{
	if (b != NULL) {
		free(b->mem);
		free(b->buf);
		free(b->dirtybuf);
	}
	free(b);
}

static inline void
bufent_buffer_set(struct buffer *b, int x, int y, teken_char_t ch,
    const teken_attr_t *attr)
{
	struct bufent *a = &b->buf[y][x];

	a->ch = ch;
	a->attr = *attr;
	if (!(b->dirtyflag || a->dirty)) {
		a->dirty = 1;
		b->dirtybuf[b->dirtycount] = y * b->width + x;
		b->dirtycount++;
	}
}

static inline void
bufent_buffer_fill(struct buffer *b, const teken_rect_t *rect,
    teken_char_t ch, const teken_attr_t *attr)
{
	teken_unit_t i, j, rbegin, rend, cbegin, cend;

	rbegin = rect->tr_begin.tp_row;
	rend = rect->tr_end.tp_row;
	cbegin = rect->tr_begin.tp_col;
	cend = rect->tr_end.tp_col;

	if (rend - rbegin > 1)
		b->dirtyflag = 1;

	if (b->dirtyflag) {
		for (i = rbegin; i < rend; i++) {
			for (j = cbegin; j < cend; j++) {
				b->buf[i][j].ch = ch;
				b->buf[i][j].attr = *attr;
			}
		}
	} else {
		for (i = rbegin; i < rend; i++) {
			for (j = cbegin; j < cend; j++) {
				b->buf[i][j].ch = ch;
				b->buf[i][j].attr = *attr;
				if (!b->buf[i][j].dirty) {
					b->buf[i][j].dirty = 1;
					b->dirtybuf[b->dirtycount] =
					    i * b->width + j;
					b->dirtycount++;
				}
			}
		}
	}
}

static inline void
bufent_buffer_copy(struct buffer *b, const teken_rect_t *rect,
    const teken_pos_t *pos)
{
	teken_unit_t w, h;
	teken_unit_t scol, srow, tcol, trow;
	int a;

	scol = rect->tr_begin.tp_col;
	srow = rect->tr_begin.tp_row;
	tcol = pos->tp_col;
	trow = pos->tp_row;
	w = rect->tr_end.tp_col - rect->tr_begin.tp_col;
	h = rect->tr_end.tp_row - rect->tr_begin.tp_row;

	b->dirtyflag = 1;

	/* Special optimized code path */
	if (scol == 0 && tcol == 0 && w == b->width &&
	    (srow + 1 == trow || srow == trow + 1)) {
		struct bufent *tmp;
		if (srow < trow) {
			assert(trow == srow + 1);
			/* moving one line up */
			memcpy(b->buf[trow + h - 1],
			    b->buf[srow], w * sizeof(**b->buf));
			tmp = b->buf[trow + h - 1];
			for (a = h - 1; a >= 0; a--) {
				b->buf[trow + a] = b->buf[srow + a];
			}
			b->buf[srow] = tmp;
		} else {
			assert(trow + 1 == srow);
			/* moving one line down */
			memcpy(b->buf[trow],
			    b->buf[srow + h - 1], w * sizeof(**b->buf));
			tmp = b->buf[trow];
			for (a = 0; a < h; a++) {
				b->buf[trow + a] = b->buf[srow + a];
			}
			b->buf[srow + h - 1] = tmp;
		}
		return;
	}

	if (srow < trow) {
		for (a = h - 1; a >= 0; a--) {
			memcpy(&b->buf[trow + a][tcol],
			    &b->buf[srow + a][scol], w * sizeof(**b->buf));
		}
	} else if (srow > trow) {
		for (a = 0; a < h; a++) {
			memcpy(&b->buf[trow + a][tcol],
			    &b->buf[srow + a][scol], w * sizeof(**b->buf));
		}
	} else {
		for (a = 0; a < h; a++) {
			memmove(&b->buf[trow + a][tcol],
			    &b->buf[srow + a][scol], w * sizeof(**b->buf));
		}
	}
}

static inline void
bufent_buffer_save(struct buffer *b, struct bufent *buf)
{
	int i, j, idx = 0;

	for (i = 0; i < b->height; i++) {
		for (j = 0; j < b->width; j++) {
			buf[idx] = b->buf[i][j];
			idx++;
		}
	}
}

static inline void
bufent_buffer_dirtycell_xy(struct buffer *b, int x, int y)
{
	struct bufent *cell;

	if (b->dirtyflag)
		return;

	cell = BUFFER_CELL_XY(b, x, y);
	if (!cell->dirty) {
		cell->dirty = 1;
		b->dirtybuf[b->dirtycount] = y * b->width + x;
		b->dirtycount++;
	}
}

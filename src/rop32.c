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

#include <err.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_CACHE_H

#include "fbdraw.h"

typedef struct MyFaceRec_ {
	const char	*file_path;
	int		 face_index;
} MyFaceRec, *MyFace;

struct rop_obj {
	uint32_t *fb;

	struct pointrectangle clip;
	uint16_t width;

	FTC_Manager manager;
	FTC_ScalerRec scaler, boldscaler;
	FTC_CMapCache cmc;
	FTC_SBitCache sbit;
	MyFaceRec fid, boldfid;

	FT_Library library;
	FT_Face face, boldface;
	FT_Size sz;

	uint16_t fontwidth, fontheight;

	uint32_t cmap_idx;

	bool doalpha;
};

static void rop32_drawhoriz(struct rop_obj *, point, point, color);
static void rop32_drawvert(struct rop_obj *, point, point, color);
static FT_Error my_face_requester(FTC_FaceID, FT_Library, FT_Pointer,
    FT_Face *);
static void rop32_alphaexpand(void *, int, uint8_t *, int, int, int, int, int,
    color, color);
static void rop32_blit8_aa(struct rop_obj *, point, uint8_t *, int, int,
    int, color, color);
static void rop32_monoexpand(void *, int, uint8_t *, int, int, int, int, int,
    color);
static void rop32_blit1(struct rop_obj *, point, uint8_t *, int, int,
    int, color, color);

static FT_Error
my_face_requester(FTC_FaceID face_id, FT_Library library,
    FT_Pointer request_data __unused, FT_Face *aface)
{
	MyFace face = (MyFace)face_id;
	return FT_New_Face(library, face->file_path, face->face_index, aface);
}

static int
openfont(FTC_Manager manager, MyFaceRec *fid, FT_Face *face)
{
	int error;

	error = FTC_Manager_LookupFace(manager, fid, face);
	if (error)
		return error;

#if 0
	printf("There are %d faces embedded into the font \"%s\"\n",
	    face->num_faces, fid->file_path);
	printf("There are %d fixed sizes embedded into the font \"%s\"\n",
	    face->num_fixed_sizes, fid->file_path);
	printf("Fixed sizes:");
	for (i = 0; i < face->num_fixed_sizes; i++)
		printf(" %d", face->available_sizes[i].height);
	printf("\n");
#endif

	return 0;
}

/* ARGSUSED */
struct rop_obj *
rop32_init(char *fp, char *boldfp, int h, int *fn_width, int *fn_height,
    bool alpha)
{
	struct rop_obj *self;
	char default_fp[] = "/usr/local/share/fonts/dejavu/DejaVuSansMono.ttf";
	char default_boldfp[] =
	    "/usr/local/share/fonts/dejavu/DejaVuSansMono-Bold.ttf";

	self = calloc(1, sizeof(struct rop_obj));
	if (self == NULL) {
		warn("calloc");
		return NULL;
	}

	int error;

	self->doalpha = alpha;

	if (fp == NULL && boldfp == NULL)
		boldfp = default_boldfp;
	if (fp == NULL)
		fp = default_fp;

	self->fid.file_path = fp;
	self->fid.face_index = 0;
	self->boldfid.file_path = boldfp;
	self->boldfid.face_index = 0;
	error = FT_Init_FreeType(&self->library);
	if (error) {
		printf("Failed to initialize freetype\n");
		return NULL;
	}

	/* initialize cache manager */
	error = FTC_Manager_New(self->library, 0, 0, 1 << 20,
	    &my_face_requester, NULL, &self->manager);
	if (error) {
		printf("Failed to initialize manager\n");
		return NULL;
	}

	error = FTC_CMapCache_New(self->manager, &self->cmc);
	if (error) {
		printf("Failed to initialize cmapcache\n");
		return NULL;
	}

	error = FTC_SBitCache_New(self->manager, &self->sbit);
	if (error) {
		printf("Failed to initialize sbitcache\n");
		return NULL;
	}

	error = openfont(self->manager, &self->fid, &self->face);
	if (error == FT_Err_Unknown_File_Format) {
		printf("Font format of \"%s\" is unsupported\n", fp);
		return NULL;
	} else if (error) {
		printf("Failed to initialize font \"%s\"\n", fp);
		return NULL;
	}

	if (self->boldfid.file_path == NULL)
		goto skipbold;
	error = openfont(self->manager, &self->boldfid, &self->boldface);
	if (error == FT_Err_Unknown_File_Format) {
		printf("Font format of \"%s\" is unsupported\n", boldfp);
		return NULL;
	} else if (error) {
		printf("Failed to initialize font \"%s\"\n", boldfp);
		return NULL;
	}
skipbold:

#if 0
	/*
	 * XXX This logic would need to synchronize settings with the bold
	 *     font as well.
	 */
	printf("num of charmaps: %d\n", self->face->num_charmaps);
	for (i = 0; i < self->face->num_charmaps; i++) {
		printf("id: %d\n", self->face->charmaps[i]->encoding_id);
		if (self->face->charmaps[i]->encoding_id ==
		    FT_ENCODING_UNICODE)
			self->cmap_idx = i;
	}
	if (i == self->face->num_charmaps)
#endif
		self->cmap_idx = -1;
#if 0
	printf("cmap_idx: %d\n", self->cmap_idx);
#endif

	self->scaler.face_id = &self->fid;
	self->scaler.pixel = 1;
	self->scaler.height = h;
	self->scaler.width = 0;
	error = FTC_Manager_LookupSize(self->manager, &self->scaler,
	    &self->sz);
	if (error) {
		printf("Failed to set pixel size %d for font \"%s\"\n", h, fp);
		return NULL;
	}
	self->fontwidth = (self->sz->metrics.max_advance >> 6);
	self->fontheight = (self->sz->metrics.height >> 6);
#if 0
	printf("width: %d height: %d\n", self->fontwidth, self->fontheight);
#endif

	if (fn_width != NULL)
		*fn_width = self->fontwidth;
	if (fn_height != NULL)
		*fn_height = self->fontheight;

	if (self->boldfid.file_path == NULL)
		goto skipboldscaler;
	self->boldscaler.face_id = &self->boldfid;
	self->boldscaler.pixel = 1;
	self->boldscaler.height = h;
	self->boldscaler.width = 0;
	error = FTC_Manager_LookupSize(self->manager, &self->boldscaler,
	    &self->sz);
	if (error) {
		printf("Failed to set pixel size %d for font \"%s\"\n", h, boldfp);
		return NULL;
	}
skipboldscaler:

	return self;
}

/*
 * Set clip rectangle with left-upper corner and right-bottom corner.
 */
void
rop32_setclip(struct rop_obj *self, point leftupper, point rightbottom)
{
	self->clip.a = leftupper;
	self->clip.b = rightbottom;
}

void
rop32_setcontext(struct rop_obj *self, void *mem, uint16_t w)
{
	self->fb = mem;
	self->width = w;
}

static inline void
rop32_alphaexpand(void *target, int towidth, uint8_t *src, int x, int y,
    int w, int h, int srcpitch, color fg, color bg)
{
	uint32_t *p = (uint32_t *)target;
	uint8_t *mysrc = &src[y * srcpitch + x];
	int i, j;
	uint8_t a;
	uint8_t g, ag;

	g = (fg & 0x0000ff00) >> 8;
	ag = (bg & 0x0000ff00) >> 8;

	uint32_t myrb, myarb;
	myrb = fg & 0x00ff00ff;
	myarb = bg & 0x00ff00ff;

/* XXX Use sse instructions instead */
#define AASCALE(c,d,f) (((((uint16_t)(c)) * (f)) + (((uint16_t)(d)) * (255 - (f)))) >> 8)
#define AASCALE2(c,d,f) ((((c) * (f) + (d) * (255 - (f))) >> 8) & 0x00ff00ff)
	for (i = 0; i < h; i++) {
		uint8_t *isrc = &mysrc[i * srcpitch];
		uint32_t *ip = &p[i * towidth];
		for (j = 0; j < w; j++) {
			a = isrc[j];
			if (a > 0) {
				ip[j] =
				    AASCALE2(myrb,myarb,a) |
				    (AASCALE(g,ag,a) << 8);
			}
		}
	}
#undef AASCALE
#undef AASCALE2
}

static inline void
rop32_blit8_aa(struct rop_obj *self, point pos, uint8_t *src, int w, int h,
    int pitch, color fg, color bg)
{
	uint32_t *p = &((uint32_t *)self->fb)[pos.y * self->width + pos.x];
	int a, b, c, d;

	a = MAX(0, self->clip.a.x - pos.x);
	b = MAX(0, self->clip.a.y - pos.y);
	c = MIN(w, self->clip.b.x - pos.x);
	d = MIN(h, self->clip.b.y - pos.y);

	rop32_alphaexpand(&p[b * self->width + a], self->width, src, a, b,
	    c - a, d - b, pitch, fg, bg);
}

/*
 * XXX It is often more efficient to write all pixels (because of
 *     write-combining memory type)
 */
static inline void
rop32_monoexpand(void *target, int towidth, uint8_t *src, int x, int y,
    int w, int h, int srcpitch, color col)
{
	uint32_t *p = (uint32_t *)target;
	int i, j;

	for (i = y; i < h + y; i++) {
		uint8_t *isrc = &src[i * srcpitch];
		uint32_t *ip = &p[(i - y) * towidth];
		for (j = x; j < w + x; j++)
			if (isrc[j >> 3] & (0x80 >> (j & 0x7)))
				ip[j - x] = col;
	}
}

static void
rop32_blit1(struct rop_obj *self, point pos, uint8_t *src, int w, int h,
    int pitch, color col, color bg __unused)
{
	uint32_t *p = &((uint32_t *)self->fb)[pos.y * self->width + pos.x];
	int a, b, c, d;

	a = MAX(0, self->clip.a.x - pos.x);
	b = MAX(0, self->clip.a.y - pos.y);
	c = MIN(w, self->clip.b.x - pos.x);
	d = MIN(h, self->clip.b.y - pos.y);

	rop32_monoexpand(&p[b * self->width + a], self->width, src, a, b,
	    c - a, d - b, pitch, col);
}

static void
rop32_drawhoriz(struct rop_obj *self, point start, point end, color col)
{
	uint32_t *p;
	int16_t a, b;
	uint16_t i;

	if (start.y < self->clip.a.y || start.y >= self->clip.b.y)
		return;

	p = &((uint32_t *)self->fb)[start.y * self->width];

	a = MAX(self->clip.a.x, start.x);
	b = MIN(self->clip.b.x - 1, end.x);

	for (i = a; i <= b; i++)
		p[i] = col;
}

static void
rop32_drawvert(struct rop_obj *self, point start, point end, color col)
{
	uint32_t *p;
	int16_t a, b;
	uint16_t i;

	if (start.x < self->clip.a.x || start.x >= self->clip.b.x)
		return;

	p = &((uint32_t *)self->fb)[start.x];

	a = MAX(self->clip.a.y, start.y);
	b = MIN(self->clip.b.y, end.y);

	for (i = a; i <= b; i++)
		p[i * self->width] = col;
}

/*
 * Draw a line using the bresenham algorithm. Revert to horizontal or
 * vertical line drawing functions when possible.
 */
void
rop32_line(struct rop_obj *self, point start, point end, color col)
{
	int x, y, dx, dy, sx, sy;
	int er, er2;
	uint32_t *p = (uint32_t *)self->fb;

	if (start.x == end.x) {
		rop32_drawhoriz(self, start, end, col);
		return;
	} else if (start.y == end.y) {
		rop32_drawvert(self, start, end, col);
		return;
	}

	sx = start.x < end.x ? 1 : -1;
	sy = start.y < end.y ? 1 : -1;

	if (sx > 0)
		dx = end.x - start.x;
	else
		dx = start.x - end.x;

	if (sy > 0)
		dy = end.y - start.y;
	else
		dy = start.y - end.y;

	er = dx - dy;

	x = start.x;
	y = start.y;

#define SETPIX(v,w) if ((v) >= self->clip.a.x && (v) < self->clip.b.x && \
			(w) >= self->clip.a.y && (w) < self->clip.b.y) { \
			    p[(w) * self->width + v] = col;		\
		    }

	SETPIX(x,y);
	while (x != end.x || y != end.y) {
		er2 = 2 * er;
		if (er2 > -dy) {
			er = er - dy;
			x += sx;
		}
		if (x == end.x && y == end.y) {
			SETPIX(x,y);
			break;
		}
		if (er2 < dx) {
			er = er + dx;
			y += sy;
		}
		SETPIX(x,y);
	}
#undef SETPIX
}

/*
 * Fill a rectangle, given the position, size and color.
 */
void
rop32_rect(struct rop_obj *self, point pos, dimension dim, color col)
{
	int i, j;
	uint32_t *p = (uint32_t *)self->fb;
	int16_t a, b, c, d;

	a = MAX(pos.x, self->clip.a.x);
	b = MIN(pos.x + dim.x, self->clip.b.x);
	c = MAX(pos.y, self->clip.a.y);
	d = MIN(pos.y + dim.y, self->clip.b.y);

	for (i = c; i < d; i++)
		for (j = a; j < b; j++)
			p[i * self->width + j] = col;
}

/*
 * Blit a rectangle from source to target, given the size of the area.
 */
void
rop32_move(struct rop_obj *self, point source, point target, dimension dim)
{
	int i;
	uint32_t *p = (uint32_t *)self->fb, *sp, *tp;
	int16_t a, b, c, d;

	a = MAX(source.x, self->clip.a.x);
	dim.x = dim.x - (a - source.x);
	dim.x = MIN(dim.x, self->clip.b.x - a);
	if (dim.x <= 0)
		return;
	target.x += a - source.x;
	source.x = a;

	b = MAX(target.x, self->clip.a.x);
	dim.x = dim.x - (b - target.x);
	dim.x = MIN(dim.x, self->clip.b.x - b);
	if (dim.x <= 0)
		return;
	source.x += b - target.x;
	target.x = b;

	c = MAX(source.y, self->clip.a.y);
	dim.y = dim.y - (c - source.y);
	dim.y = MIN(dim.y, self->clip.b.y - c);
	if (dim.y <= 0)
		return;
	target.y += c - source.y;
	source.y = c;

	d = MAX(target.y, self->clip.a.y);
	dim.y = dim.y - (d - target.y);
	dim.y = MIN(dim.y, self->clip.b.y - d);
	if (dim.y <= 0)
		return;
	source.y += d - target.y;
	target.y = d;

	sp = &p[source.y * self->width + source.x];
	tp = &p[target.y * self->width + target.x];

	printf("actual source of blit: x: %d y: %d\n", source.x, source.y);
	printf("actual target of blit: x: %d y: %d\n", target.x, target.y);
	printf("actual size of blit: x: %d y: %d\n", dim.x, dim.y);
	if (source.y >= target.y) {
		for (i = 0; i < dim.y; i++)
			memmove(&tp[i * self->width], &sp[i * self->width],
			    dim.x * 4);
	} else {
		for (i = dim.y - 1; i >= 0; i--)
			memmove(&tp[i * self->width], &sp[i * self->width],
			    dim.x * 4);
	}
}

/*
 * Draw a character, given the left upper corner to start drawing.
 */
point
rop32_char(struct rop_obj *self, point pos, color fg, color bg, uint32_t c,
    int flags)
{
	FT_Int idx;
	FTC_SBit sbit;
	int error;
	int16_t bty;

	idx = FTC_CMapCache_Lookup(self->cmc,
	    ((flags & 2) && self->boldfid.file_path != NULL) ?
	    &self->boldfid : &self->fid,
	    self->cmap_idx, c);
//	if (idx == 0)
//		printf("cmapcache_lookup for 0x%08x failed\n", c);

	if (self->doalpha)
		error = FTC_SBitCache_LookupScaler(self->sbit,
		    ((flags & 2) && self->boldfid.file_path != NULL) ?
		    &self->boldscaler : &self->scaler,
		    FT_LOAD_RENDER, idx, &sbit, NULL);
	else
		error = FTC_SBitCache_LookupScaler(self->sbit,
		    ((flags & 2) && self->boldfid.file_path != NULL) ?
		    &self->boldscaler : &self->scaler,
		    FT_LOAD_RENDER | FT_LOAD_MONOCHROME, idx, &sbit, NULL);
	if (error) {
		printf("Failed to lookup in sbitcache\n");
		return pos;
	}

	if (sbit->buffer == 0) {
//		if (c != ' ' && c != '\0')
//			printf("Missing glyph bitmap\n");
		goto justadvance;
	}

	if (self->doalpha)
		rop32_blit8_aa(self,
		    (point){pos.x + sbit->left,
		    pos.y + (self->sz->metrics.ascender >> 6) - sbit->top},
		    sbit->buffer, sbit->width, sbit->height,
		    sbit->pitch, fg, bg);
	else
		rop32_blit1(self,
		    (point){pos.x + sbit->left,
		    pos.y + (self->sz->metrics.ascender >> 6) - sbit->top},
		    sbit->buffer, sbit->width, sbit->height,
		    sbit->pitch, fg, bg);

justadvance:
	/* Underlining currently only works nicely for monospaced fonts */
	if (flags & 1) {
		bty = pos.y + (self->sz->metrics.ascender >> 6) + 2;
		rop32_drawhoriz(self, (point){pos.x, bty},
//		    (point){pos.x + sbit->xadvance - 1, bty - 1}, fg);
		    (point){pos.x + self->fontwidth - 1, bty}, fg);
	}

	return (point){pos.x + sbit->xadvance, pos.y};
}

/*
 * Draw a string, given the left upper corner to start drawing.
 */
point
rop32_text(struct rop_obj *self, point pos, color fg, color bg, char *str,
    int flags)
{
	int i, m;

	m = strlen(str);
	for (i = 0; i < m; i++)
		pos = rop32_char(self, pos, fg, bg, str[i], flags);

	return pos;
}

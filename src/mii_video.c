/*
 * mii_video.c
 *
 * Copyright (C) 2023 Michel Pollet <buserror@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "mii.h"
#include "mii_bank.h"
#include "mii_sw.h"
#include "minipt.h"
#include "debug_log.h"

#if defined(__AVX2__)
#include <immintrin.h>
typedef uint32_t u32_v __attribute__((vector_size(32)));
#define VEC_ALIGN 31
#define VEC_ECOUNT 8
#else
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
typedef uint32_t u32_v __attribute__((vector_size(16)));
#define VEC_ALIGN 15
#define VEC_ECOUNT 4
#endif

enum {
	// https://rich12345.tripod.com/aiivideo/vbl.html
	MII_VBL_DOWN_CYCLES 	= 12480,
	MII_VBL_UP_CYCLES 		= 4550,
	MII_VIDEO_H_CYCLES 		= 40,
	MII_VIDEO_HB_CYCLES 	= 25,
};

#define MII_VIDEO_MIXED_LINE 		(192 - (4 * 8))
// frequency of the blinking text, in frames. When that bit changes, we flash
#define MII_VIDEO_FLASH_FRAME_MASK 	0x10

// this is the bank video memory is read from. This differs from the AUX
// bank as it doesn't change when the Ramworks card bank is changed
#define MII_VIDEO_BANK MII_BANK_AUX_BASE

/*
 * Colors were lifted from
 * https://comp.sys.apple2.narkive.com/lTSrj2ZI/apple-ii-colour-rgb
 * and
 * https://www.mrob.com/pub/xapple2/colors.html
 */
#define HI_LUMA(r,g,b) \
		((uint8_t)(0.2126 * (r) + 0.7152 * (g) + 0.0722 * (b)))

/*
 * You migth have to tweak this for performance reasons. At least on nVidia
 * cards, GL_BGRA is faster than GL_RGBA.
 */
#define HI_RGB(_r,_g,_b) (0xff000000 | ((_b) << 16) | ((_g) << 8) | (_r))
#define HI_C_RGB(_r,_g,_b)	(mii_color_t) \
		(0xff000000 | ((_b) << 16) | ((_g) << 8) | (_r))

#define HI_GET_RGB(_rgb, _r, _g, _b) { \
		(_r) = (_rgb) & 0xff; \
		(_g) = ((_rgb) >> 8) & 0xff; \
		(_b) = ((_rgb) >> 16) & 0xff; \
	}

/* this 'dims' the colors for every second line of pixels
 * This is a very very cheap filter but it works really well!
 */
#define C_SCANLINE_MASK 0xffc0c0c0

// these are more or less arbitrary orders really
enum mii_video_color_mode_e {
	CI_BLACK = 0,
	CI_PURPLE, CI_GREEN, CI_BLUE, CI_ORANGE, CI_WHITE, CI_MAGENTA,
	CI_DARKBLUE,CI_DARKGREEN,CI_GRAY1,CI_GRAY2,CI_LIGHTBLUE,
	CI_BROWN,CI_PINK,CI_YELLOW,CI_AQUA,
};
typedef struct mii_palette_t {
	const char * 	name;
	uint32_t 		mono_color;
	mii_color_t 	color[16];
} mii_palette_t;

static const mii_palette_t palettes[] = {
	[0] = {
		.name = "Color NTSC",
		.color = {
			[CI_BLACK] 		= HI_C_RGB(0x00,0x00,0x00),
			[CI_PURPLE] 	= HI_C_RGB(0xff,0x44,0xfd),
			[CI_GREEN] 		= HI_C_RGB(0x14,0xf5,0x3c),
			[CI_BLUE] 		= HI_C_RGB(0x14,0xcf,0xfd),
			[CI_ORANGE] 	= HI_C_RGB(0xff,0x6a,0x3c),
			[CI_WHITE] 		= HI_C_RGB(0xff,0xff,0xff),
			[CI_MAGENTA] 	= HI_C_RGB(0xe3,0x1e,0x60),
			[CI_DARKBLUE] 	= HI_C_RGB(0x60,0x4e,0xbd),
			[CI_DARKGREEN] 	= HI_C_RGB(0x00,0xa3,0x60),
			[CI_GRAY1] 		= HI_C_RGB(0x9c,0x9c,0x9c),
			[CI_GRAY2] 		= HI_C_RGB(0x9c,0x9c,0x9c),
			[CI_LIGHTBLUE] 	= HI_C_RGB(0xd0,0xc3,0xff),
			[CI_BROWN] 		= HI_C_RGB(0x60,0x72,0x03),
			[CI_PINK] 		= HI_C_RGB(0xff,0xa0,0xd0),
			[CI_YELLOW] 	= HI_C_RGB(0xd0,0xdd,0x8d),
			[CI_AQUA] 		= HI_C_RGB(0x72,0xff,0xd0),
		},
	},
	[1] = {
		.name = "NTSC 2",
		.color = {
			[CI_BLACK]		= HI_C_RGB(0x00,0x00,0x00),
			[CI_MAGENTA] 	= HI_C_RGB(0x9F,0x1B,0x48),
			[CI_DARKBLUE] 	= HI_C_RGB(0x48,0x32,0xEB),
			[CI_PURPLE] 	= HI_C_RGB(0xD6,0x43,0xFF),
			[CI_DARKGREEN]	= HI_C_RGB(0x19,0x75,0x44),
			[CI_GRAY1]		= HI_C_RGB(0x81,0x81,0x81),
			[CI_BLUE] 		= HI_C_RGB(0x36,0x92,0xFF),
			[CI_LIGHTBLUE]	= HI_C_RGB(0xB8,0x9E,0xFF),
			[CI_BROWN] 		= HI_C_RGB(0x49,0x65,0x00),
			[CI_ORANGE] 	= HI_C_RGB(0xD8,0x73,0x00),
			[CI_GRAY2] 		= HI_C_RGB(0x81,0x81,0x81),
			[CI_PINK] 		= HI_C_RGB(0xFB,0x8F,0xBC),
			[CI_GREEN] 		= HI_C_RGB(0x3C,0xCC,0x00),
			[CI_YELLOW] 	= HI_C_RGB(0xBC,0xD6,0x00),
			[CI_AQUA] 		= HI_C_RGB(0x6C,0xE6,0xB8),
			[CI_WHITE] 		= HI_C_RGB(0xF1,0xF1,0xF1),
		},
	},
	[2] = {
		.name = "Color Mega2",
		.color = {
			[CI_BLACK	] = HI_C_RGB(0x00,0x00,0x00),
			[CI_MAGENTA	] = HI_C_RGB(0xDB,0x1F,0x42),
			[CI_DARKBLUE] = HI_C_RGB(0x0C,0x11,0xA4),
			[CI_PURPLE	] = HI_C_RGB(0xDC,0x43,0xE1),
			[CI_DARKGREEN]= HI_C_RGB(0x1C,0x82,0x31),
			[CI_GRAY1	] = HI_C_RGB(0x63,0x63,0x63),
			[CI_BLUE	] = HI_C_RGB(0x39,0x3D,0xFF),
			[CI_LIGHTBLUE]= HI_C_RGB(0x7A,0xB3,0xFF),
			[CI_BROWN	] = HI_C_RGB(0x91,0x64,0x00),
			[CI_ORANGE	] = HI_C_RGB(0xFA,0x77,0x00),
			[CI_GRAY2	] = HI_C_RGB(0xB3,0xB3,0xB3),
			[CI_PINK	] = HI_C_RGB(0xFB,0xA5,0x93),
			[CI_GREEN	] = HI_C_RGB(0x40,0xDE,0x00),
			[CI_YELLOW	] = HI_C_RGB(0xFE,0xFE,0x00),
			[CI_AQUA	] = HI_C_RGB(0x67,0xFC,0xA3),
			[CI_WHITE	] = HI_C_RGB(0xFF,0xFF,0xFF),
		},
	},
	[3] = {
		.name = "Green",
		.mono_color = HI_C_RGB(0x14, 0xf5, 0x3c)
	},
	[4] = {
		.name = "Amber",
		.mono_color = HI_C_RGB(0xfd, 0xcf, 0x14),
	},
};

/*
 * All video mode colors. Note that this is not REALLY a color palette in this
 * state, instead, it is a color index in the palette that has been chosen by
 * the user... The set_video_mode function will synthetize the actual colors,
 * as well as the 'dim' variant use for artifacts.
 */
const mii_video_clut_t mii_base_clut = {
	.lores = {{
		[0x0] = CI_BLACK,	[0x1] = CI_MAGENTA,	[0x2] = CI_DARKBLUE,[0x3] = CI_PURPLE,
		[0x4] = CI_DARKGREEN,[0x5] = CI_GRAY1,	[0x6] = CI_BLUE,	[0x7] = CI_LIGHTBLUE,
		[0x8] = CI_BROWN,	[0x9] = CI_ORANGE,	[0xa] = CI_GRAY2,	[0xb] = CI_PINK,
		[0xc] = CI_GREEN,	[0xd] = CI_YELLOW,	[0xe] = CI_AQUA,	[0xf] = CI_WHITE,
		},{
		[0x0] = CI_BLACK,	[0x1] = CI_DARKBLUE,[0x2] = CI_DARKGREEN,[0x3] = CI_BLUE,
		[0x4] = CI_BROWN,	[0x5] = CI_GRAY2,	[0x6] = CI_GREEN,	[0x7] = CI_AQUA,
		[0x8] = CI_MAGENTA,	[0x9] = CI_PURPLE,	[0xa] = CI_GRAY1,	[0xb] = CI_LIGHTBLUE,
		[0xc] = CI_ORANGE,	[0xd] = CI_PINK,	[0xe] = CI_YELLOW,	[0xf] = CI_WHITE,
	} },
	.dhires = {
		[0x0] = CI_BLACK,	[0x1] = CI_MAGENTA,	[0x2] = CI_BROWN,	[0x3] = CI_ORANGE,
		[0x4] = CI_DARKGREEN,[0x5] = CI_GRAY1,	[0x6] = CI_GREEN,	[0x7] = CI_YELLOW,
		[0x8] = CI_DARKBLUE,[0x9] = CI_PURPLE,	[0xa] = CI_GRAY2,	[0xb] = CI_PINK,
		[0xc] = CI_BLUE,	[0xd] = CI_LIGHTBLUE,[0xe] = CI_AQUA,	[0xf] = CI_WHITE,
	},
	.hires = {
		CI_BLACK, CI_PURPLE, CI_GREEN, CI_GREEN, CI_PURPLE,
		CI_BLUE, CI_ORANGE, CI_ORANGE, CI_BLUE, CI_WHITE,
	},
#if 0
	.hires2 = {
		CI_BLACK, CI_GREEN, CI_PURPLE, CI_WHITE,
		CI_BLACK, CI_ORANGE, CI_BLUE, CI_WHITE,
	},
#endif
	.mono = { CI_BLACK, CI_WHITE },
};


// Used for DHRES decoding
static inline uint8_t reverse4(uint8_t b) {
	b = (b & 0b0001) << 3 | (b & 0b0010) << 1 |
		(b & 0b0100) >> 1 | (b & 0b1000) >> 3;
	return b;
}
static inline uint8_t reverse8(uint8_t b) {
	b = reverse4(b) << 4 | reverse4(b >> 4);
	return b;
}

static inline int
_mii_addr_to_line_text_lores(
		uint16_t a)	// ZERO based, not 0x400 based
{
	int hole = (a & 0x7f) > 0x77;
	if (hole)
		return -1;
	int group = ((a >> 7) & 0x7);
	int gline = (a & 0x7f) / 40;
	int line = (group + (gline * 8)) * 8;
	return line;
}
/*
 * check if addr is in the current text page, including page2 switch --
 * this also works for lowres (and mixed mode)
 */
static void
_mii_line_check_text_lores(
		struct mii_video_t *video,
		uint32_t 			sw,
		uint16_t 			addr)
{
	bool 	store80 = SWW_GETSTATE(sw, SW80STORE);
	bool 	page2 	= store80 ? 0 : SWW_GETSTATE(sw, SWPAGE2);
	uint16_t a = 0x400 + (0x400 * page2);

	// if addr is in the text/lores page, convert the addr into a line number
	// and mark that line as dirty. In this particular, case, we need to mark
	// 7 video lines as dirty obviously
	if (addr >= a && addr < a + 0x400) {
		a = addr - a;
		int line = _mii_addr_to_line_text_lores(a);
		if (line < 0)
			return;
		for (int i = line; i < line + 8; i++) {
			video->lines_dirty[i / 64] |= 1ULL << (i & 63);
		}
	}
}

/*
 * check if addr is in the current graphic page, including page2 switch
 * We don't have to care about dhires etc, as either bank would be same addresses
 *
 * This means in the unlikely case where code writes to the aux memory at the
 * same address as the main memory for that graphic mode, we will dirty lines
 * that don't need to be dirty, but it is a small price to pay for this
 * unusual case. Also, this would just have a slight impact on performance anyway.
 *
 * Also handle mixed mode, when mixed is on, check the bottom lines of text
 * for any changes
 */
static void
_mii_line_check_hires_dires(
		struct mii_video_t *video,
		uint32_t 			sw,
		uint16_t 			addr)
{
	bool 	store80 = SWW_GETSTATE(sw, SW80STORE);
	bool 	page2 	= store80 ? 0 : SWW_GETSTATE(sw, SWPAGE2);
	bool	mixed 	= SWW_GETSTATE(sw, SWMIXED);
	uint16_t a = (0x2000 + (0x2000 * page2));

	if (addr >= a && addr < a + 0x2000) {
		a = addr - a;
		int hole = (a & 0x78) == 0x78;// (a & 0x7f) > 0x77;
		if (hole)
			return;
		int g = ((a >> 7) & 0x7);
		int g2 = (a >> 10) & 0x7;
		int gline = (a & 0x7f) / 40;
		int line = (gline * 64) + (g * 8) + g2;

		if (!mixed || line < MII_VIDEO_MIXED_LINE)
			video->lines_dirty[line / 64] |= 1ULL << (line & 63);
	}
	if (mixed) {
		a = 0x400 + (0x400 * page2);
		if (addr >= a && addr < a + 0x400) {
			a = addr - a;
			int line = _mii_addr_to_line_text_lores(a);
			if (line < MII_VIDEO_MIXED_LINE)
				return;
			for (int i = line; i < line + 8; i++) {
				video->lines_dirty[i / 64] |= 1ULL << (i & 63);
			}
		}
	}
}

#if !MII_RP2350
// Desktop-only rendering functions that use the large pixels[] buffer

static void
_mii_line_render_dhires_mono(
		struct mii_video_t *video,
		uint32_t 			sw,
		mii_bank_t * 		main,
		mii_bank_t * 		aux )
{
	bool page2 	= SWW_GETSTATE(sw, SW80STORE) ? 0 : SWW_GETSTATE(sw, SWPAGE2);
	uint16_t a = (0x2000 + (0x2000 * page2));

	video->base_addr = a;
	a = _mii_line_to_video_addr(a, video->line);
	video->line_addr = a;
	uint32_t * screen = video->pixels +
						(video->line * MII_VIDEO_WIDTH * 2);

	const uint32_t clut[2] = {
			video->clut.mono[0],
			video->clut.mono[1] };
	for (int x = 0; x < 40; x++) {
		uint32_t ext = (mii_bank_peek(aux, a + x) & 0x7f) |
						((mii_bank_peek(main, a + x) & 0x7f) << 7);
		for (int bi = 0; bi < 14; bi++) {
			uint8_t pixel = (ext >> bi) & 1;
			uint32_t col = clut[pixel];
			*screen++ = col;
		}
	}
}

/* get exactly 1 bits from position bit from the buffer */
static inline uint8_t
_mii_get_1bits(
		uint8_t * buffer,
		int bit)
{
	int in_byte = (bit) / 8;
	int in_bit 	= 7 - ((bit) % 8);
	uint8_t b = (buffer[in_byte] >> in_bit) & 1;
	return b;
}


static void
_mii_line_render_dhires_color(
		struct mii_video_t *video,
		uint32_t 			sw,
		mii_bank_t * 		main,
		mii_bank_t * 		aux )
{
	bool page2 	= SWW_GETSTATE(sw, SW80STORE) ? 0 : SWW_GETSTATE(sw, SWPAGE2);
	uint16_t a = (0x2000 + (0x2000 * page2));

	video->base_addr = a;
	a = _mii_line_to_video_addr(a, video->line);
	video->line_addr = a;
	uint32_t * screen = video->pixels +
						(video->line * MII_VIDEO_WIDTH * 2);

	uint8_t bits[71] = { 0 };

	for (int x = 0; x < 80; x++) {
		uint8_t b = mii_bank_peek(x & 1 ? main : aux, a + (x / 2));
		// this reverse the 7 bits of each bytes into the bit buffer
		for (int i = 0; i < 7; i++) {
			int out_index = 2 + (x * 7) + i;
			int out_byte = out_index / 8;
			int out_bit = 7 - (out_index % 8);
			int bit = (b >> i) & 1;
			bits[out_byte] |= bit << out_bit;
		}
	}
	// destination pixels are offset by 2 pixels, so the image is centered
	// with an 'artifact' on the left and right side, seems to match pictures I've
	// seen on TFT screens.
	for (int i = 0, d = 2; i < 560; i++, d++) {
        const uint8_t pixel =
			(_mii_get_1bits(bits, i + 3) << (3 - ((d + 3) % 4))) +
			(_mii_get_1bits(bits, i + 2) << (3 - ((d + 2) % 4))) +
			(_mii_get_1bits(bits, i + 1) << (3 - ((d + 1) % 4))) +
			(_mii_get_1bits(bits, i) << (3 - (d % 4)));
		uint32_t col = video->clut.dhires[pixel];
		*screen++ = col;
	}
}

static void
_mii_line_render_hires(
		struct mii_video_t *video,
		uint32_t 			sw,
		mii_bank_t * 		main,
		mii_bank_t * 		aux )
{
	bool page2 	= SWW_GETSTATE(sw, SW80STORE) ? 0 : SWW_GETSTATE(sw, SWPAGE2);
	uint16_t a = (0x2000 + (0x2000 * page2));

	video->base_addr = a;
	a = _mii_line_to_video_addr(a, video->line);
	video->line_addr = a;
	uint32_t * screen = video->pixels +
						(video->line * MII_VIDEO_WIDTH * 2);
	uint8_t *src = main->mem;

	uint8_t b0 = 0;
	uint8_t b1 = src[a + 0];
	uint32_t lastcol = 0;
	for (int x = 0; x < 40; x++) {
		// last columns are clear, don't wrap around
		uint8_t b2 	= x == 39 ? 0 : src[a + x + 1];
		// last 2 pixels, current 7 pixels, next 2 pixels
		uint16_t run =  ((b0 & 0x60) >> ( 5 )) |
						((b1 & 0x7f) << ( 2 )) |
						((b2 & 0x03) << ( 9 ));
		int odd = (x & 1) << 1;
		int offset = (b1 & 0x80) >> 5;
		if (!video->monochrome) {
			for (int i = 0; i < 7; i++) {
				uint8_t left = (run >> (1 + i)) & 1;
				uint8_t pixel = (run >> (2 + i)) & 1;
				uint8_t right = (run >> (3 + i)) & 1;

				int idx = 0;	// black
				if (pixel) {
					if (left || right) {
						idx = 9;	// white
					} else {
						idx = offset + odd + (i & 1) + 1;
					}
				} else {
					if (left && right) {
						idx = offset + odd + 1 - (i & 1) + 1;
					}
				}
				uint32_t col = video->clut.hires[idx];
				if (col != lastcol) {
					uint32_t nc  = video->clut_low.hires[idx];
					*screen++ = nc; //col & C_SCANLINE_MASK;
					*screen++ = nc; //col & C_SCANLINE_MASK;
					lastcol = col;
				} else {
					*screen++ = col;
					*screen++ = col;
				}
			}
		} else {
			for (int i = 0; i < 7; i++) {
				uint8_t pixel = (run >> (2 + i)) & 1;
				uint32_t col = video->clut.mono[pixel];
				if (col != lastcol) {
					*screen++ = col & C_SCANLINE_MASK;
					lastcol = col;
				} else
					*screen++ = col;
				*screen++ = col;
			}
		}
		b0 = b1;
		b1 = b2;
	}
}

static void
_mii_line_render_text(
		struct mii_video_t *video,
		uint32_t 			sw,
		mii_bank_t * 		main,
		mii_bank_t * 		aux )
{
	bool page2 	= SWW_GETSTATE(sw, SW80STORE) ? 0 : SWW_GETSTATE(sw, SWPAGE2);
	uint16_t a = (0x400 + (0x400 * page2));

	video->base_addr = a;
	int i = video->line >> 3;
	a += ((i & 0x07) << 7) | ((i >> 3) << 5) | ((i >> 3) << 3);
	video->line_addr = a;
	const uint8_t *rom_base = video->rom->rom;

	// International ROMS are 8K, the rom_bank variable allows switching between
	// the default (0) US ROM and the International ROM
	rom_base += video->rom->len > (4*1024) && video->rom_bank ? (4*1024) : 0;

	bool 	col80 	= SWW_GETSTATE(sw, SW80COL);
	bool 	altset 	= SWW_GETSTATE(sw, SWALTCHARSET);
	int 	flash 	= video->frame_count & MII_VIDEO_FLASH_FRAME_MASK ?
							-0x40 : 0x40;
	uint32_t * screen = video->pixels +
						(video->line * MII_VIDEO_WIDTH * 2);

	for (int x = 0; x < 40 + (40 * col80); x++) {
		uint8_t c = 0;
		if (col80)
			c = mii_bank_peek(x & 1 ? main : aux, a + (x >> 1));
		else
			c = mii_bank_peek(main, a + x);
		if (!altset) {
			if (c >= 0x40 && c <= 0x7f)
				c = (int)c + flash;
		}
		const uint8_t * rom = rom_base + (c << 3);
		uint8_t bits = rom[video->line & 0x07];
		for (int pi = 0; pi < 7; pi++) {
			uint8_t pixel = (bits >> pi) & 1;
			uint32_t col = video->clut.mono[!pixel];
			*screen++ = col;
			if (!col80)
				*screen++ = col;
		}
	}
}

static void
_mii_line_render_lores(
		struct mii_video_t *video,
		uint32_t 			sw,
		mii_bank_t * 		main,
		mii_bank_t * 		aux )
{
	bool page2 	= SWW_GETSTATE(sw, SW80STORE) ? 0 : SWW_GETSTATE(sw, SWPAGE2);
	uint16_t a = (0x400 + (0x400 * page2));

	video->base_addr = a;
	int i = video->line >> 3;
	a += ((i & 0x07) << 7) | ((i >> 3) << 5) | ((i >> 3) << 3);
	video->line_addr = a;

	bool 	col80 	= SWW_GETSTATE(sw, SW80COL);
	uint32_t * screen = video->pixels +
						(video->line * MII_VIDEO_WIDTH * 2);
	mii_video_clut_t * clut = &video->clut;
	mii_video_clut_t * clut_low = &video->clut_low;
	uint32_t lastcolor = 0;
	for (int x = 0; x < 40 + (40 * col80); x++) {
		uint16_t c = 0;
		if (col80)
			c = mii_bank_peek(x & 1 ? main : aux, a + (x >> 1));
		else
			c = mii_bank_peek(main, a + x);

		int lo_line = video->line / 4;
		c = (c >> ((lo_line & 1) * 4)) & 0xf;
//		c |= (c << 4);
		uint32_t color = clut->lores[(x & col80) ^ col80][c & 0x0f];
		uint32_t dim = clut_low->lores[(x & col80) ^ col80][c & 0x0f];
		if (!video->monochrome) {
			for (int pi = 0; pi < 7; pi++) {
				uint32_t pixel = color;
				if (pixel != lastcolor) {
					pixel = dim;
					lastcolor = color;
				}
				*screen++ = pixel;
				if (!col80)
					*screen++ = pixel;
			}
		} else {
			c = reverse4(c);
			c |= c << 4;
			c |= c << 8;
			if (col80) {
				for (int pi = 0; pi < 7; pi++) {
					uint8_t b = (c >> pi) & 1;
					uint32_t pixel = b ? color : dim;
					*screen++ = pixel;
				}
			} else {
				if (x & 1) c >>= 2;
				for (int pi = 0; pi < 14; pi++) {
					uint8_t b = (c >> pi) & 1;
					uint32_t pixel = b ? color : dim;
					*screen++ = pixel;
				}
			}
		}
	}
}

#endif // !MII_RP2350 - end of desktop render functions

#if MII_RP2350
// RP2350 stubs for callback-based rendering system (we use our own direct render)
static void
_mii_video_mark_dirty(
		mii_video_t *video)
{
	video->frame_dirty = 1;
}

static void
_mii_video_mode_changed(
		mii_video_t *video,
		uint32_t sw_state)
{
	(void)sw_state;
	video->frame_dirty = 1;
}

void
mii_video_OOB_write_check(
		mii_t *mii,
		uint16_t addr,
		uint16_t size)
{
	(void)addr;
	(void)size;
	mii->video.frame_dirty = 1;
}

/*
 * Lightweight VBL timer for RP2350.
 * This ONLY handles the VBL soft switch timing - no pixel rendering.
 * Games depend on accurate VBL timing for animation pacing.
 * 
 * VBL timing per https://rich12345.tripod.com/aiivideo/vbl.html:
 * - 192 visible lines * 65 cycles = 12480 cycles (VBL = 0x80)
 * - VBL period = 4550 cycles (VBL = 0x00)
 * - Total frame = 17030 cycles
 */
static uint64_t
mii_video_vbl_timer_cb(
		mii_t *mii,
		void *param)
{
	(void)param;
	mii_bank_t * sw = &mii->bank[MII_BANK_SW];
	mii_video_t * video = &mii->video;
	
	// Apple II VBL convention:
	// - During visible area: VBL bit 7 = 0 (value 0x00)
	// - During vblank: VBL bit 7 = 1 (value 0x80)
	// vbl_phase: 0 = visible, 1 = vblank
	if (video->vbl_phase == 0) {
		// End of visible area, entering vblank - SET bit 7
		mii_bank_poke(sw, SWVBL, 0x80);
		video->vbl_phase = 1;
		video->frame_count++;
		return (uint64_t)(MII_VBL_UP_CYCLES * mii->speed);
	} else {
		// End of vblank, starting visible area - CLEAR bit 7
		mii_bank_poke(sw, SWVBL, 0x00);
		video->vbl_phase = 0;
		return (uint64_t)(MII_VBL_DOWN_CYCLES * mii->speed);
	}
}

/*
 * Reset the VBL timer to a known good state.
 * Call this after long operations (like disk loading) that may have
 * caused the timer to become inactive due to accumulating negative cycles.
 */
void
mii_video_reset_vbl_timer(mii_t *mii)
{
	mii_video_t *video = &mii->video;
	mii_bank_t *sw = &mii->bank[MII_BANK_SW];
	
	// Reset to start of visible area
	video->vbl_phase = 0;
	mii_bank_poke(sw, SWVBL, 0x00);
	
	// Set timer to a positive value - must be > 0 for timer to run
	mii_timer_set(mii, video->timer_id, MII_VBL_DOWN_CYCLES);
	
	// Reset last_run to current cycle count
	mii->timer.last_run = mii->cpu.total_cycle + mii->cpu.cycle;
}

#else // !MII_RP2350
/*
 * This return the correct line drawing function callback for the mode
 * and softswitches
 */
static mii_video_cb_t
_mii_video_get_line_render_cb(
		mii_video_t *video,
		uint32_t sw_state)
{
	mii_video_cb_t res = { 0 };

	bool	text 	= SWW_GETSTATE(sw_state, SWTEXT);
	bool 	col80 	= SWW_GETSTATE(sw_state, SW80COL);
	bool 	hires 	= SWW_GETSTATE(sw_state, SWHIRES);
	bool 	dhires 	= SWW_GETSTATE(sw_state, SWDHIRES);

	if (hires && !text && col80 && dhires) {
		uint8_t reg = video->an3_mode;
		if (reg != 0 && !video->monochrome)
			res.render = _mii_line_render_dhires_color;
		else
			res.render = _mii_line_render_dhires_mono;
		res.check = _mii_line_check_hires_dires;
	} else if (hires && !text) {
		res.render = _mii_line_render_hires;
		res.check = _mii_line_check_hires_dires;
	} else if (text) {
		res.render = _mii_line_render_text;
		res.check = _mii_line_check_text_lores;
	} else {
		res.render = _mii_line_render_lores;
		res.check = _mii_line_check_text_lores;
	}
	return res;
}

static void
_mii_video_mark_dirty(
		mii_video_t *video)
{
	video->frame_dirty = 1;
	video->lines_dirty[0] =
			video->lines_dirty[1] =
			video->lines_dirty[2] = -1LL;
}

/*
 * This is called when the video mode changes, and we need to update the
 * line drawing callback
 */
static void
_mii_video_mode_changed(
		mii_video_t *video,
		uint32_t sw_state)
{
	mii_video_cb_t res = _mii_video_get_line_render_cb(video, sw_state);
	if (res.render != video->line_cb.render) {
		video->line_cb = res;
		_mii_video_mark_dirty(video);
	}
}


/*
 * This is called when writes are made from outside the 6502 emulation, for
 * example the DMA from smartport. Otherwise you could BLOAD an image in video
 * ram and there would be no way of knowing if the addresses *were* in the video
 * ram. So this call is used by anything doing DMA (currently just smartport)
 */
void
mii_video_OOB_write_check(
		mii_t *mii,
		uint16_t addr,
		uint16_t size)
{
	for (int i = 0; i < size; i += 40)
		mii->video.line_cb.check(&mii->video, mii->sw_state, addr + i);
}

/*
 * This is the state machine to draw a line of the video output
 * All timings lifted from https://rich12345.tripod.com/aiivideo/vbl.html
 *
 * This is a 'protothread' basically cooperative scheduling using an
 * old compiler trick. It's not a real thread, but it's a way to
 * write code that looks like a thread, and is easy to read.
 * + pt_start macro starts the 'thread' (and return to last yield point).
 * + pt_yield() yields until the function is called again.
 * + pt_end() macro ends the thread.
 * Remeber you cannot have locals in the thread, they must be
 * static or global.
 * *everything* before the pt_start call is ran every time, so you can use
 * that to reload some sort of state, as here, were we get all the
 * video mode softswitches.
 *
 * This function is also a 'cycle timer' it returns the number of 6502
 * cycles to wait until being called again, so it mostly returns the
 * number of cycles until the next horizontal blanking between each lines,
 * but also the number of cycles until the next vertical blanking once
 * the last line is drawn.
 */
static uint64_t
mii_video_timer_cb(
		mii_t *mii,
		void *param)
{
	uint64_t res = MII_VIDEO_H_CYCLES * mii->speed;

	mii_bank_t * sw = &mii->bank[MII_BANK_SW];
	uint32_t 	sw_state = mii->sw_state;
	mii_bank_t * main = &mii->bank[MII_BANK_MAIN];
	mii_bank_t * aux = &mii->bank[MII_VIDEO_BANK];
	mii_video_t * video = &mii->video;

	pt_start(video->state);
	/*
		We 'cheat' and draw a whole line at a time, then 'wait' until
		horizontal blanking, then wait until vertical blanking.
	*/
	do {
		// 'clear' VBL flag. Flag is 0 during retrace
		mii_bank_poke(sw, SWVBL, 0x80);

		mii_video_line_drawing_cb line_drawing = video->line_cb.render;
		/* If we are in mixed mode past line 160, check if we need to
		 * switch from the 'main mode' callback to the text callback */
		if (video->line >= MII_VIDEO_MIXED_LINE) {
			bool	mixed	= SWW_GETSTATE(sw_state, SWMIXED);
			if (mixed) {
				uint32_t sw 	= sw_state;
				SWW_SETSTATE(sw, SWTEXT, 1);
				if (sw != sw_state)
					line_drawing = _mii_video_get_line_render_cb(video, sw).render;
			}
		}
		if (video->lines_dirty[video->line / 64] &
								(1ULL << (video->line & 63))) {
			line_drawing(video, sw_state, main, aux);

			uint32_t * screen = video->pixels +
								(video->line * MII_VIDEO_WIDTH * 2);
			uint32_t * l2 = screen + MII_VIDEO_WIDTH;

#if defined(__AVX2__)
			const __m256i mask = _mm256_set1_epi32(C_SCANLINE_MASK);
			// Process scanline using AVX GCC intrinsic
			for (int i = 0; i < MII_VIDEO_WIDTH; i += 8) {
				__m256i src = _mm256_loadu_si256((__m256i *)(screen + i));
				__m256i result = _mm256_and_si256(src, mask);
				_mm256_storeu_si256((__m256i *)(l2 + i), result);
			}
#elif defined(__SSE2__)
			const __m128i mask = _mm_set1_epi32(C_SCANLINE_MASK);
			// Process scanline using SSE GCC intrinsic
			for (int i = 0; i < MII_VIDEO_WIDTH; i += 4) {
				__m128i src = _mm_loadu_si128((__m128i *)(screen + i));
				__m128i result = _mm_and_si128(src, mask);
				_mm_storeu_si128((__m128i *)(l2 + i), result);
			}
#else
#if 1	// generic vector code -- NEON and wasm?
			const u32_v mask = C_SCANLINE_MASK - (u32_v){};	// broadcast
			for (int i = 0; i < MII_VIDEO_WIDTH; i += VEC_ECOUNT,
							screen += VEC_ECOUNT, l2 += VEC_ECOUNT) {
				u32_v s = *(u32_v *)screen;
				s &= mask;
				*(u32_v *)l2 = s;
			}
#else
			for (int i = 0; i < MII_VIDEO_WIDTH; i++)
				*l2++ = *screen++ & C_SCANLINE_MASK;
#endif
#endif

			video->lines_dirty[video->line / 64] &=
								~(1ULL << (video->line & 63));
			video->frame_dirty = 1;
#if MII_VIDEO_DEBUG_HEAPMAP
			video->video_hmap[video->line] = 0xff;
#endif
		}
		video->line++;
		if (video->line == 192) {
			video->line = 0;
			video->line_addr = video->base_addr;
			video->timer_max = MII_VIDEO_H_CYCLES;
			res = video->timer_max * mii->speed;
			pt_yield(video->state);
			mii_bank_poke(sw, SWVBL, 0x00);
			video->timer_max = MII_VBL_UP_CYCLES;
			res = video->timer_max * mii->speed;
			/*
			 * This is to handle the corner case where text screen has some
			 * blinking text, and we need to redraw the screen.
			 *
			 * We only check every 16 frames, so we don't waste time redrawing
			 * the screen every frame. Also, the alt char set needs to be off,
			 * as the blinking text is only in the main charset.
			 */
			uint32_t new_frame = video->frame_count + 1;
			if ((new_frame & MII_VIDEO_FLASH_FRAME_MASK) !=
					(video->frame_count & MII_VIDEO_FLASH_FRAME_MASK)) {
				if (!SW_GETSTATE(mii, SWALTCHARSET))
					_mii_video_mark_dirty(video);
			}
			video->frame_count = new_frame;
			pt_yield(video->state);
			// check if we need to switch the video mode, in case the UI switches
			// Color/mono palette etc
			mii->cpu.instruction_run = 0;	// stop current instruction run!
			if (video->frame_dirty)
				video->frame_seed++;
			video->frame_dirty = 0;
		} else {
			video->timer_max = MII_VIDEO_H_CYCLES + MII_VIDEO_HB_CYCLES;
			res = video->timer_max * mii->speed;
			pt_yield(video->state);
		}
	} while (1);
	pt_end(video->state);
	return res;
}
#endif // !MII_RP2350 - end of desktop callback system

/*
 * TODO: this doesn't work yet. Don't get overexcited about this.
 * Or, get overexcited about this and fix it! :-)
 *
 * This is mostly how it's supposed to work anyway. Any 'read' access should
 * be able to deduce where the 'beam' is by looking at what the remaining
 * timer cycles are, and the current line.
 * And then deduce the exact position horizontally on the screen.
 *
 * But, it doesn't really work for now.
 */
uint8_t
mii_video_get_vapor(
		mii_t *mii)
{
	uint8_t res = 0;
	int64_t timer = mii_timer_get(mii, mii->video.timer_id);
	timer = timer / mii->speed;
	uint16_t addr = mii->video.line_addr;
	int64_t current = mii->video.timer_max - timer;
	addr += current - 25;
	res = mii_bank_peek(&mii->bank[MII_BANK_MAIN], addr);
//	printf("VAPOR %5ld/%5ld %04x->%04x %02x\n",
//			current, mii->video.timer_max, mii->video.line_addr, addr, res);
	return res;
}

bool
mii_access_video(
		mii_t *mii,
		uint16_t addr,
		uint8_t * byte,
		bool write)
{
	bool res = false;
#if !MII_RP2350
	if (write)
		mii->video.line_cb.check(&mii->video, mii->sw_state, addr);
#else
	if (write)
		mii->video.frame_dirty = 1;
#endif
	mii_bank_t * sw = &mii->bank[MII_BANK_SW];
	switch (addr) {
		case SWALTCHARSETOFF:
		case SWALTCHARSETON:
			/*
			 * Apple II soft-switches generally trigger on both read and write.
			 * Some software uses BIT/LDA on $C0xx to flip switches.
			 */
			res = true;
			SW_SETSTATE(mii, SWALTCHARSET, addr & 1);
			mii_bank_poke(sw, SWALTCHARSET, (addr & 1) << 7);
			// in case there is some blinking text, we need to redraw
			_mii_video_mark_dirty(&mii->video);
			if (!write)
				*byte = mii_video_get_vapor(mii);
			break;
		case SWVBL:
		case SW80COL:
		case SWTEXT:
		case SWMIXED:
		case SWPAGE2:
		case SWHIRES:
		case SWALTCHARSET:
		case SWRDDHIRES:
			res = true;
			/* we OR the return flag, as the lower 7 bits are keyboard related */
			if (!write) {
				*byte |= mii_bank_peek(sw, addr);
			}
			break;
		case SWHIRESOFF:
		case SWHIRESON:
		//	res = true;	// we return false here, so generic SW code is called
			SW_SETSTATE(mii, SWHIRES, addr & 1);
			mii_bank_poke(sw, SWHIRES, (addr & 1) << 7);
			_mii_video_mode_changed(&mii->video, mii->sw_state);
			break;
		case SWPAGE2OFF:
		case SWPAGE2ON:
		//	res = true;	// we return false here, so generic SW code is called
			SW_SETSTATE(mii, SWPAGE2, addr & 1);
			mii_bank_poke(sw, SWPAGE2, (addr & 1) << 7);
			if (!write)
				*byte = mii_bank_peek(sw, SWPAGE2);
			// 80STORE completely changes the meaning of PAGE2
			if (!SW_GETSTATE(mii, SW80STORE)) {
				_mii_video_mode_changed(&mii->video, mii->sw_state);
				_mii_video_mark_dirty(&mii->video);
			}
		 	break;
		case SW80COLOFF:
		case SW80COLON:
			/*
			 * 80COL is toggled by access (read or write). Games commonly use BIT.
			 * If we ignore reads here, 80COL never turns on and DHGR can't render.
			 */
			res = true;
			SW_SETSTATE(mii, SW80COL, addr & 1);
			mii_bank_poke(sw, SW80COL, (addr & 1) << 7);
			_mii_video_mode_changed(&mii->video, mii->sw_state);
			if (!write)
				*byte = mii_video_get_vapor(mii);
			break;
		case SWDHIRESOFF: 	//  0xc05f,
		case SWDHIRESON: { 	// = 0xc05e,
			res = true;
			mii_video_t * video = &mii->video;
			uint8_t an3 = !!mii_bank_peek(sw, SWAN3);
			bool an3_on = !!(addr & 1); // 5f is ON, 5e is OFF
			uint8_t reg = mii_bank_peek(sw, SWAN3_REGISTER);
			if (an3_on && !an3) {
				uint8_t bit = SW_GETSTATE(mii, SW80COL);
				reg = ((reg << 1) | bit) & 3;
			//	printf("VIDEO 80:%d REG now %x\n", bit, reg);
				video->an3_mode = reg;
				mii_bank_poke(sw, SWAN3_REGISTER, reg);
			}
			mii_bank_poke(sw, SWAN3, an3_on ? 0x80 : 0);
		//	printf("DHRES IS %s mode:%d\n", (addr & 1) ? "OFF" : "ON ", reg);
			SW_SETSTATE(mii, SWDHIRES, !(addr & 1));
			mii_bank_poke(sw, SWRDDHIRES, (!(addr & 1)) << 7);
			_mii_video_mark_dirty(video);
			_mii_video_mode_changed(&mii->video, mii->sw_state);
		}	break;
		case SWTEXTOFF:
		case SWTEXTON:
			res = true;
			SW_SETSTATE(mii, SWTEXT, addr & 1);
			mii_bank_poke(sw, SWTEXT, (addr & 1) << 7);
			_mii_video_mode_changed(&mii->video, mii->sw_state);
			if (!write)
				*byte = mii_video_get_vapor(mii);
			break;
		case SWMIXEDOFF:
		case SWMIXEDON:
			res = true;
			SW_SETSTATE(mii, SWMIXED, addr & 1);
			mii_bank_poke(sw, SWMIXED, (addr & 1) << 7);
			_mii_video_mode_changed(&mii->video, mii->sw_state);
			if (!write)
				*byte = mii_video_get_vapor(mii);
			break;
	}
	return res;
}

/*
 * This is mostly a debug function, to be able to 'force' a full screen
 * refresh, for example when the emulator is paused, and you want to see
 * the screen 'as it is' for example if you poke memory while debugging.
 */
#if !MII_RP2350
void
mii_video_full_refresh(
		mii_t *mii)
{
	_mii_video_mark_dirty(&mii->video);

	if (mii->state == MII_RUNNING)
		return;

	mii_bank_t * sw = &mii->bank[MII_BANK_SW];
	_mii_video_mark_dirty(&mii->video);
	do {
		mii_video_timer_cb(mii, NULL);
	} while (mii_bank_peek(sw, SWVBL));
	do {
		mii_video_timer_cb(mii, NULL);
	} while (!mii_bank_peek(sw, SWVBL));
}
#else // MII_RP2350
void
mii_video_full_refresh(
		mii_t *mii)
{
	mii->video.frame_dirty = 1;
}
#endif // MII_RP2350

void
mii_video_init(
	mii_t *mii)
{
	mii_video_t * video = &mii->video;
	video->rom = mii_rom_get(
			mii->emu == MII_EMU_IIC ? "iic_video" : "iiee_video");
#if MII_RP2350
	// RP2350: Use lightweight VBL-only timer for proper game timing
	// Start in visible phase
	video->vbl_phase = 0;
	mii->video.timer_id = mii_timer_register(mii,
				mii_video_vbl_timer_cb, NULL, MII_VBL_DOWN_CYCLES, "vbl_timer");
	MII_DEBUG_PRINTF("VBL timer registered (id=%d)\n", mii->video.timer_id);
#else
	mii->video.timer_id = mii_timer_register(mii,
				mii_video_timer_cb, NULL, MII_VIDEO_H_CYCLES, __func__);
#endif
	// start the DHRES in color
	mii_bank_t * sw = &mii->bank[MII_BANK_SW];
	mii_bank_poke(sw, SWAN3_REGISTER, 1);
	_mii_video_mode_changed(&mii->video, mii->sw_state);
	mii_video_set_mode(mii, 0);
}

typedef struct {
	double r,g,b; 	// a fraction between 0 and 1
} frgb_t;

typedef struct {
	double h;		// angle in degrees
	double s,v;		// a fraction between 0 and 1
} fhsv_t;

static fhsv_t
rgb2hsv(frgb_t in)
{
	fhsv_t out;
	double min, max, delta;

	min = in.r < in.g ? in.r : in.g;
	min = min < in.b ? min : in.b;
	max = in.r > in.g ? in.r : in.g;
	max = max > in.b ? max : in.b;

	out.v = max; // v
	delta = max - min;
	if (delta < 0.00001) {
		out.s = 0;
		out.h = 0; // undefined, maybe nan?
		return out;
	}
	if (max > 0.0) { // NOTE: if Max is == 0, this divide would cause a crash
		out.s = (delta / max); // s
	} else {
		// if max is 0, then r = g = b = 0
		// s = 0, h is undefined
		out.s = 0.0;
		out.h = NAN; // its now undefined
		return out;
	}
	if (in.r >= max) // > is bogus, just keeps compilor happy
		out.h = (in.g - in.b) / delta; // between yellow & magenta
	else if (in.g >= max)
		out.h = 2.0 + (in.b - in.r) / delta; // between cyan & yellow
	else
		out.h = 4.0 + (in.r - in.g) / delta; // between magenta & cyan
	out.h *= 60.0; // degrees
	if (out.h < 0.0)
		out.h += 360.0;
	return out;
}

static frgb_t
hsv2rgb(fhsv_t in)
{
	double hh, p, q, t, ff;
	long i;
	frgb_t out;

	if (in.s <= 0.0) {
		out.r = out.g = out.b = in.v;
		return out;
	}
	hh = in.h;
	if (hh >= 360.0) {
		hh = 0.0;
	}
	hh /= 60.0;
	i = (long)hh;
	ff = hh - i;
	p = in.v * (1.0 - in.s);
	q = in.v * (1.0 - (in.s * ff));
	t = in.v * (1.0 - (in.s * (1.0 - ff)));

	switch (i) {
		case 0: out.r = in.v; out.g = t; out.b = p; break;
		case 1: out.r = q; out.g = in.v; out.b = p; break;
		case 2: out.r = p; out.g = in.v; out.b = t; break;
		case 3: out.r = p; out.g = q; out.b = in.v; break;
		case 4: out.r = t; out.g = p; out.b = in.v; break;
		case 5:
		default: out.r = in.v; out.g = p; out.b = q; break;
	}
	return out;
}

/*
 * Takes a RGB color, and a base color, and returns a color that is
 * the same luminance as the RGB color, but with the hue of the base color
 * This is not an exact formula, and there are some chroma drifts, but it
 * will do for now.
 */
static inline uint32_t
_mii_rgb_to_lumed_color(
		uint32_t rgb,
		uint32_t base)
{
#if 0
	uint8_t r, g, b;
	HI_GET_RGB(rgb, r, g, b);
	uint8_t br, bg, bb;
	HI_GET_RGB(base, br, bg, bb);
	frgb_t in = { r / 255.0, g / 255.0, b / 255.0 };
	frgb_t base_in = { br / 255.0, bg / 255.0, bb / 255.0 };
	fhsv_t hsv = rgb2hsv(in);
	fhsv_t base_hsv = rgb2hsv(base_in);
	fhsv_t n = base_hsv;
	n.v *= hsv.v;
	frgb_t out = hsv2rgb(n);
	r = out.r * 255;
	g = out.g * 255;
	b = out.b * 255;
	return HI_RGB(r, g, b);
#else
	uint8_t r, g, b;
	HI_GET_RGB(rgb, r, g, b);
	uint8_t l = HI_LUMA(r, g, b);
	if (l == 0)
		return HI_RGB(0,0,0);
	uint8_t br, bg, bb;
	HI_GET_RGB(base, br, bg, bb);
//	uint8_t bl = HI_LUMA(br, bg, bb);
	r = (br * l) / 255;
	g = (bg * l) / 255;
	b = (bb * l) / 255;
	rgb = HI_RGB(r, g, b);
	return rgb;
#endif
}

/*
 * All this kitchen mess is to implement a 'palette' system, where we can
 * cycle through different color palettes. The palettes are defined in the
 * palettes array, and the mii_video_set_mode() function is used to switch
 * between them.
 *
 * It calculates a 'dimmed' version of the colors, and stores them in the
 * clut_low structure, the 'dimmed' colors are used for creating artifacts.
 */
void
mii_video_set_mode(
		mii_t *mii,
		uint8_t mode)
{
	mii_video_t * video = &mii->video;
	// used to implement cycling through palettes
	if (mode >= (sizeof(palettes) / sizeof(palettes[0])))
		mode = 0;
//	printf("%s mode %d\n", __func__, mode);
	video->color_mode = mode;
	mii_video_clut_t * clut = &video->clut;

	uint32_t base = palettes[mode].mono_color;
	video->monochrome = base != 0;
	if (video->monochrome) {
		// convert one set of RGB colors to monochrome. arbitrarily 0
		const mii_palette_t * pal = &palettes[0];
		// base CLUT is using color *indexes* in the palette we picked
		for (uint i = 0; i < sizeof(clut->colors) / sizeof(clut->colors[0]); i++)
			clut->colors[i] = pal->color[mii_base_clut.colors[i]];
		for (uint i = 0; i < sizeof(clut->colors) / sizeof(clut->colors[0]); i++) {
			clut->colors[i] = _mii_rgb_to_lumed_color(
									pal->color[mii_base_clut.colors[i]], base);
		}
		// now calculate a new lores color table, with dimmer colors
		uint8_t br, bg, bb;
		HI_GET_RGB(base, br, bg, bb);
		frgb_t base_in = { br / 255.0, bg / 255.0, bb / 255.0 };
		fhsv_t base_hsv = rgb2hsv(base_in);
		base_hsv.v /= 2.0;
		frgb_t out = hsv2rgb(base_hsv);
		br = out.r * 255;
		bg = out.g * 255;
		bb = out.b * 255;
		base = HI_RGB(br, bg, bb);

		clut = &video->clut_low;
		*clut = video->clut;

		for (uint i = 0; i < sizeof(clut->colors) / sizeof(clut->colors[0]); i++) {
			clut->colors[i] = _mii_rgb_to_lumed_color(
								clut->colors[i], base);
		}
	} else {
		const mii_palette_t * pal = &palettes[mode];
		// base CLUT is using color *indexes* in the palette we picked
		for (uint i = 0; i < sizeof(clut->colors) / sizeof(clut->colors[0]); i++)
			clut->colors[i] = pal->color[mii_base_clut.colors[i]];
		clut = &video->clut_low;
		*clut = video->clut;

		for (uint i = 0; i < sizeof(clut->colors) / sizeof(clut->colors[0]); i++) {
			uint8_t br, bg, bb;
			HI_GET_RGB(clut->colors[i], br, bg, bb);
			frgb_t base_in = { br / 255.0, bg / 255.0, bb / 255.0 };
			fhsv_t base_hsv = rgb2hsv(base_in);
			base_hsv.s *= 0.75;
			base_hsv.v *= 0.75;
			frgb_t out = hsv2rgb(base_hsv);
			br = out.r * 255;
			bg = out.g * 255;
			bb = out.b * 255;
			clut->colors[i] = HI_RGB(br, bg, bb);
		}
	}
	mii_video_full_refresh(mii);
}


static void
_mii_mish_video(
		void * param,
		int argc,
		const char * argv[])
{
	mii_t * mii = param;
	mii_video_t * video = &mii->video;

	if (!argv[1]) {
		MII_DEBUG_PRINTF("VIDEO mode %d\n", video->color_mode);
		MII_DEBUG_PRINTF(" ROM %s (%s)\n", video->rom->name, video->rom->description);
		MII_DEBUG_PRINTF(" ROM bank %s\n", video->rom_bank ? "ON" : "OFF");
		MII_DEBUG_PRINTF(" AN3 mode %d\n", video->an3_mode);
		MII_DEBUG_PRINTF(" Monochrome %s\n", video->monochrome ? "ON" : "OFF");
		return;
	}
	if (!strcmp(argv[1], "clut")) {
		for (int i = 0; i < 16; i++) {
			MII_DEBUG_PRINTF("%01x: %08x %08x %08x\n", i,
					video->clut.lores[0][i],
					video->clut.lores[1][i],
					video->clut.dhires[i]);
		}
		return;
	}
	if (!strcmp(argv[1], "color")) {
		mii_bank_t * sw = &mii->bank[MII_BANK_SW];
		uint8_t reg = mii_bank_peek(sw, SWAN3_REGISTER);
		MII_DEBUG_PRINTF("AN3 REG %d -> %d\n", reg, 1);
		video->an3_mode = 1;
		mii_bank_poke(sw, SWAN3_REGISTER, 1);
		_mii_video_mode_changed(video, mii->sw_state);
		mii_video_full_refresh(mii);
		return;
	}
	if (!strcmp(argv[1], "mono")) {
		mii_bank_t * sw = &mii->bank[MII_BANK_SW];
		uint8_t reg = mii_bank_peek(sw, SWAN3_REGISTER);
		MII_DEBUG_PRINTF("AN3 REG %d -> %d\n", reg, 0);
		video->an3_mode = 0;
		mii_bank_poke(sw, SWAN3_REGISTER, 0);
		_mii_video_mode_changed(video, mii->sw_state);
		mii_video_full_refresh(mii);
		return;
	}
	if (!strcmp(argv[1], "dirty")) {
		_mii_video_mode_changed(video, mii->sw_state);
		mii_video_full_refresh(mii);
		return;
	}
	if (!strcmp(argv[1], "rom")) {
		const char * name = argv[2];
		mii_rom_t * rom = mii_rom_get_class(NULL, "video");
		while (rom && rom->name) {
			if (name && !strcmp(rom->name, name)) {
				MII_DEBUG_PRINTF("ROM set to %s (%s)\n", rom->name, rom->description);
				mii->video.rom = rom;
				mii_video_full_refresh(mii);
				return;
			} else if (!name) {
				MII_DEBUG_PRINTF("ROM %s (%s)\n", rom->name, rom->description);
			}
			rom = SLIST_NEXT(rom, self);
		}
		fprintf(stderr, "ROM %s not found\n", name);
		return;
	}
	if (!strcmp(argv[1], "bank")) {
		// toggle video_bank and display wether it will do anything with this ROM
		if (video->rom->len > (4*1024)) {
			video->rom_bank = !video->rom_bank;
			MII_DEBUG_PRINTF("ROM %s alternative bank %s\n",
					video->rom->name,
					video->rom_bank ? "ON" : "OFF");
			mii_video_full_refresh(mii);
		} else {
			MII_DEBUG_PRINTF("Video rom %s doesn't have alternative charsets\n",
					video->rom->name);
		}
		return;
	}
#ifdef TRACE
	if (!strcmp(argv[1], "trace")) {
		_trace = 1;
		mii_video_full_refresh(mii);
		_trace = 0;
		return;
	}
#endif
	fprintf(stderr, "Unknown video command %s\n", argv[1]);

	// print usage
	fprintf(stderr, "video: test patterns generator\n");
	fprintf(stderr, " <default>: dump color tables\n");
	fprintf(stderr, " list: dump color tables\n");
	fprintf(stderr, " color: set color mode\n");
	fprintf(stderr, " mono: set mono mode\n");
	fprintf(stderr, " dirty: force full refresh\n");
	fprintf(stderr, " rom <name>: set video rom\n");
	fprintf(stderr, " bank: toggle video rom bank\n");
}

#if !MII_RP2350
#include "mish.h"

MISH_CMD_NAMES(video, "video");
MISH_CMD_HELP(video,
		"video: test patterns generator",
		" <default>: dump color tables",
		" rom [<name>]: set (or list) video roms",
		" bank: toggle video rom bank (if rom is > 4k)",
		" clut: dump color tables",
		" color: set color mode",
		" mono: set mono mode",
		" dirty: force full refresh"
		);
MII_MISH(video, _mii_mish_video);
#endif

#if MII_RP2350
/*
 * RP2350-specific video rendering for HDMI output
 * Renders to a 320x240 8-bit indexed framebuffer
 */

// Map CI_* palette indices (desktop) to RP2350 palette indices (Apple II lores order)
const uint8_t rp2350_ci_to_hw[16] = {
	[CI_BLACK] = 0,
	[CI_PURPLE] = 3,
	[CI_GREEN] = 12,
	[CI_BLUE] = 6,
	[CI_ORANGE] = 9,
	[CI_WHITE] = 15,
	[CI_MAGENTA] = 1,
	[CI_DARKBLUE] = 2,
	[CI_DARKGREEN] = 4,
	[CI_GRAY1] = 5,
	[CI_GRAY2] = 10,
	[CI_LIGHTBLUE] = 7,
	[CI_BROWN] = 8,
	[CI_PINK] = 11,
	[CI_YELLOW] = 13,
	[CI_AQUA] = 14,
};

#include <pico.h>
extern volatile int lock_y;
static uint8_t line_buffer[320] __aligned(4) __scratch_x("line_buffer");

// Render text mode (40 column) to framebuffer - OPTIMIZED
static void __attribute__((hot))
mii_video_render_text40_rp2350(
		mii_t *mii,
		uint8_t *fb,
		int fb_width)
{
	mii_bank_t *main_bank = &mii->bank[MII_BANK_MAIN];
	mii_bank_t *aux_bank = &mii->bank[MII_VIDEO_BANK];
	mii_video_t *video = &mii->video;
	const uint8_t *char_rom = video->rom ? video->rom->rom : NULL;
	
	if (!char_rom) {
		return;
	}
	uint32_t sw = mii->sw_state;
	bool page2 = SWW_GETSTATE(sw, SW80STORE) ? 0 : SWW_GETSTATE(sw, SWPAGE2);
	uint16_t base_addr = 0x400 + (0x400 * page2);
	bool col80 = SWW_GETSTATE(sw, SW80COL);
	bool altset = SWW_GETSTATE(sw, SWALTCHARSET);
	const uint8_t *rom_base = char_rom;
	if (video->rom && video->rom->len > (4 * 1024) && video->rom_bank)
		rom_base += (4 * 1024);

    pin_ram_pages_for(main_bank->ua.vram_desc, base_addr, 0x400);
    if (col80)
        pin_ram_pages_for(aux_bank->ua.vram_desc, base_addr, 0x400);
	else
        pin_ram_pages_for(aux_bank->ua.vram_desc, base_addr, 0); // unpin unused
		
	uint8_t main_row[40];
	int flash = (video->frame_count & 0x10) ? -0x40 : 0x40;
	
	// Text screen is 40x24 (or 80x24 if SW80COL is on)
	// Render at 192 lines and vertically center (24 pixel offset)
	
	for (int row = 0; row < 24; row++) {
		// Apple II text memory is interleaved
		uint16_t line_addr = base_addr + (row & 7) * 0x80 + (row / 8) * 0x28;

		mii_bank_read(main_bank, line_addr, main_row, 40);
		if (!col80) {
			// 40-column mode - most common case, highly optimized
			for (int x = 0; x < 40; x++) {
				uint8_t c = main_row[x];

				// Flash handling
				if (!altset && c >= 0x40 && c <= 0x7F)
					c = (int)c + flash;

				const uint8_t *char_data = rom_base + (c << 3);
				int fb_x_base = x * 8;
				uint8_t *fb_row_base = fb + 24 * fb_width + row * 8 * fb_width + fb_x_base;

				for (int cy = 0; cy < 8; cy++) {
					uint8_t bits = char_data[cy];
					uint8_t *fb_ptr = fb_row_base + cy * fb_width;
					// Unrolled inner loop for 7 pixels + 1 padding
					fb_ptr[0] = (bits & 0x01) ? 0 : 15;
					fb_ptr[1] = (bits & 0x02) ? 0 : 15;
					fb_ptr[2] = (bits & 0x04) ? 0 : 15;
					fb_ptr[3] = (bits & 0x08) ? 0 : 15;
					fb_ptr[4] = (bits & 0x10) ? 0 : 15;
					fb_ptr[5] = (bits & 0x20) ? 0 : 15;
					fb_ptr[6] = (bits & 0x40) ? 0 : 15;
					fb_ptr[7] = 0;  // 8th pixel padding
				}
			}
		} else {
			// 80-column mode
			uint8_t aux_row[40];
			mii_bank_read(aux_bank, line_addr, aux_row, 40);
			for (int x = 0; x < 80; x++) {
				uint8_t c;
				if (x & 1)
					c = main_row[x >> 1];
				else
					c = aux_row[x >> 1];

				if (!altset && c >= 0x40 && c <= 0x7F)
					c = (int)c + flash;

				const uint8_t *char_data = rom_base + (c << 3);

				for (int cy = 0; cy < 8; cy++) {
					uint8_t bits = char_data[cy];
					int fb_y = 24 + row * 8 + cy;
					if (fb_y >= 240)
						continue;
					int fb_x_base = x * 4;
					uint8_t *fb_ptr = fb + fb_y * fb_width + fb_x_base;
					for (int px = 0; px < 4 && fb_x_base + px < fb_width; px++) {
						int bit0 = px * 2;
						bool pixel = ((bits >> bit0) & 1) | ((bits >> (bit0 + 1)) & 1);
						fb_ptr[px] = pixel ? 0 : 15;
					}
				}
			}
		}
	}
}

// Render bottom 4 text lines for mixed mode (lines 160-191 in Apple II coords) - OPTIMIZED
static void __attribute__((hot))
mii_video_render_text40_mixed_rp2350(
		mii_t *mii,
		uint8_t *fb,
		int fb_width)
{
	mii_bank_t *main_bank = &mii->bank[MII_BANK_MAIN];
	mii_bank_t *aux_bank = &mii->bank[MII_VIDEO_BANK];
	mii_video_t *video = &mii->video;
	const uint8_t *char_rom = video->rom ? video->rom->rom : NULL;
	
	if (!char_rom) return;
	uint32_t sw = mii->sw_state;
	bool page2 = SWW_GETSTATE(sw, SW80STORE) ? 0 : SWW_GETSTATE(sw, SWPAGE2);
	uint16_t base_addr = 0x400 + (0x400 * page2);
	bool col80 = SWW_GETSTATE(sw, SW80COL);
	bool altset = SWW_GETSTATE(sw, SWALTCHARSET);
	const uint8_t *rom_base = char_rom;
	if (video->rom && video->rom->len > (4 * 1024) && video->rom_bank)
		rom_base += (4 * 1024);
	
    pin_ram_pages_for(main_bank->ua.vram_desc, base_addr, 0x400);
    if (col80)
        pin_ram_pages_for(aux_bank->ua.vram_desc, base_addr, 0x400);
    else
        pin_ram_pages_for(aux_bank->ua.vram_desc, base_addr, 0); // unpin AUX if unused
		
    uint8_t main_row[40];
    uint8_t aux_row[40];

	int flash = (video->frame_count & 0x10) ? -0x40 : 0x40;
	
	// In mixed mode, only render the bottom 4 text rows (rows 20-23)
	// These correspond to Apple II lines 160-191
	for (int row = 20; row < 24; row++) {
		uint16_t line_addr = base_addr + (row & 7) * 0x80 + (row / 8) * 0x28;
		mii_bank_read(main_bank, line_addr, main_row, 40);
		if (!col80) {
			for (int x = 0; x < 40; x++) {
				uint8_t c = main_row[x];

				if (!altset && c >= 0x40 && c <= 0x7F)
					c = (int)c + flash;

				const uint8_t *char_data = rom_base + (c << 3);
				int fb_x_base = x * 8;
				uint8_t *fb_row_base = fb + 24 * fb_width + row * 8 * fb_width + fb_x_base;

				for (int cy = 0; cy < 8; cy++) {
					uint8_t bits = char_data[cy];
					uint8_t *fb_ptr = fb_row_base + cy * fb_width;
					fb_ptr[0] = (bits & 0x01) ? 0 : 15;
					fb_ptr[1] = (bits & 0x02) ? 0 : 15;
					fb_ptr[2] = (bits & 0x04) ? 0 : 15;
					fb_ptr[3] = (bits & 0x08) ? 0 : 15;
					fb_ptr[4] = (bits & 0x10) ? 0 : 15;
					fb_ptr[5] = (bits & 0x20) ? 0 : 15;
					fb_ptr[6] = (bits & 0x40) ? 0 : 15;
					fb_ptr[7] = 0;
				}
			}
		} else {
			mii_bank_read(aux_bank, line_addr, aux_row, 40);
			for (int x = 0; x < 80; x++) {
				uint8_t c = (x & 1) ? main_row[x >> 1] : aux_row[x >> 1];

				if (!altset && c >= 0x40 && c <= 0x7F)
					c = (int)c + flash;

				const uint8_t *char_data = rom_base + (c << 3);
				for (int cy = 0; cy < 8; cy++) {
					uint8_t bits = char_data[cy];
					int fb_y = 24 + row * 8 + cy;
					if (fb_y >= 240) continue;
					int fb_x_base = x * 4;
					uint8_t *fb_ptr = fb + fb_y * fb_width + fb_x_base;
					for (int px = 0; px < 4 && fb_x_base + px < fb_width; px++) {
						int bit0 = px * 2;
						bool pixel = ((bits >> bit0) & 1) | ((bits >> (bit0 + 1)) & 1);
						fb_ptr[px] = pixel ? 0 : 15;
					}
				}
			}
		}
	}
}

// Render hi-res graphics to framebuffer - OPTIMIZED
static void __attribute__((hot))
mii_video_render_hires_rp2350(
		mii_t *mii,
		uint8_t *fb,
		int fb_width)
{
	mii_bank_t *main_bank = &mii->bank[MII_BANK_MAIN];
	mii_bank_t *aux_bank = &mii->bank[MII_VIDEO_BANK];
	mii_video_t *video = &mii->video;
	const uint8_t HW_BLACK = 0;
	const uint8_t HW_WHITE = 15;
	
	// Check PAGE2 switch to select which HGR page
	uint32_t sw = mii->sw_state;
	bool page2 = SWW_GETSTATE(sw, SW80STORE) ? 0 : SWW_GETSTATE(sw, SWPAGE2);
	uint16_t base_addr = page2 ? 0x4000 : 0x2000;
	
	// HGR is 280x192. Render 1:1 into a 320-wide buffer with 20px borders.
	// Use the same artifact-color decoding as the desktop renderer (_mii_line_render_hires).
	const int x_off = (320 - 280) / 2; // 20
	const bool mono = video->monochrome;

	pin_ram_pages_for(main_bank->ua.vram_desc, base_addr, 0x2000);
    pin_ram_pages_for(aux_bank->ua.vram_desc,  base_addr, 0);
	uint8_t line_buf[40];   // one HGR line = 40 bytes
	
	for (int line = 0; line < 192; line++) {
		// Apple II HGR line address calculation (same as original)
		// Use the same formula as _mii_line_to_video_addr
		uint16_t line_addr = base_addr + 
			((line & 0x07) << 10) +     // (line % 8) * 1024
			(((line >> 3) & 0x07) << 7) + // ((line / 8) % 8) * 128
			((line >> 6) * 40);          // (line / 64) * 40
		
		int fb_y = 24 + line;  // 24 pixel vertical offset to center
		if (fb_y >= 240) continue;
		
		mii_bank_read(main_bank, line_addr, line_buf, 40);		
		uint8_t *fb_row = line_buffer;
		// Clear the whole row to black so borders don't retain stale pixels.
		memset(fb_row, HW_BLACK, (size_t)fb_width);

		uint8_t b0 = 0;
		uint8_t b1 = line_buf[0];
		for (int col = 0; col < 40; col++) {
			uint8_t b2 = (col == 39) ? 0 : line_buf[col + 1];
			// last 2 pixels, current 7 pixels, next 2 pixels
			uint16_t run = ((b0 & 0x60) >> 5) |
						((b1 & 0x7f) << 2) |
						((b2 & 0x03) << 9);
			int odd = (col & 1) << 1;
			int offset = (b1 & 0x80) >> 5; // 0 or 4

			for (int i = 0; i < 7; i++) {
				uint8_t left = (run >> (1 + i)) & 1;
				uint8_t pixel = (run >> (2 + i)) & 1;
				uint8_t right = (run >> (3 + i)) & 1;
				int idx = 0; // black
				if (!mono) {
					if (pixel) {
						if (left || right) {
							idx = 9; // white
						} else {
							idx = offset + odd + (i & 1) + 1;
						}
					} else {
						if (left && right) {
							idx = offset + odd + 1 - (i & 1) + 1;
						}
					}
					uint8_t ci = (uint8_t)mii_base_clut.hires[idx];
					uint8_t hw = rp2350_ci_to_hw[ci & 0x0f];
					int x = col * 7 + i;
					fb_row[x_off + x] = hw;
				} else {
					int x = col * 7 + i;
					fb_row[x_off + x] = pixel ? HW_WHITE : HW_BLACK;
				}
			}
			b0 = b1;
			b1 = b2;
		}
		while (fb_y == lock_y) ; // unsure unlocked
		memcpy(fb + fb_y * fb_width, line_buffer, fb_width);
	}
}

static void __attribute__((hot))
mii_video_render_dhires_rp2350(
		mii_t *mii,
		uint8_t *fb,
		int fb_width)
{
	mii_bank_t *main_bank = &mii->bank[MII_BANK_MAIN];
	mii_bank_t *aux_bank = &mii->bank[MII_VIDEO_BANK];
	const uint32_t sw = mii->sw_state;
	const bool page2 = SWW_GETSTATE(sw, SW80STORE) ? 0 : SWW_GETSTATE(sw, SWPAGE2);
	uint16_t base_addr = 0x2000 + (0x2000 * page2);

    pin_ram_pages_for(main_bank->ua.vram_desc, base_addr, 0x2000);
    pin_ram_pages_for(aux_bank->ua.vram_desc,  base_addr, 0x2000);

    uint8_t main_row[40];
    uint8_t aux_row[40];

	// Apple II DHGR is 560x192. We render into 320x240 with 24px top margin.
	// Use nearest-neighbor horizontal resample: src_x = (x * 7) / 4.
	const bool color = (mii->video.an3_mode != 0) && !mii->video.monochrome;

	for (int line = 0; line < 192; line++) {
		uint16_t line_addr = _mii_line_to_video_addr(base_addr, (uint8_t)line);
		int fb_y = 24 + line;
		if (fb_y >= 240)
			continue;

        mii_bank_read(main_bank, line_addr, main_row, 40);
        mii_bank_read(aux_bank,  line_addr, aux_row,  40);

		uint8_t *fb_row = line_buffer;
		if (!color) {
			// Mono: combine MAIN/AUX 7-bit streams into 14-bit pixels (560 wide)
			// Cache column data to avoid repeated memory lookups
			int last_col = -1;
			uint32_t ext = 0;
			for (int x = 0; x < 320; x++) {
				int src = (x * 7) / 4; // 0..559
				int col = src / 14;    // 0..39
				if (col != last_col) {
					ext = (aux_row[col] & 0x7f) | ((main_row[col] & 0x7f) << 7);
					last_col = col;
				}
				int bi = src % 14;
				uint8_t pixel = (ext >> bi) & 1;
				fb_row[x] = pixel ? 15 : 0;
			}
		}
		else {
			// Color: build a bit buffer for 80 bytes (AUX/MAIN interleaved)
			uint8_t bits[71] = {0};
			for (int x = 0; x < 80; x++) {
				uint8_t b = (x & 1) ? main_row[x / 2] : aux_row[x / 2];
				for (int i = 0; i < 7; i++) {
					int out_index = 2 + (x * 7) + i;
					int out_byte = out_index / 8;
					int out_bit = 7 - (out_index % 8);
					int bit = (b >> i) & 1;
					bits[out_byte] |= bit << out_bit;
				}
			}

			for (int x = 0; x < 320; x++) {
				int i = (x * 7) / 4; // 0..559
				int d = 2 + i;
				uint8_t pixel =
					(_mii_get_1bits_rp2350(bits, i + 3) << (3 - ((d + 3) % 4))) +
					(_mii_get_1bits_rp2350(bits, i + 2) << (3 - ((d + 2) % 4))) +
					(_mii_get_1bits_rp2350(bits, i + 1) << (3 - ((d + 1) % 4))) +
					(_mii_get_1bits_rp2350(bits, i)     << (3 - (d % 4)));
				uint8_t ci = (uint8_t)mii_base_clut.dhires[pixel];
				fb_row[x] = rp2350_ci_to_hw[ci & 0x0f];
			}
		}
		while (fb_y == lock_y) ; // unsure unlocked
		memcpy(fb + fb_y * fb_width, line_buffer, fb_width);
	}
}

// Render lo-res graphics to framebuffer - OPTIMIZED
static void __attribute__((hot))
mii_video_render_lores_rp2350(
		mii_t *mii,
		uint8_t *fb,
		int fb_width)
{
	mii_bank_t *main_bank = &mii->bank[MII_BANK_MAIN];
	mii_bank_t *aux_bank = &mii->bank[MII_VIDEO_BANK];
	
	// Lo-res is 40x48 blocks
	// Screen memory is 40x24 bytes, each byte has:
	//   - low nibble (bits 0-3): top half of block (4 scanlines)  
	//   - high nibble (bits 4-7): bottom half of block (4 scanlines)
	// So 24 rows of bytes = 48 rows of LORES blocks
	// Each LORES block is 7 pixels wide x 4 scanlines tall on Apple II
	// We scale to 320x240: 8 pixels wide x 5 scanlines tall
	
	// Check PAGE2 switch - Page 1 at $400, Page 2 at $800
	bool page2 = !!(mii->sw_state & M_SWPAGE2);
	uint16_t base_addr = page2 ? 0x800 : 0x400;
	
    pin_ram_pages_for(main_bank->ua.vram_desc, base_addr, 0x400);
    pin_ram_pages_for(aux_bank->ua.vram_desc,  base_addr, 0);
	
    uint8_t main_row[40];

	for (int lores_row = 0; lores_row < 48; lores_row++) {
		// Convert LORES row (0-47) to memory row (0-23) 
		int mem_row = lores_row / 2;
		int is_bottom_half = lores_row & 1;
		
		// Apple II screen memory address calculation
		uint16_t line_addr = base_addr + (mem_row & 7) * 0x80 + (mem_row / 8) * 0x28;
		
        mii_bank_read(main_bank, line_addr, main_row, 40);
		// Each LORES row maps to 5 framebuffer rows (48 * 5 = 240)
		int fb_y_start = lores_row * 5;
		
		for (int col = 0; col < 40; col++) {
			uint8_t byte = main_row[col];  // Direct memory access
			uint8_t color = is_bottom_half ? ((byte >> 4) & 0x0F) : (byte & 0x0F);
			
			// Each LORES column maps to 8 framebuffer columns (40 * 8 = 320)
			int fb_x_start = col * 8;
			
			// Fill the 8x5 pixel block - use memset for speed
			for (int dy = 0; dy < 5 && (fb_y_start + dy) < 240; dy++) {
				uint8_t *fb_row = fb + (fb_y_start + dy) * fb_width + fb_x_start;
				// Use uint64_t write for 8 pixels at once (assumes alignment)
				memset(fb_row, color, 8);
			}
		}
	}
}

// Main render function for RP2350
void
mii_video_render(
		mii_t *mii)
{
	// This function updates the internal mii->video.pixels array
	// For now, we'll do nothing here since we render directly to HDMI buffer
}

// Forward declaration
int mii_disk2_get_motor_state(void);

// Draw a simple floppy disk activity indicator in the bottom border
static void
mii_video_draw_floppy_indicator(uint8_t *hdmi_buffer, int motor_state, uint32_t frame_count)
{
	if (motor_state == 0)
		return;  // No motor active, don't draw
	
	// Flash the icon (on/off every 8 frames, approx 130ms at 60Hz) to indicate activity
	if ((frame_count / 8) % 2 == 0) {
		return; 
	}
	
	// Draw in bottom-right corner of bottom border
	// Bottom border starts at row 216 (rows 216-239 = 24 rows)
	// Icon position: 10x10 pixels (scaled up a bit), right side
	int start_x = 300;  
	int start_y = 222;  
	
	// Improved floppy disk icon (10x10)
	// 0 = transparent, 1 = body, 2 = label/shutter
	static const uint16_t floppy_icon[10] = {
		0b0111111110, // .########.
		0b1001110001, // #..###...#
		0b1001110001, // #..###...#
		0b1001110001, // #..###...#
		0b1001110001, // #..###...#
		0b1000000001, // #........#
		0b1001111001, // #..####..#
		0b1001111001, // #..####..#
		0b1001111001, // #..####..#
		0b0111111110, // .########.
	};
	
	// Color: Green for drive 1, Red/Orange for drive 2
	uint8_t body_color = (motor_state == 1) ? 0x1C : 0xE0;  
	
	for (int y = 0; y < 10; y++) {
		uint16_t row = floppy_icon[y];
		for (int x = 0; x < 10; x++) {
			if (row & (1 << (9 - x))) {
				int offset = (start_y + y) * 320 + (start_x + x);
				// Solid color
				hdmi_buffer[offset] = body_color;
			}
		}
	}
}

// Scale Apple II video to HDMI framebuffer
void
mii_video_scale_to_hdmi(
		mii_video_t *video,
		uint8_t *hdmi_buffer)
{
	// Get parent mii structure
	mii_t *mii = (mii_t *)((char*)video - offsetof(mii_t, video));
	
	// Clear top and bottom borders (24 rows each) to black
	// Top border: rows 0-23
	memset(hdmi_buffer, 0, 320 * 24);
	// Bottom border: rows 216-239
	memset(hdmi_buffer + 320 * 216, 0, 320 * 24);
	
	uint32_t sw = mii->sw_state;
	bool text_mode = !!(sw & M_SWTEXT);
	bool mixed = !!(sw & M_SWMIXED);
	bool hires = !!(sw & M_SWHIRES);
	bool page2 = SWW_GETSTATE(sw, SW80STORE) ? 0 : SWW_GETSTATE(sw, SWPAGE2);
	bool col80 = !!(sw & M_SW80COL);
	bool dhires = !!(sw & M_SWDHIRES);
	uint8_t an3_mode = video->an3_mode;
	
	if (text_mode) {
		// Pure text mode
		mii_video_render_text40_rp2350(mii, hdmi_buffer, 320);
	} else if (hires) {
		// Hi-res graphics mode
		// DHGR requires: HIRES=1, TEXT=0, DHIRES=1, and either 80COL=1 or an3_mode indicates DHGR
		// an3_mode: 0=40col text/lores, 1=DHGR color, 2=DHGR mono, 3=80col text
		bool is_dhgr = dhires && (col80 || (an3_mode >= 1 && an3_mode <= 2));
		if (is_dhgr) {
			mii_video_render_dhires_rp2350(mii, hdmi_buffer, 320);
		} else {
			mii_video_render_hires_rp2350(mii, hdmi_buffer, 320);
		}
		if (mixed) {
			// Mixed mode: render bottom 4 text lines (lines 160-191)
			// This overlays text on top of the HGR screen
			mii_video_render_text40_mixed_rp2350(mii, hdmi_buffer, 320);
		}
	} else {
		// Lo-res graphics mode
		mii_video_render_lores_rp2350(mii, hdmi_buffer, 320);
	}
	
	// Draw floppy activity indicator in bottom border
	int motor_state = mii_disk2_get_motor_state();
	if (motor_state > 0) {
		mii_video_draw_floppy_indicator(hdmi_buffer, motor_state, mii->video.frame_count);
	}
}

#endif // MII_RP2350

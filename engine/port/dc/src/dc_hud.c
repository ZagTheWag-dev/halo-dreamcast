/*
DC_HUD.C

The HUD on the PowerVR (n64.h's HUD API, port/n64/src/n64_hud.c on the N64):
the game's own HUD code (interface/hud*.c, motion_sensor.c, game_engine.c,
through port/n64/game/n64_hud_game.c) hands its quads, glyphs and blips here
in the game's 640x480 units, which are the Dreamcast's pixels. Each is
staged in RAM as translucent-list primitives (a header when it changes,
four vertices a quad) and sent after the world's translucent polygons
(dc_hud_submit, from n64_render_frame_end), at a depth past everything
(1/w from HUD_Z_BASE up, a step a quad: the autosorted list keeps the
order they were drawn in).

Halo's own blending (premultiplied, n64.h): colour * texture + framebuffer *
(1 - alpha * texture alpha), PVR_BLEND_ONE / PVR_BLEND_INVSRCALPHA with
MODULATEALPHA. The sprites are premultiplied (tools/n64/hud_dc.py) and take
the game's colour as it is; Halo's text blends straight, so the glyphs'
colours, the blips' and the pause screens' fills are premultiplied here.

rom:/hud/<map>.dch, all into video memory with the map: the sprites (8-bit
palettised, twiddled: the coverage ramp in palette bank 0, alpha and
intensity in four bits each in bank 1; ARGB4444 for the rare coloured
one), the meters (kept in RAM at the N64's half size and coloured on the
CPU, as n64_hud.c's meter_surface, into METER_SLOTS small textures, loaded
between the frame's lists), and every font's glyphs in one 4-bit atlas
(16-entry bank 32). The palettes are ARGB8888 (one format for every
palette on the PowerVR: whoever comes next with palettes keeps it).

Text: Halo's (n64_hud_glyph: draw_string.c lays it out, every glyph where
Halo puts it, its drop shadow as rasterizer_text.c draws it); and this
file's own (n64_hud_text and the pause screens) in the HUD's font, the HUD
globals' "fullscreen font" (large_ui on Blood Gulch).
*/

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dc/pvr.h>

#include "dc_render.h"

#define HUD_SPRITES 512
/* the frame's 32-byte entries: a quad is 4, 5 with its header */
#define HUD_ENTRIES 6144
/* past the first-person models' 1/w (at most about 87,000: dc_render.c's
1024 times the first-person near plane's 85), a step a quad (the PowerVR
sorts the translucent list by 1/w: one quad over the last) */
#define HUD_Z_BASE 131072.0f
#define HUD_Z_STEP 16.0f
#define METER_SLOTS 12
#define METER_W 128
#define METER_H 32
#define BLIP_SIZE 16
#define FIRST_CHARACTER 32
#define CHARACTER_COUNT 95
/* the palettes: 8-bit banks 0 and 1 (entries 0-511), 4-bit bank 32
(entries 512-527) */
#define PALETTE_COVERAGE 0
#define PALETTE_GREY 1
#define PALETTE_FONT 32

/* hud/<map>.dch sprite kinds (tools/n64/hud_dc.py) */
#define DCH_COVERAGE 0
#define DCH_GREY 1
#define DCH_COLOUR 2
#define DCH_METER 3
#define DCH_OPAQUE 4
#define DCH_VQ_ARGB4444 5
#define DCH_VQ_RGB565 6
/* a vertical ramp: its rows' colours (ARGB8888, premultiplied), drawn as
Gouraud strips (hud_ramp) */
#define DCH_RAMP 7
#define DCH_NO_FONT 0xFFFF
/* the bands a ramp is drawn in */
#define RAMP_BANDS 32
/* a sprite's flags: loaded the first time it is drawn (the shell's) */
#define DCH_STREAMED 1
/* a VQ texture's codebook: 256 2x2 blocks of 16-bit texels */
#define VQ_CODEBOOK 2048
/* the video memory streamed sprites leave free (the menu's own) */
#define STREAM_HEADROOM (96 * 1024)
/* the fonts whose characters are mapped (n64_font_load: the menu's) */
#define FONTS_MAPPED 16

#define HUD_MIN(a, b) ((a) < (b) ? (a) : (b))
#define HUD_MAX(a, b) ((a) > (b) ? (a) : (b))
/* x / 255 for x up to 65535, without the SH-4's software division */
#define DIV255(x) (((x) + 1 + ((x) >> 8)) >> 8)
#define PACK4444(a, r, g, b) ((uint16_t)((((a) >> 4) << 12) | (((r) >> 4) << 8) | (((g) >> 4) << 4) | ((b) >> 4)))

/* ---------- hud/<map>.dch, little-endian */

struct dch_header
{
	char magic[4];
	uint32_t version;
	uint16_t sprite_count, font_count, glyph_count, hud_font;
	uint32_t sprites_offset, fonts_offset, glyphs_offset, atlas_offset;
	uint16_t atlas_width, atlas_height;
	uint32_t reserved[3];
};

struct dch_sprite
{
	uint16_t width, height, texture_width, texture_height;
	uint8_t kind, scale;
	uint16_t flags;
	uint32_t offset, size;
	uint32_t reserved[3];
};

struct dch_font
{
	char name[32];
	int16_t ascent, descent, leading_height, leading_width;
	uint16_t first_glyph, glyph_count;
	uint32_t reserved;
};

struct dch_glyph
{
	uint16_t code, x, y;
	uint8_t width, height;
	int8_t origin_x, origin_y;
	uint8_t advance, pad;
};

typedef union
{
	pvr_poly_hdr_t header;
	pvr_vertex_t vertex;
} hud_entry;

/* a font of the file (n64.h's struct n64_font: n64_font_load), its
characters 32-126 by glyph number */
struct n64_font
{
	char name[32];
	int ascent, descent, cap;
	int16_t map[95];
};

typedef char dch_sizes_assert[sizeof(struct dch_header) == 48 && sizeof(struct dch_sprite) == 32 &&
	sizeof(struct dch_font) == 48 && sizeof(struct dch_glyph) == 12 && sizeof(hud_entry) == 32 ? 1 : -1];

struct hud_sprite
{
	pvr_poly_hdr_t header[2]; /* bilinear, point sampled */
	pvr_ptr_t texture;
	uint8_t *meter;           /* DCH_METER: order, shape */
	int width, height, scale;
	float u_scale, v_scale;   /* a stored texel's texture coordinates */
	/* DCH_STREAMED: where it is in the file, its texture's format and
	size, the frame it was last drawn in (hud.frame) */
	int streamed, format, texture_width, texture_height;
	uint32_t offset, size;
	unsigned long last_drawn;
	/* DCH_RAMP: its rows' colours */
	uint32_t *ramp;
};

struct meter_slot
{
	pvr_poly_hdr_t header;
	pvr_ptr_t texture;
	int sprite, s0, t0, width, height;
	unsigned long minimum_color, maximum_color, flash_color, background_color, tint_color;
	unsigned long last_used; /* hud.frame */
	int dirty;               /* pixels made, not loaded yet */
};

static struct hud_sprite hud_sprites[HUD_SPRITES] __attribute__((aligned(32)));
/* Halo's menus are drawing (dc_menu.c): n64_hud_game.c draws their
bitmaps as the widgets blend them */
int dc_hud_widgets;
static struct meter_slot meters[METER_SLOTS] __attribute__((aligned(32)));
static hud_entry hud_stage[HUD_ENTRIES] __attribute__((aligned(32)));
static uint16_t meter_pixels[METER_SLOTS][METER_W * METER_H] __attribute__((aligned(32)));

static struct
{
	pvr_poly_hdr_t fill_header, blip_header, font_header, ramp_header;
	int reserved;
	int sprite_count, meter_count, missing;
	unsigned long vram_bytes;
	/* the glyphs, every font's; the HUD's font's characters 32-126 */
	struct dch_glyph *glyphs;
	int glyph_count;
	float atlas_u, atlas_v;
	pvr_ptr_t atlas, blip_texture;
	int font, font_ascent, font_descent, font_cap;
	int16_t font_map[CHARACTER_COUNT];
	/* every font's characters (the menu's text) */
	struct n64_font fonts[FONTS_MAPPED];
	int font_count;
	/* the frame's primitives */
	int count, quads_this_frame, drawing;
	const pvr_poly_hdr_t *last_header;
	int x0, y0, x1, y1;
	int view_count;
	unsigned long frame; /* from 2: a slot never used is free */
	int inset;
	unsigned long quads, glyphs_drawn, overflows, meter_builds, meter_misses;
	/* the streamed sprites' file (open while the HUD is set), the buffer
	they are read into, what is in video memory */
	long stream_file;
	uint32_t *stream_bounce;
	unsigned long stream_bounce_size, streamed_bytes, stream_loads, stream_evictions, stream_failures;
} hud = {.frame = 2, .inset = -1, .font = -1, .stream_file = -1};

static inline int floor_int(float x) { int i = (int)x; return (float)i > x ? i - 1 : i; }
static inline int ceil_int(float x) { int i = (int)x; return (float)i < x ? i + 1 : i; }

static uint32_t premultiply(unsigned long argb)
{
	unsigned a = (unsigned)(argb >> 24);

	return (uint32_t)a << 24 | (uint32_t)DIV255(((argb >> 16) & 255) * a) << 16 |
		(uint32_t)DIV255(((argb >> 8) & 255) * a) << 8 | (uint32_t)DIV255((argb & 255) * a);
}

/* a translucent-list header: no fog or culling, Halo's blend; the depth
test passes (the HUD's 1/w is past the world's) and writes nothing */
static void hud_compile(pvr_poly_hdr_t *header, pvr_ptr_t texture, int format, int width, int height, int filter)
{
	pvr_poly_cxt_t c;

	if (texture)
	{
		pvr_poly_cxt_txr(&c, PVR_LIST_TR_POLY, format, width, height, texture, filter);
		c.txr.env = PVR_TXRENV_MODULATEALPHA;
		c.txr.uv_clamp = PVR_UVCLAMP_UV;
		c.txr.mipmap = PVR_MIPMAP_DISABLE;
	}
	else
		pvr_poly_cxt_col(&c, PVR_LIST_TR_POLY);
	c.gen.shading = PVR_SHADE_FLAT;
	c.gen.fog_type = PVR_FOG_DISABLE;
	c.gen.culling = PVR_CULLING_NONE;
	c.depth.comparison = PVR_DEPTHCMP_GREATER;
	c.depth.write = PVR_DEPTHWRITE_DISABLE;
	c.blend.src = PVR_BLEND_ONE;
	c.blend.dst = PVR_BLEND_INVSRCALPHA;
	pvr_poly_compile(header, &c);
}

/* ---------- staging */

/* room for a quad and, when it changes, its header: its four vertices */
static pvr_vertex_t *hud_quad_room(const pvr_poly_hdr_t *header)
{
	int need = 4 + (header != hud.last_header);
	hud_entry *e;

	if (!hud.drawing)
		return NULL;
	if (hud.count + need > HUD_ENTRIES)
	{
		hud.overflows++;
		return NULL;
	}
	e = &hud_stage[hud.count];
	if (header != hud.last_header)
	{
		e->header = *header;
		e++;
		hud.last_header = header;
	}
	hud.count += need;
	hud.quads++;
	return &e->vertex;
}

/* corners 0 top left, 1 top right, 2 bottom right, 3 bottom left, sent as
the strip 0, 1, 3, 2 */
static void hud_put(pvr_vertex_t *d, const float x[4], const float y[4], const float u[4], const float v[4],
	uint32_t argb)
{
	static const int order[4] = {0, 1, 3, 2};
	float z = HUD_Z_BASE + HUD_Z_STEP * (float)hud.quads_this_frame++;
	int i;

	for (i = 0; i < 4; i++)
	{
		int k = order[i];

		d[i].flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
		d[i].x = x[k];
		d[i].y = y[k];
		d[i].z = z;
		d[i].u = u[k];
		d[i].v = v[k];
		d[i].argb = argb;
		d[i].oargb = 0;
	}
}

static void hud_rect(const pvr_poly_hdr_t *header, float x0, float y0, float x1, float y1,
	float u0, float v0, float u1, float v1, uint32_t argb)
{
	float x[4] = {x0, x1, x1, x0}, y[4] = {y0, y0, y1, y1};
	float u[4] = {u0, u1, u1, u0}, v[4] = {v0, v0, v1, v1};
	pvr_vertex_t *d = hud_quad_room(header);

	if (d)
		hud_put(d, x, y, u, v, argb);
}

/* a box in a straight ARGB colour */
static void hud_fill(float x0, float y0, float x1, float y1, unsigned long argb)
{
	hud_rect(&hud.fill_header, x0, y0, x1, y1, 0.f, 0.f, 0.f, 0.f, premultiply(argb));
}

/* ---------- what is kept for good: the palettes, the fill header, the
blip, the meters' slots (96 KB of video memory). Made as the PowerVR comes
up (dc_render.c n64_render_initialize), before any level, model or HUD
texture: at the bottom of video memory, so the maps' textures, given back
and taken again round them, leave no hole the slots would pin (it was the
first map's load: after the menu's level, in the middle of the free
space) */

static void hud_reserve(void)
{
	static uint16_t dot[BLIP_SIZE * BLIP_SIZE] __attribute__((aligned(32)));
	int i, x, y;

	if (hud.reserved || !dc_pvr_up)
		return;
	hud.reserved = 1;
	/* the sprites' and the glyphs' palettes, premultiplied: the coverage
	ramp (v, v, v, v), alpha and intensity in four bits each, and the
	glyphs' sixteen steps of white */
	pvr_set_pal_format(PVR_PAL_ARGB8888);
	for (i = 0; i < 256; i++)
	{
		uint32_t a = (uint32_t)(i >> 4) * 17, k = (uint32_t)(i & 15) * 17;

		pvr_set_pal_entry(PALETTE_COVERAGE * 256 + i, (uint32_t)i * 0x01010101u);
		pvr_set_pal_entry(PALETTE_GREY * 256 + i, a << 24 | k << 16 | k << 8 | k);
	}
	for (i = 0; i < 16; i++)
		pvr_set_pal_entry(PALETTE_FONT * 16 + i, (uint32_t)i * 17 * 0x01010101u);
	hud_compile(&hud.fill_header, NULL, 0, 0, 0, PVR_FILTER_NEAREST);
	/* (the ramps': their colours shaded down the strip) */
	{
		pvr_poly_cxt_t c;

		pvr_poly_cxt_col(&c, PVR_LIST_TR_POLY);
		c.gen.shading = PVR_SHADE_GOURAUD;
		c.gen.fog_type = PVR_FOG_DISABLE;
		c.gen.culling = PVR_CULLING_NONE;
		c.depth.comparison = PVR_DEPTHCMP_GREATER;
		c.depth.write = PVR_DEPTHWRITE_DISABLE;
		c.blend.src = PVR_BLEND_ONE;
		c.blend.dst = PVR_BLEND_INVSRCALPHA;
		pvr_poly_compile(&hud.ramp_header, &c);
	}
	/* a soft round dot (n64_hud.c make_blip), premultiplied white */
	hud.blip_texture = pvr_mem_malloc(sizeof(dot));
	if (hud.blip_texture)
	{
		for (y = 0; y < BLIP_SIZE; y++)
		{
			for (x = 0; x < BLIP_SIZE; x++)
			{
				float dx = (x + 0.5f) / (BLIP_SIZE / 2) - 1.f, dy = (y + 0.5f) / (BLIP_SIZE / 2) - 1.f;
				float d = 1.f - sqrtf(dx * dx + dy * dy);
				unsigned k = d <= 0.f ? 0 : d >= 0.5f ? 255 : (unsigned)(d * 510.f);

				dot[y * BLIP_SIZE + x] = PACK4444(k, k, k, k);
			}
		}
		pvr_txr_load(dot, hud.blip_texture, sizeof(dot));
		hud_compile(&hud.blip_header, hud.blip_texture, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED,
			BLIP_SIZE, BLIP_SIZE, PVR_FILTER_BILINEAR);
	}
	for (i = 0; i < METER_SLOTS; i++)
	{
		meters[i].sprite = -1;
		meters[i].texture = pvr_mem_malloc(METER_W * METER_H * 2);
		if (meters[i].texture)
			hud_compile(&meters[i].header, meters[i].texture, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED,
				METER_W, METER_H, PVR_FILTER_BILINEAR);
	}
}

void dc_hud_reserve(void)
{
	hud_reserve();
}

/* ---------- the map's sprites and fonts */

void n64_hud_forget_recordings(void)
{
}

/* what the map's HUD made given back (the slots, the blip and the
palettes stay): the PowerVR done with them first */
void n64_hud_release(void)
{
	int i;

	if (hud.sprite_count || hud.atlas)
		pvr_wait_render_done();
	for (i = 0; i < hud.sprite_count; i++)
	{
		if (hud_sprites[i].texture)
			pvr_mem_free(hud_sprites[i].texture);
		free(hud_sprites[i].meter);
		free(hud_sprites[i].ramp);
	}
	memset(hud_sprites, 0, sizeof(hud_sprites));
	hud.sprite_count = hud.meter_count = hud.missing = 0;
	hud.vram_bytes = 0;
	if (hud.stream_file >= 0)
		n64_file_close(hud.stream_file);
	hud.stream_file = -1;
	free(hud.stream_bounce);
	hud.stream_bounce = NULL;
	hud.stream_bounce_size = hud.streamed_bytes = 0;
	if (hud.atlas)
		pvr_mem_free(hud.atlas);
	hud.atlas = NULL;
	free(hud.glyphs);
	hud.glyphs = NULL;
	hud.glyph_count = 0;
	hud.font = -1;
	hud.font_count = 0;
	for (i = 0; i < METER_SLOTS; i++)
	{
		meters[i].sprite = -1;
		meters[i].dirty = 0;
	}
}

static int read_at(long file, unsigned long offset, void *buffer, unsigned long size)
{
	return n64_file_seek(file, (long)offset) >= 0 && n64_file_read(file, buffer, (long)size) == (long)size;
}

/* a sprite kind's texture format (hud_dc.py KIND_*) */
static int sprite_format(int kind)
{
	switch (kind)
	{
	case DCH_COLOUR: return PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED;
	case DCH_OPAQUE: return PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED;
	case DCH_VQ_ARGB4444: return PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE;
	case DCH_VQ_RGB565: return PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE;
	}
	return PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_TWIDDLED |
		PVR_TXRFMT_8BPP_PAL(kind == DCH_GREY ? PALETTE_GREY : PALETTE_COVERAGE);
}

/* the bytes of a sprite's texture */
static uint32_t sprite_bytes(const struct dch_sprite *e)
{
	uint32_t texels = (uint32_t)e->texture_width * e->texture_height;

	if (e->kind == DCH_VQ_ARGB4444 || e->kind == DCH_VQ_RGB565)
		return VQ_CODEBOOK + texels / 4;
	return texels * (e->kind == DCH_COLOUR || e->kind == DCH_OPAQUE ? 2 : 1);
}

/* the glyphs, the atlas, every font's characters (the first FONTS_MAPPED
fonts': n64_font_load) and the HUD's font's */
static void hud_load_fonts(long file, const struct dch_header *header, void *bounce)
{
	unsigned long table = (unsigned long)header->glyph_count * sizeof(struct dch_glyph);
	unsigned long bytes = (unsigned long)header->atlas_width * header->atlas_height / 2;
	struct dch_font font;
	int i, f;

	for (i = 0; i < CHARACTER_COUNT; i++)
		hud.font_map[i] = -1;
	hud.font_count = 0;
	if (!header->glyph_count || !bytes)
		return;
	hud.glyphs = malloc(table);
	if (!hud.glyphs || !read_at(file, header->glyphs_offset, hud.glyphs, table) ||
		!read_at(file, header->atlas_offset, bounce, bytes) || !(hud.atlas = pvr_mem_malloc(bytes)))
	{
		free(hud.glyphs);
		hud.glyphs = NULL;
		hud.missing++;
		return;
	}
	pvr_txr_load(bounce, hud.atlas, bytes);
	hud.vram_bytes += bytes;
	hud.glyph_count = header->glyph_count;
	hud.atlas_u = 1.f / header->atlas_width;
	hud.atlas_v = 1.f / header->atlas_height;
	hud_compile(&hud.font_header, hud.atlas, PVR_TXRFMT_PAL4BPP | PVR_TXRFMT_4BPP_PAL(PALETTE_FONT) |
		PVR_TXRFMT_TWIDDLED, header->atlas_width, header->atlas_height, PVR_FILTER_NEAREST);
	for (f = 0; f < header->font_count && f < FONTS_MAPPED; f++)
	{
		struct n64_font *mapped = &hud.fonts[f];

		if (!read_at(file, header->fonts_offset + f * sizeof(font), &font, sizeof(font)))
			break;
		memcpy(mapped->name, font.name, sizeof(mapped->name));
		mapped->name[sizeof(mapped->name) - 1] = 0;
		for (i = 0; i < CHARACTER_COUNT; i++)
			mapped->map[i] = -1;
		for (i = font.first_glyph; i < font.first_glyph + font.glyph_count && i < hud.glyph_count; i++)
		{
			int code = hud.glyphs[i].code;

			if (code >= FIRST_CHARACTER && code < FIRST_CHARACTER + CHARACTER_COUNT && mapped->map[code - FIRST_CHARACTER] < 0)
				mapped->map[code - FIRST_CHARACTER] = (int16_t)i;
		}
		mapped->ascent = font.ascent;
		mapped->descent = font.descent;
		/* (how far a capital rises: H's, n64_font.c n64_font_cap_height) */
		i = mapped->map['H' - FIRST_CHARACTER];
		mapped->cap = i >= 0 && hud.glyphs[i].origin_y > 0 ? hud.glyphs[i].origin_y : font.ascent;
		hud.font_count = f + 1;
	}
	if (header->hud_font == DCH_NO_FONT || header->hud_font >= hud.font_count)
		return;
	memcpy(hud.font_map, hud.fonts[header->hud_font].map, sizeof(hud.font_map));
	hud.font = header->hud_font;
	hud.font_ascent = hud.fonts[hud.font].ascent;
	hud.font_descent = hud.fonts[hud.font].descent;
	hud.font_cap = hud.fonts[hud.font].cap;
}

void n64_hud_set_map(const char *map_name)
{
	char path[64], line[160];
	struct dch_header header;
	struct dch_sprite *table = NULL;
	void *bounce = NULL;
	unsigned long biggest = 32;
	long file;
	int i;

	n64_hud_release();
	hud_reserve();
	if (!dc_pvr_up)
		return;
	snprintf(path, sizeof(path), "rom:/hud/%s.dch", map_name);
	file = n64_file_open(path);
	if (file < 0)
	{
		snprintf(line, sizeof(line), "dch: no %s: no HUD", path);
		n64_log(line);
		return;
	}
	if (n64_file_read(file, &header, sizeof(header)) != (long)sizeof(header) || memcmp(header.magic, "DCH1", 4) ||
		header.version != 1 || header.sprite_count > HUD_SPRITES)
	{
		n64_log("dch: the HUD file is not this build's");
		goto done;
	}
	table = malloc(header.sprite_count * sizeof(*table) + 1);
	if (!table || !read_at(file, header.sprites_offset, table, header.sprite_count * sizeof(*table)))
		goto done;
	for (i = 0; i < (int)header.sprite_count; i++)
	{
		if (!(table[i].flags & DCH_STREAMED))
			biggest = HUD_MAX(biggest, table[i].size);
	}
	biggest = HUD_MAX(biggest, (unsigned long)header.atlas_width * header.atlas_height / 2);
	bounce = memalign(32, biggest);
	if (!bounce)
		goto done;
	hud.sprite_count = (int)header.sprite_count;
	for (i = 0; i < hud.sprite_count; i++)
	{
		const struct dch_sprite *e = &table[i];
		struct hud_sprite *s = &hud_sprites[i];
		int format;

		s->width = e->width;
		s->height = e->height;
		s->scale = e->scale ? e->scale : 1;
		if (e->kind == DCH_METER)
		{
			if (e->size == (uint32_t)e->width * e->height * 2 && (s->meter = malloc(e->size)) &&
				!read_at(file, e->offset, s->meter, e->size))
			{
				free(s->meter);
				s->meter = NULL;
			}
			hud.meter_count += s->meter != NULL;
			hud.missing += s->meter == NULL;
			continue;
		}
		if (e->kind == DCH_RAMP)
		{
			if (e->size == (uint32_t)e->height * 4 && e->height && (s->ramp = malloc(e->size)) &&
				!read_at(file, e->offset, s->ramp, e->size))
			{
				free(s->ramp);
				s->ramp = NULL;
			}
			hud.missing += s->ramp == NULL;
			continue;
		}
		format = sprite_format(e->kind);
		if (e->kind > DCH_VQ_RGB565 || e->size != sprite_bytes(e))
		{
			hud.missing++;
			continue;
		}
		s->u_scale = 1.f / e->texture_width;
		s->v_scale = 1.f / e->texture_height;
		/* (the shell's: when first drawn, hud_stream) */
		if (e->flags & DCH_STREAMED)
		{
			s->streamed = 1;
			s->format = format;
			s->texture_width = e->texture_width;
			s->texture_height = e->texture_height;
			s->offset = e->offset;
			s->size = e->size;
			continue;
		}
		if (!read_at(file, e->offset, bounce, e->size) || !(s->texture = pvr_mem_malloc(e->size)))
		{
			hud.missing++;
			continue;
		}
		pvr_txr_load(bounce, s->texture, e->size);
		hud.vram_bytes += e->size;
		hud_compile(&s->header[0], s->texture, format, e->texture_width, e->texture_height, PVR_FILTER_BILINEAR);
		hud_compile(&s->header[1], s->texture, format, e->texture_width, e->texture_height, PVR_FILTER_NEAREST);
	}
	hud_load_fonts(file, &header, bounce);
	/* (the file stays open for the streamed sprites) */
	for (i = 0; i < hud.sprite_count && !hud_sprites[i].streamed; i++)
		;
	if (i < hud.sprite_count)
	{
		hud.stream_file = file;
		file = -1;
	}
done:
	if (file >= 0)
		n64_file_close(file);
	free(table);
	free(bounce);
	snprintf(line, sizeof(line), "dch: %s, %d sprites (%d meters, %d missing), %d glyphs, %lu KB of video memory, "
		"%lu KB left", map_name, hud.sprite_count, hud.meter_count, hud.missing, hud.glyph_count, hud.vram_bytes / 1024,
		(unsigned long)pvr_mem_available() / 1024);
	n64_log(line);
}

int n64_hud_ensure_sprite(int number)
{
	return number >= 0 && number < hud.sprite_count && (hud_sprites[number].texture || hud_sprites[number].meter ||
		hud_sprites[number].streamed || hud_sprites[number].ramp);
}

/* ---------- ramps: an upright quad of a ramp's rows t0 to t1 (stored
texels), RAMP_BANDS bands down it, each row's colour times the quad's (both
premultiplied), shaded between (Gouraud: 8 bits a channel, where a 16-bit
texture banded) */
static void hud_ramp(const struct hud_sprite *sprite, const struct n64_hud_quad *quad, float t0, float t1)
{
	int need = 2 * (RAMP_BANDS + 1) + (&hud.ramp_header != hud.last_header), k, last = sprite->height - 1;
	float x0 = quad->x[0], x1 = quad->x[1], y0 = quad->y[0], y1 = quad->y[2];
	float z = HUD_Z_BASE + HUD_Z_STEP * (float)hud.quads_this_frame++;
	uint32_t qa = (uint32_t)(quad->color >> 24), qr = (uint32_t)(quad->color >> 16) & 255,
		qg = (uint32_t)(quad->color >> 8) & 255, qb = (uint32_t)quad->color & 255;
	hud_entry *e;

	if (!hud.drawing || hud.count + need > HUD_ENTRIES)
	{
		hud.overflows++;
		return;
	}
	e = &hud_stage[hud.count];
	if (&hud.ramp_header != hud.last_header)
	{
		e->header = hud.ramp_header;
		e++;
		hud.last_header = &hud.ramp_header;
	}
	hud.count += need;
	hud.quads++;
	for (k = 0; k <= RAMP_BANDS; k++)
	{
		float f = (float)k / RAMP_BANDS, row = t0 + (t1 - t0) * f, y = y0 + (y1 - y0) * f, w;
		int r0 = HUD_MAX(0, HUD_MIN(last, floor_int(row))), r1 = HUD_MIN(last, r0 + 1);
		uint32_t a = sprite->ramp[r0], b = sprite->ramp[r1], argb = 0;
		int shift, side;

		w = HUD_MAX(0.f, HUD_MIN(1.f, row - (float)r0));
		for (shift = 0; shift < 32; shift += 8)
		{
			float c = ((a >> shift) & 255) * (1.f - w) + ((b >> shift) & 255) * w;
			uint32_t q = shift == 24 ? qa : shift == 16 ? qr : shift == 8 ? qg : qb;

			argb |= (uint32_t)DIV255((uint32_t)(c + 0.5f) * q) << shift;
		}
		for (side = 0; side < 2; side++)
		{
			pvr_vertex_t *v = &e->vertex;

			v->flags = k == RAMP_BANDS && side ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
			v->x = side ? x1 : x0;
			v->y = y;
			v->z = z;
			v->u = v->v = 0.f;
			v->argb = argb;
			v->oargb = 0;
			e++;
		}
	}
}

/* ---------- streamed sprites (the shell's pictures, ~4 MB: more than the
video memory the menu has): read from the file and put in video memory the
first time they are drawn, the least lately drawn given back when there is
no room (none the PowerVR may still be drawing from: this frame's or the
last's). Copied with the CPU's own writes, not pvr_txr_load's store
queues: a frame's opaque list may be open (its direct rendering has them) */

static void vram_copy(pvr_ptr_t destination, const uint32_t *source, uint32_t size)
{
	volatile uint32_t *out = (volatile uint32_t *)destination;
	uint32_t i;

	for (i = 0; i < (size + 3) / 4; i++)
		out[i] = source[i];
}

/* the least lately drawn streamed sprite given back; 0 if none can be */
static int stream_evict(void)
{
	struct hud_sprite *pick = NULL;
	int i;

	for (i = 0; i < hud.sprite_count; i++)
	{
		struct hud_sprite *s = &hud_sprites[i];

		if (s->streamed && s->texture && s->last_drawn + 2 <= hud.frame && (!pick || s->last_drawn < pick->last_drawn))
			pick = s;
	}
	if (!pick)
		return 0;
	pvr_mem_free(pick->texture);
	pick->texture = NULL;
	hud.streamed_bytes -= pick->size;
	hud.stream_evictions++;
	return 1;
}

/* the sprite in video memory (loaded now if it is a streamed one that is
not); 0 if it cannot be */
static int hud_stream(struct hud_sprite *s)
{
	pvr_ptr_t texture = NULL;

	if (s->texture || !s->streamed)
	{
		s->last_drawn = hud.frame;
		return s->texture != NULL;
	}
	if (hud.stream_file < 0)
		return 0;
	if (hud.stream_bounce_size < s->size)
	{
		free(hud.stream_bounce);
		hud.stream_bounce = memalign(32, s->size);
		hud.stream_bounce_size = hud.stream_bounce ? s->size : 0;
		if (!hud.stream_bounce)
			return 0;
	}
	if (!read_at(hud.stream_file, s->offset, hud.stream_bounce, s->size))
	{
		hud.stream_failures++;
		return 0;
	}
	for (;;)
	{
		if (pvr_mem_available() >= s->size + STREAM_HEADROOM && (texture = pvr_mem_malloc(s->size)))
			break;
		if (!stream_evict())
		{
			hud.stream_failures++;
			return 0;
		}
	}
	vram_copy(texture, hud.stream_bounce, s->size);
	s->texture = texture;
	s->last_drawn = hud.frame;
	hud_compile(&s->header[0], texture, s->format, s->texture_width, s->texture_height, PVR_FILTER_BILINEAR);
	hud_compile(&s->header[1], texture, s->format, s->texture_width, s->texture_height, PVR_FILTER_NEAREST);
	hud.streamed_bytes += s->size;
	hud.stream_loads++;
	return 1;
}

/* ---------- views */

void n64_hud_set_view_count(int count)
{
	hud.view_count = count;
}

void n64_hud_set_inset(int pixels)
{
	hud.inset = pixels < 0 ? 0 : pixels;
}

/* (in the N64's 320x240 pixels: n64_render_game.c doubles it into the
game's units, which are the Dreamcast's pixels; testing setting
"debug_hud_inset=<pixels>") */
int n64_hud_inset(void)
{
	if (hud.inset < 0)
	{
		char text[16];

		hud.inset = n64_debug_text("debug_hud_inset", text, sizeof(text)) ? atoi(text) : 0;
		if (hud.inset < 0)
			hud.inset = 0;
	}
	return hud.inset;
}

/* every view's HUD every frame (the N64 records one and shows it again) */
int n64_hud_begin(int view, int x0, int y0, int x1, int y1)
{
	hud.drawing = dc_pvr_up && view >= 0;
	hud.x0 = x0;
	hud.y0 = y0;
	hud.x1 = x1;
	hud.y1 = y1;
	return hud.drawing;
}

void n64_hud_end(void)
{
	hud.drawing = 0;
}

/* ---------- meters: the part of the sprite a quad shows, as Halo's meter
shader colours it (n64.h): the texels whose fill order is under the flash
colour's alpha full, from the minimum colour to the maximum by their order
(plus the flash colour from the minimum colour's alpha up), the rest the
background; all scaled by the tint's intensity, premultiplied by the
shape, the framebuffer kept by the tint's alpha (full) or the background's
(empty). Made in RAM, loaded between the frame's lists (dc_hud_upload) */
static const pvr_poly_hdr_t *meter_texture(const struct n64_hud_quad *quad, const struct hud_sprite *sprite,
	int s0, int t0, int width, int height)
{
	struct meter_slot *pick = NULL;
	unsigned minimum_alpha = (unsigned)(quad->minimum_color >> 24), flash_alpha = (unsigned)(quad->flash_color >> 24);
	unsigned scale = (unsigned)(quad->tint_color & 0xFF); /* the tint's intensity (n64_hud.c) */
	unsigned full_alpha = 255 - (unsigned)(quad->tint_color >> 24);
	unsigned empty_alpha = 255 - (unsigned)(quad->background_color >> 24);
	unsigned er, eg, eb;
	uint16_t *pixels;
	int i, x, y;

	width = HUD_MIN(width, METER_W);
	height = HUD_MIN(height, METER_H);
	for (i = 0; i < METER_SLOTS; i++)
	{
		struct meter_slot *m = &meters[i];

		if (!m->texture)
			continue;
		if (m->sprite == quad->sprite && m->s0 == s0 && m->t0 == t0 && m->width == width && m->height == height &&
			m->minimum_color == quad->minimum_color && m->maximum_color == quad->maximum_color &&
			m->flash_color == quad->flash_color && m->background_color == quad->background_color &&
			m->tint_color == quad->tint_color)
		{
			m->last_used = hud.frame;
			return &m->header;
		}
		/* (not one the PowerVR may still be drawing: this frame's or the
		last's) */
		if (m->last_used + 2 <= hud.frame && (!pick || m->last_used < pick->last_used))
			pick = m;
	}
	if (!pick)
	{
		hud.meter_misses++;
		return NULL;
	}
	er = DIV255(((quad->background_color >> 16) & 255) * scale);
	eg = DIV255(((quad->background_color >> 8) & 255) * scale);
	eb = DIV255((quad->background_color & 255) * scale);
	pixels = meter_pixels[pick - meters];
	memset(pixels, 0, sizeof(meter_pixels[0]));
	for (y = 0; y < height; y++)
	{
		const uint8_t *in = sprite->meter + ((t0 + y) * sprite->width + s0) * 2;
		uint16_t *out = pixels + y * METER_W;

		for (x = 0; x < width; x++, in += 2)
		{
			unsigned order = in[0], shape = in[1], r, g, b, a;

			if (!shape)
				continue;
			if (order < flash_alpha)
			{
				r = DIV255(((quad->minimum_color >> 16) & 255) * (255 - order) + ((quad->maximum_color >> 16) & 255) * order);
				g = DIV255(((quad->minimum_color >> 8) & 255) * (255 - order) + ((quad->maximum_color >> 8) & 255) * order);
				b = DIV255((quad->minimum_color & 255) * (255 - order) + (quad->maximum_color & 255) * order);
				if (order >= minimum_alpha)
				{
					r = HUD_MIN(255u, r + (unsigned)((quad->flash_color >> 16) & 255));
					g = HUD_MIN(255u, g + (unsigned)((quad->flash_color >> 8) & 255));
					b = HUD_MIN(255u, b + (unsigned)(quad->flash_color & 255));
				}
				r = DIV255(r * scale);
				g = DIV255(g * scale);
				b = DIV255(b * scale);
				a = full_alpha;
			}
			else
			{
				r = er;
				g = eg;
				b = eb;
				a = empty_alpha;
			}
			out[x] = PACK4444(DIV255(a * shape), DIV255(r * shape), DIV255(g * shape), DIV255(b * shape));
		}
	}
	pick->dirty = 1;
	pick->sprite = quad->sprite;
	pick->s0 = s0;
	pick->t0 = t0;
	pick->width = width;
	pick->height = height;
	pick->minimum_color = quad->minimum_color;
	pick->maximum_color = quad->maximum_color;
	pick->flash_color = quad->flash_color;
	pick->background_color = quad->background_color;
	pick->tint_color = quad->tint_color;
	pick->last_used = hud.frame;
	hud.meter_builds++;
	return &pick->header;
}

/* ---------- the game's quads */

void n64_hud_quad(const struct n64_hud_quad *quad)
{
	struct hud_sprite *sprite;
	const pvr_poly_hdr_t *header;
	float s[4], t[4], u[4], v[4], s0, s1, t0, t1;
	int is0, it0, is1, it1, upright, i;
	uint32_t argb = (uint32_t)quad->color;
	pvr_vertex_t *d;

	if (!hud.drawing || quad->sprite < 0 || quad->sprite >= hud.sprite_count)
		return;
	sprite = &hud_sprites[quad->sprite];
	if (sprite->ramp)
	{
		/* (upright quads only, as the menus draw them) */
		hud_ramp(sprite, quad, HUD_MIN(quad->t[0], quad->t[2]) * sprite->scale,
			HUD_MAX(quad->t[0], quad->t[2]) * sprite->scale);
		return;
	}
	if (!sprite->meter && !hud_stream(sprite))
		return;
	/* the N64's half-size texels (n64_hud_game.c) in the stored sprite's */
	for (i = 0; i < 4; i++)
	{
		s[i] = quad->s[i] * sprite->scale;
		t[i] = quad->t[i] * sprite->scale;
	}
	upright = fabsf(quad->y[0] - quad->y[1]) < 0.5f && fabsf(quad->x[1] - quad->x[2]) < 0.5f &&
		quad->x[1] > quad->x[0] && quad->y[2] > quad->y[1];
	s0 = HUD_MIN(s[0], s[2]);
	s1 = HUD_MAX(s[0], s[2]);
	t0 = HUD_MIN(t[0], t[2]);
	t1 = HUD_MAX(t[0], t[2]);
	is0 = floor_int(HUD_MAX(s0, 0.f));
	it0 = floor_int(HUD_MAX(t0, 0.f));
	is1 = ceil_int(HUD_MIN(s1, (float)sprite->width));
	it1 = ceil_int(HUD_MIN(t1, (float)sprite->height));
	if (is1 <= is0 || it1 <= it0)
		return;
	if (quad->meter)
	{
		if (!sprite->meter)
			return;
		header = meter_texture(quad, sprite, is0, it0, is1 - is0, it1 - it0);
		if (!header)
			return;
		for (i = 0; i < 4; i++)
		{
			u[i] = (s[i] - is0) * (1.f / METER_W);
			v[i] = (t[i] - it0) * (1.f / METER_H);
		}
		argb = 0xFFFFFFFFu;
	}
	else
	{
		int point = 0;

		if (!sprite->texture)
			return;
		/* point sampled at one texel a pixel (the HUD's own pixels), else
		bilinear */
		if (upright && s1 > s0 && t1 > t0)
		{
			float scale_x = (quad->x[1] - quad->x[0]) / (s1 - s0), scale_y = (quad->y[2] - quad->y[1]) / (t1 - t0);

			point = fabsf(scale_x - 1.f) < 0.02f && fabsf(scale_y - 1.f) < 0.02f;
		}
		header = &sprite->header[point];
		for (i = 0; i < 4; i++)
		{
			u[i] = s[i] * sprite->u_scale;
			v[i] = t[i] * sprite->v_scale;
		}
	}
	d = hud_quad_room(header);
	if (d)
		hud_put(d, quad->x, quad->y, u, v, argb);
}

/* ---------- text */

/* one of Halo's glyphs (n64_hud_game.c, from draw_string.c), the part of
it from (s, t) width x height at (x, y); colour straight ARGB */
void n64_hud_glyph(int glyph, float x, float y, int s, int t, int width, int height, unsigned long color)
{
	const struct dch_glyph *g;

	if (!hud.drawing || glyph < 0 || glyph >= hud.glyph_count || !hud.atlas)
		return;
	g = &hud.glyphs[glyph];
	if (s < 0 || t < 0 || s + width > g->width || t + height > g->height || width <= 0 || height <= 0)
		return;
	hud_rect(&hud.font_header, x, y, x + width, y + height, (g->x + s) * hud.atlas_u, (g->y + t) * hud.atlas_v,
		(g->x + s + width) * hud.atlas_u, (g->y + t + height) * hud.atlas_v, premultiply(color));
	hud.glyphs_drawn++;
}

/* a font's characters: the one asked for, else the HUD's (NULL: none) */
static const int16_t *font_characters(const struct n64_font *font)
{
	return font ? font->map : hud.font >= 0 ? hud.font_map : NULL;
}

static float font_text_width(const struct n64_font *font, const char *text)
{
	const int16_t *map = font_characters(font);
	float width = 0.f;

	if (!map)
		return (float)strlen(text) * 8.f;
	for (; *text; text++)
	{
		int c = (unsigned char)*text, i;

		if (c >= FIRST_CHARACTER && c < FIRST_CHARACTER + CHARACTER_COUNT && (i = map[c - FIRST_CHARACTER]) >= 0)
			width += hud.glyphs[i].advance;
	}
	return width;
}

static float text_width(const char *text)
{
	return font_text_width(NULL, text);
}

/* a font (NULL: the HUD's), its baseline at y, squeezed across by
x_scale; colour straight ARGB */
static void font_text_draw(const struct n64_font *font, float x, float y, unsigned long color, const char *text,
	float x_scale)
{
	const int16_t *map = font_characters(font);
	uint32_t argb = premultiply(color);

	if (!map || !hud.atlas)
		return;
	for (; *text; text++)
	{
		int c = (unsigned char)*text, i;
		const struct dch_glyph *g;

		if (c < FIRST_CHARACTER || c >= FIRST_CHARACTER + CHARACTER_COUNT || (i = map[c - FIRST_CHARACTER]) < 0)
			continue;
		g = &hud.glyphs[i];
		if (g->width && g->height)
		{
			float gx = x - g->origin_x * x_scale, gy = y - g->origin_y;

			hud_rect(&hud.font_header, gx, gy, gx + g->width * x_scale, gy + g->height, g->x * hud.atlas_u,
				g->y * hud.atlas_v, (g->x + g->width) * hud.atlas_u, (g->y + g->height) * hud.atlas_v, argb);
		}
		x += g->advance * x_scale;
	}
}

static void text_draw(float x, float y, unsigned long color, const char *text, float x_scale)
{
	font_text_draw(NULL, x, y, color, text, x_scale);
}

/* from the top of the text's box (n64_hud_game.c's prompts, Firefight's
line), with Halo's drop shadow (rasterizer_text.c: black at the text's
alpha, a pixel down and right) */
void n64_hud_text(int x, int y, unsigned long color, const char *text)
{
	float squeeze = n64_render_hud_squeeze();

	if (!hud.drawing || !text || !*text)
		return;
	text_draw((float)x + 1.f, (float)(y + hud.font_ascent) + 1.f, color & 0xFF000000u, text, squeeze);
	text_draw((float)x, (float)(y + hud.font_ascent), color, text, squeeze);
}

float n64_hud_text_width(const char *text)
{
	return text_width(text) * n64_render_hud_squeeze();
}

int n64_hud_text_height(void)
{
	return hud.font >= 0 ? hud.font_ascent + hud.font_descent : 12;
}

/* a motion sensor blip, added (its colour's alpha 0: Halo's glow) */
void n64_hud_blip(float x, float y, float width, float height, unsigned long color)
{
	if (!(color & 0xF0F0F0) || !hud.blip_texture)
		return;
	hud_rect(&hud.blip_header, x - width * 0.5f, y - height * 0.5f, x + width * 0.5f, y + height * 0.5f,
		0.f, 0.f, 1.f, 1.f, (uint32_t)(color & 0xFFFFFF));
}

/* ---------- the frame (dc_render.c n64_render_frame_end) */

/* the meters made this frame into their textures: after the opaque list
(its direct rendering has the store queues), before the others */
void dc_hud_upload(void)
{
	int i;

	for (i = 0; i < METER_SLOTS; i++)
	{
		if (meters[i].dirty && meters[i].texture)
			pvr_txr_load(meter_pixels[i], meters[i].texture, sizeof(meter_pixels[0]));
		meters[i].dirty = 0;
	}
}

int dc_hud_pending(void)
{
	return hud.count > 0;
}

/* into the open translucent list, after the world's */
void dc_hud_submit(void)
{
	if (hud.count)
		pvr_prim(hud_stage, (size_t)hud.count * sizeof(hud_entry));
}

void dc_hud_frame_end(void)
{
	hud.count = hud.quads_this_frame = 0;
	hud.last_header = NULL;
	hud.drawing = 0;
	if (++hud.frame % 600 == 0 && hud.quads)
	{
		char line[160];

		snprintf(line, sizeof(line), "dch: %lu quads a frame (%lu glyphs), %lu overflowed, %lu meter textures made, "
			"%lu meters left out", hud.quads / 600, hud.glyphs_drawn / 600, hud.overflows, hud.meter_builds,
			hud.meter_misses);
		n64_log(line);
		if (hud.stream_file >= 0)
		{
			snprintf(line, sizeof(line), "dch: streamed sprites, %lu KB in video memory, %lu loaded, %lu given back, "
				"%lu failed, %lu KB left", hud.streamed_bytes / 1024, hud.stream_loads, hud.stream_evictions,
				hud.stream_failures, (unsigned long)pvr_mem_available() / 1024);
			n64_log(line);
			hud.stream_loads = hud.stream_evictions = hud.stream_failures = 0;
		}
		hud.quads = hud.glyphs_drawn = hud.overflows = hud.meter_builds = hud.meter_misses = 0;
	}
}

/* ---------- the pause screens (n64_hud.c's, over the frame), in the HUD's
font; the button names the Dreamcast's */

#define PAUSE_BACKDROP 0x70000000u
#define PAUSE_PANEL 0xC0001028u
#define PAUSE_FRAME 0xFF8CC0FFu
#define PAUSE_CHOSEN 0xA0306CB8u
#define PAUSE_HUMAN 0x40306CB8u
#define PAUSE_TEXT 0xFF8CC0FFu
#define PAUSE_BRIGHT 0xFFFFFFFFu
#define PAUSE_DIM 0xC05880B0u

static const unsigned long pause_team_colors[2] = {0xFFD03838u, 0xFF3C6CE0u};
static float pause_middle, pause_squeeze;

static float pause_x(float x)
{
	return pause_middle + (x - pause_middle) * pause_squeeze;
}

static void pause_box(float x0, int y0, float x1, int y1, unsigned long argb)
{
	hud_fill(pause_x(x0), (float)y0, pause_x(x1), (float)y1, argb);
}

/* a frame's four sides (not overlapping: they are blended) */
static void pause_frame(float x0, int y0, float x1, int y1, int line, unsigned long argb)
{
	pause_box(x0, y0, x1, y0 + line, argb);
	pause_box(x0, y1 - line, x1, y1, argb);
	pause_box(x0, y0 + line, x0 + line, y1 - line, argb);
	pause_box(x1 - line, y0 + line, x1, y1 - line, argb);
}

enum { PAUSE_LEFT, PAUSE_RIGHT, PAUSE_CENTRE };

/* text from x, ending at x or about x; y its baseline; narrowed to fit
maximum (0: any width) */
static void pause_text_fit(float x, int y, int align, unsigned long color, const char *text, int maximum)
{
	float width = text_width(text);
	float scale = maximum > 0 && width > maximum ? maximum / width : 1.f, shown = width * scale;

	if (align == PAUSE_RIGHT)
		x -= shown;
	else if (align == PAUSE_CENTRE)
		x -= shown * 0.5f;
	text_draw(pause_x(x), (float)y, color, text, pause_squeeze * scale);
}

static void pause_text(float x, int y, int align, unsigned long color, const char *text)
{
	pause_text_fit(x, y, align, color, text, 0);
}

/* drawn after every view's HUD, into the same frame */
static int pause_begin(void)
{
	if (!dc_pvr_up)
		return 0;
	hud.drawing = 1;
	pause_middle = n64_render_screen_width() * 0.5f;
	pause_squeeze = n64_render_hud_squeeze();
	/* the game behind, darkened */
	hud_fill(0.f, 0.f, (float)n64_render_screen_width(), (float)n64_render_screen_height(), PAUSE_BACKDROP);
	return 1;
}

void n64_hud_pause(const struct n64_pause_screen *screen)
{
	int width = n64_render_screen_width(), height = n64_render_screen_height();
	/* (480 lines: two pixels where the N64's 240 have one) */
	int unit = height >= 480 ? 2 : 1, pad = 6 * unit, gap = 8 * unit;
	int rows = screen->row_count, cap, pitch, total, i, y, top, box_height;
	int name_width, score_width, kills_width, deaths_width, marker, button_width, inner;
	float x0, x1, name_x, score_x, kills_x, deaths_x;
	char number[12];

	if (!pause_begin())
		return;
	if (rows > N64_SCORE_ROWS)
		rows = N64_SCORE_ROWS;
	/* the columns as wide as their widest text */
	cap = hud.font_cap;
	name_width = (int)text_width("NAME");
	score_width = (int)text_width("SCORE");
	kills_width = (int)text_width("KILLS");
	deaths_width = (int)text_width("DEATHS");
	for (i = 0; i < rows; i++)
	{
		const struct n64_score_row *row = &screen->rows[i];

		name_width = HUD_MAX(name_width, (int)text_width(row->name));
		score_width = HUD_MAX(score_width, (int)text_width(row->score));
		snprintf(number, sizeof(number), "%d", row->kills);
		kills_width = HUD_MAX(kills_width, (int)text_width(number));
		snprintf(number, sizeof(number), "%d", row->deaths);
		deaths_width = HUD_MAX(deaths_width, (int)text_width(number));
	}
	marker = cap + 3 * unit;
	button_width = (int)text_width("MAIN MENU") + 2 * pad;
	inner = marker + name_width + 3 * gap + score_width + kills_width + deaths_width;
	inner = HUD_MAX(inner, 2 * button_width + 2 * gap + (int)text_width("START RESUME"));
	inner = HUD_MAX(inner, (int)text_width(screen->note ? screen->note : ""));
	x0 = (width - inner) * 0.5f - pad;
	x1 = (width + inner) * 0.5f + pad;
	/* from the top: the title (and its note), the headings over a rule, a
	row each, the buttons; closer rows if they don't fit */
	pitch = cap + 4 * unit;
	for (;;)
	{
		box_height = pitch + 4 * unit;
		total = pad + pitch * (screen->note ? 2 : 1) + 2 * unit + pitch + unit + 3 * unit + rows * pitch +
			4 * unit + box_height + pad;
		if (total <= height - 4 * unit || pitch <= cap + 1)
			break;
		pitch--;
	}
	top = (height - total) / 2;
	pause_box(x0, top, x1, top + total, PAUSE_PANEL);
	pause_frame(x0, top, x1, top + total, unit, PAUSE_FRAME);
	y = top + pad;
	pause_text(pause_middle, y + cap, PAUSE_CENTRE, PAUSE_BRIGHT, screen->title);
	y += pitch;
	if (screen->note)
	{
		pause_text(pause_middle, y + cap, PAUSE_CENTRE, PAUSE_TEXT, screen->note);
		y += pitch;
	}
	y += 2 * unit;
	name_x = x0 + pad + marker;
	score_x = name_x + name_width + gap + score_width;
	kills_x = score_x + gap + kills_width;
	deaths_x = kills_x + gap + deaths_width;
	pause_text(name_x, y + cap, PAUSE_LEFT, PAUSE_DIM, "NAME");
	pause_text(score_x, y + cap, PAUSE_RIGHT, PAUSE_DIM, "SCORE");
	pause_text(kills_x, y + cap, PAUSE_RIGHT, PAUSE_DIM, "KILLS");
	pause_text(deaths_x, y + cap, PAUSE_RIGHT, PAUSE_DIM, "DEATHS");
	y += pitch;
	pause_box(x0 + pad, y, x1 - pad, y + unit, 0x808CC0FFu);
	y += unit + 3 * unit;
	for (i = 0; i < rows; i++)
	{
		const struct n64_score_row *row = &screen->rows[i];
		unsigned long color = row->human ? PAUSE_BRIGHT : PAUSE_TEXT;

		/* (the players on the controllers on a band of their own) */
		if (row->human)
			pause_box(x0 + pad - 2 * unit, y - unit, x1 - pad + 2 * unit, y + pitch - unit, PAUSE_HUMAN);
		if (row->team >= 0)
			pause_box(x0 + pad, y, x0 + pad + cap, y + cap, pause_team_colors[row->team & 1]);
		pause_text(name_x, y + cap, PAUSE_LEFT, color, row->name);
		pause_text(score_x, y + cap, PAUSE_RIGHT, color, row->score);
		snprintf(number, sizeof(number), "%d", row->kills);
		pause_text(kills_x, y + cap, PAUSE_RIGHT, color, number);
		snprintf(number, sizeof(number), "%d", row->deaths);
		pause_text(deaths_x, y + cap, PAUSE_RIGHT, color, number);
		y += pitch;
	}
	y += 4 * unit;
	for (i = 0; i < 2; i++)
	{
		float bx = x0 + pad + i * (button_width + gap);

		if (screen->selected == i)
		{
			pause_box(bx, y, bx + button_width, y + box_height, PAUSE_CHOSEN);
			pause_frame(bx, y, bx + button_width, y + box_height, unit, PAUSE_FRAME);
		}
		pause_text(bx + button_width * 0.5f, y + (box_height + cap) / 2, PAUSE_CENTRE,
			screen->selected == i ? PAUSE_BRIGHT : PAUSE_TEXT, i ? "MAIN MENU" : "CONTROLS");
	}
	pause_text(x1 - pad, y + (box_height + cap) / 2, PAUSE_RIGHT, PAUSE_DIM, "START RESUME");
	hud.drawing = 0;
}

void n64_hud_pause_controls(const int *ports, int count, const int *selected_rows)
{
	/* (the rows n64.h's CONTROL_*; the Dreamcast's names for what they
	set: dc_input.c lays the N64's schemes on the pad) */
	static const char *const names[CONTROL_COUNT] = {"SCHEME", "CROUCH", "LOOK", "FIRE", "STICK SPEED",
		"BUTTON SPEED", "JUMP PACK"};
	static const char *const schemes[N64_SCHEME_COUNT] = {"STANDARD", "BUMPER JUMPER", "GOLDENEYE", "NORTH",
		"GOLDEN STYLE"};
	int width = n64_render_screen_width(), height = n64_render_screen_height();
	int unit = height >= 480 ? 2 : 1, cap, i, r;
	int columns = count > 1 ? 2 : 1, rows = count > 2 ? 2 : 1;
	int top = 23 * unit, bottom = height - 27 * unit;

	if (count < 1 || count > 4 || !pause_begin())
		return;
	cap = hud.font_cap;
	for (i = 0; i < count; i++)
	{
		int port = ports[i], x0 = (i % columns) * width / columns + 4 * unit;
		int x1 = (i % columns + 1) * width / columns - 4 * unit;
		int y0 = top + (i / columns) * (bottom - top) / rows;
		int y1 = top + (i / columns + 1) * (bottom - top) / rows - 3 * unit;
		int pitch = (y1 - y0 - cap - 7 * unit) / CONTROL_COUNT;
		const struct n64_controls *controls;
		char text[48];

		if (port < 0 || port > 3)
			continue;
		controls = &n64_controls[port];
		pause_box((float)x0, y0, (float)x1, y1, PAUSE_PANEL);
		pause_frame((float)x0, y0, (float)x1, y1, unit, PAUSE_FRAME);
		snprintf(text, sizeof(text), "PLAYER %d  -  PORT %c", i + 1, 'A' + port);
		pause_text_fit((x0 + x1) * 0.5f, y0 + cap + 4 * unit, PAUSE_CENTRE, PAUSE_BRIGHT, text, x1 - x0 - 8 * unit);
		for (r = 0; r < CONTROL_COUNT; r++)
		{
			int y = y0 + cap + 8 * unit + r * pitch, selected = selected_rows[port] == r;
			const char *value = text;

			if (selected)
				pause_box((float)(x0 + 2 * unit), y - unit, (float)(x1 - 2 * unit), y + pitch - unit, PAUSE_CHOSEN);
			switch (r)
			{
			case CONTROL_SCHEME: value = schemes[controls->scheme < N64_SCHEME_COUNT ? controls->scheme : 0]; break;
			case CONTROL_CROUCH: value = controls->toggle_crouch ? "TOGGLE" : "HOLD"; break;
			case CONTROL_LOOK: value = controls->inverted ? "INVERTED" : "NORMAL"; break;
			/* (fire and grenades on the triggers, swapped or not) */
			case CONTROL_FIRE: value = controls->swap_fire ? "L TRIGGER" : "R TRIGGER"; break;
			case CONTROL_STICK_SPEED: snprintf(text, sizeof(text), "%d", 5 + controls->stick_speed); break;
			case CONTROL_C_SPEED: snprintf(text, sizeof(text), "%d", 5 + controls->c_speed); break;
			default: value = controls->no_rumble ? "OFF" : "ON"; break;
			}
			pause_text_fit((float)(x0 + 4 * unit), y + cap, PAUSE_LEFT, selected ? PAUSE_BRIGHT : PAUSE_TEXT, names[r],
				(x1 - x0 - 12 * unit) / 2);
			pause_text_fit((float)(x1 - 4 * unit), y + cap, PAUSE_RIGHT, selected ? PAUSE_BRIGHT : PAUSE_TEXT, value,
				(x1 - x0 - 12 * unit) / 2);
		}
	}
	pause_text_fit(pause_middle, height - 15 * unit, PAUSE_CENTRE, PAUSE_TEXT, "D-PAD: UP/DOWN ROW  LEFT/RIGHT CHANGE",
		width - 12 * unit);
	pause_text_fit(pause_middle, height - 5 * unit, PAUSE_CENTRE, PAUSE_DIM, "A CYCLE   B BACK   START RESUME",
		width - 12 * unit);
	hud.drawing = 0;
}

/* ---------- the menu's drawing (dc_menu.c): the menu's file is a HUD of
its own (hud/ui.dch, tools/n64/menu_dc.py: its pictures as sprites, its
fonts), drawn outside any view, between dc_hud_draw_begin and _end, in the
menu's 640x480 pixels; n64.h's font API (n64_font.c on the N64) over the
file's fonts */

int dc_hud_draw_begin(void)
{
	hud.drawing = dc_pvr_up;
	return hud.drawing;
}

void dc_hud_draw_end(void)
{
	hud.drawing = 0;
}

int dc_hud_sprite_size(int sprite, int *width, int *height)
{
	if (sprite < 0 || sprite >= hud.sprite_count || (!hud_sprites[sprite].texture && !hud_sprites[sprite].streamed))
		return 0;
	*width = hud_sprites[sprite].width;
	*height = hud_sprites[sprite].height;
	return 1;
}

/* the part (s0, t0)-(s1, t1) of a sprite, in its texels, over the box
(x0, y0)-(x1, y1); colour straight ARGB (its alpha the sprite's
opacity). Point sampled at one texel a pixel, else bilinear */
void dc_hud_sprite(int sprite, float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1,
	unsigned long argb)
{
	struct hud_sprite *e;
	int point;

	if (sprite < 0 || sprite >= hud.sprite_count || x1 <= x0 || y1 <= y0 || !hud_stream(e = &hud_sprites[sprite]))
		return;
	point = fabsf((x1 - x0) - (s1 - s0)) < 0.02f && fabsf((y1 - y0) - (t1 - t0)) < 0.02f;
	hud_rect(&e->header[point], x0, y0, x1, y1, s0 * e->u_scale, t0 * e->v_scale, s1 * e->u_scale, t1 * e->v_scale,
		premultiply(argb));
}

void dc_hud_box(float x0, float y0, float x1, float y1, unsigned long argb)
{
	if (x1 > x0 && y1 > y0)
		hud_fill(x0, y0, x1, y1, argb);
}

/* "large", "small": the file's ui\large_ui, ui\small_ui (any font whose
name ends so, or is so); NULL when the loaded file has none */
struct n64_font *n64_font_load(const char *name)
{
	size_t length = strlen(name);
	int f;

	for (f = 0; f < hud.font_count; f++)
	{
		const char *font = hud.fonts[f].name, *slash = strrchr(font, '\\');
		const char *base = slash ? slash + 1 : font;

		if (!strcmp(font, name) || !strcmp(base, name) ||
			(!strncmp(base, name, length) && !strcmp(base + length, "_ui")))
			return &hud.fonts[f];
	}
	return NULL;
}

/* (the fonts go with the file: n64_hud_release) */
void n64_font_free(struct n64_font *font)
{
	(void)font;
}

int n64_font_height(const struct n64_font *font)
{
	return font ? font->ascent + font->descent : 12;
}

int n64_font_ascent(const struct n64_font *font)
{
	return font ? font->ascent : 10;
}

int n64_font_cap_height(const struct n64_font *font)
{
	return font ? font->cap : 10;
}

int n64_text_width(const struct n64_font *font, const char *text)
{
	return font && text ? (int)(font_text_width(font, text) + 0.5f) : 0;
}

void n64_text_draw(const struct n64_font *font, int x, int y, unsigned long color, const char *text)
{
	if (font && text && hud.drawing)
		font_text_draw(font, (float)x, (float)y, color, text, 1.f);
}

void n64_text_draw_scaled(const struct n64_font *font, float x, int y, unsigned long color, const char *text,
	float x_scale)
{
	if (font && text && hud.drawing)
		font_text_draw(font, x, (float)y, color, text, x_scale);
}

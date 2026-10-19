/*
DC_FX.C

The effects' particles, explosions and shot trails on the PowerVR: n64.h's
particle API (port/n64/src/n64_particles.c on the N64), as the N64 has them
-- a pool of sprites that fly in straight lines (gravity and drag their
only physics), grow, fade in and out and face the camera; the N64's own
explosions (a flash, a fireball of puffs that swell, rise and cool, a
little smoke after); the sniper rifle's short vapour ribbon; the flame
trail -- and what the N64 never drew: the projectiles that have no model,
plasma bolts, bullets' tracers, the tank's shell (Halo shows them by their
lights and contrails, which the converter drops), a glow and a trail each,
by the look tools/n64/effects_dc.py gives their tag.

rom:/fx/<map>.dcf (effects_dc.py): the N64's records (each effect event's
emitters), little-endian, read whole into RAM at the map's load and read in
place by the effect code (n64_fx_event, from source/effects/effects.c,
HALO_N64); the 128x64 I4 atlas of the particles' pictures, made two
ARGB4444 textures here (white with the picture's alpha, and grey: the
flame's two colours are mixed by the texel); the explosions' list; the
projectiles' looks.

Everything goes on the translucent list (staged, dc_render.c), after the
world's own translucent polygons and before the HUD, as quads facing the
camera, 1/w their depth (a world one a hair nearer than its middle, a
first-person one, a muzzle flash on the player's own gun, at 1/w times 1024
like the first-person models: in front of the world, tested against the
weapon). The PowerVR sorts its translucent list itself (autosort): no
sorting here, unlike the N64's. A particle the effect calls additive is
added to the frame (SRCALPHA, ONE), the rest blended by alpha; none takes
the PowerVR's fog (it would add the fog's colour to a glow): each fades by
its distance into the fog instead, as on the N64. An explosion's puff is
the N64's two-colour mix, the edge's colour plus (the middle's - the
edge's) x the texel: the PowerVR's offset colour (the vertex's oargb, added
after the texture) is the edge's, the vertex colour the difference.

Video memory: 16 + 16 KB for the atlas, 8 KB for the fireball's puff (64x64,
made here at boot, kept from map to map), 512 bytes for the ribbon's.
*/

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dc_render.h"
#include "n64_sniper_ribbon.h"

/* twice the N64's pool and spawn budget: the Dreamcast has the room, and
one view to draw them in */
#define MAXIMUM_PARTICLES 256
#define SPAWNS_PER_SECOND 600.0f
#define SPAWN_BURST 128.0f
#define CELL 16
#define CELLS 32
#define ATLAS_COLUMNS 8
#define ATLAS_WIDTH 128
#define ATLAS_HEIGHT 64
#define ATLAS_BYTES 4096
#define COLOUR_BYTES 128
#define EMITTER_BYTES 80
/* a particle's largest half-size in pixels at the N64's 207 pixels a unit
(n64_particles.c), scaled by the view's */
#define MAXIMUM_PIXELS 20.0f
#define N64_PIXELS_PER_UNIT 207.0f
#define FIRST_PERSON_NEAR 0.01171875f
#define FIRST_PERSON_FAR 16.0f
/* the first-person models' 1/w factor (dc_render.c) */
#define FIRST_PERSON_Z 1024.0f

#define MAXIMUM_EXPLOSIONS 8
#define MAXIMUM_EXPLOSION_EFFECTS 16
#define FIREBALL 64
#define FIRE_PUFFS 7
#define SMOKE_PUFFS 5
#define EXPLOSION_PUFFS (1 + FIRE_PUFFS + SMOKE_PUFFS)
#define FLASH_TIME 0.12f
#define FIRE_TIME 0.75f
#define SMOKE_START 0.3f
#define SMOKE_TIME 1.3f

/* the projectiles' looks (effects_dc.py LOOK_*) */
#define LOOK_GLOW 1
#define LOOK_TRAIL 2
#define LOOK_ADDITIVE 4
#define MAXIMUM_LOOKS 64
#define TRAILS 48
#define GLOWS 32
/* a trail's segment: a tick's flight, at most this long (the newest part),
fading out over its life (three or four ticks of trail behind a shot); a
glow lives a tick and a half (each tick puts it where the bolt now is) */
#define TRAIL_LENGTH 3.0f
#define TRAIL_LIFE 0.11f
#define GLOW_LIFE 0.05f
#define RIBBON_W 32
#define RIBBON_H 8

/* ---------- fx/<map>.dcf, little-endian */

struct dcf_header
{
	char magic[4];
	uint32_t version;
	uint16_t count, cells;
	uint32_t index_offset, atlas_offset, explosions_offset;
	uint16_t explosions;
	uint8_t flame_cell, pad;
	uint32_t looks_offset;
};

struct dcf_index
{
	uint16_t tag_index, size;
	uint32_t offset;
};

struct dcf_look
{
	uint16_t tag_index;
	uint8_t flags, pad;
	uint8_t glow[4], trail[4]; /* RGBA */
	float glow_radius, trail_width;
};

typedef char dcf_sizes_assert[sizeof(struct dcf_header) == 32 && sizeof(struct dcf_index) == 8 &&
	sizeof(struct dcf_look) == 20 && sizeof(struct n64_fx_emitter) == EMITTER_BYTES ? 1 : -1];

/* how each kind of explosion looks (n64_particles.c's): the fireball's
radius (world units), the colours from the flash to the coolest fire, the
smoke's, and how much smoke (0-1) */
static const struct explosion_look
{
	float radius;
	uint8_t flash[3], hot[3], warm[3], cool[3], smoke[3];
	float smoke_amount;
} explosion_looks[N64_EXPLOSION_KINDS] = {
	/* frag grenade */
	{1.1f, {255, 250, 225}, {255, 214, 120}, {250, 128, 40}, {140, 40, 18}, {64, 60, 56}, 1.f},
	/* plasma grenade: blue */
	{1.0f, {235, 245, 255}, {150, 210, 255}, {70, 130, 255}, {36, 46, 170}, {72, 82, 112}, 0.25f},
	/* rocket */
	{1.35f, {255, 250, 225}, {255, 210, 110}, {250, 120, 36}, {130, 36, 16}, {60, 56, 52}, 1.f},
	/* tank shell */
	{1.6f, {255, 250, 225}, {255, 205, 100}, {245, 112, 32}, {120, 32, 14}, {58, 54, 50}, 1.f},
	/* the needler's: pink */
	{0.6f, {255, 235, 250}, {255, 160, 235}, {230, 80, 205}, {110, 28, 100}, {80, 62, 80}, 0.15f},
	/* the fuel rod's: green */
	{1.15f, {245, 255, 210}, {210, 255, 110}, {100, 230, 36}, {38, 100, 20}, {58, 78, 45}, 0.3f},
};

struct particle
{
	float position[3], velocity[3];
	float age, life, fade_in, fade_out;
	float radius, end_scale;
	float gravity, drag;
	uint32_t color; /* RGBA */
	uint8_t cell, flags;
	int8_t local_player;
	uint8_t live;
};

struct explosion
{
	float position[3];
	float age;
	uint8_t kind, seed, live, pad;
};

struct trail
{
	float start[3], end[3];
	float age, width;
	uint32_t argb;
	uint8_t additive, pad[3];
};

struct glow
{
	float position[3];
	float age, radius;
	uint32_t argb;
};

/* the headers: the atlas white (additive, by alpha), grey (the flame), the
fireball's puff, the ribbon (by alpha, additive) */
enum
{
	H_ALPHA,
	H_ADD,
	H_FLAME,
	H_FIRE,
	H_RIBBON,
	H_RIBBON_ADD,
	H_COUNT
};

static struct
{
	struct particle particles[MAXIMUM_PARTICLES];
	int next, live;
	/* the map's file, whole: the index, the records */
	uint8_t *file;
	const struct dcf_index *index;
	int index_count;
	const struct dcf_look *looks;
	int look_count;
	uint8_t cell_colours[CELLS][4];
	uint8_t flame_cell;
	struct { uint16_t tag_index; uint8_t kind, pad; } explosion_effects[MAXIMUM_EXPLOSION_EFFECTS];
	int explosion_effect_count;
	struct explosion explosions[MAXIMUM_EXPLOSIONS];
	int next_explosion;
	uint8_t explosion_seed;
	struct { float start[3], end[3], age; } ribbons[N64_SNIPER_RIBBONS];
	int next_ribbon;
	struct trail trails[TRAILS];
	int next_trail;
	struct glow glows[GLOWS];
	int next_glow;
	/* the spawn budget, and its share (half with three or four views) */
	float tokens, budget;
	int disabled, enhanced, own_cells;
	/* video memory: the map's atlas (white, grey), the kept puff and ribbon */
	pvr_ptr_t atlas_white, atlas_grey, fireball, ribbon;
	pvr_poly_hdr_t headers[H_COUNT] __attribute__((aligned(32)));
	const pvr_poly_hdr_t *current;
	/* statistics */
	unsigned long spawned, refused, drawn, quads, us, frames_drawn;
} fx = {.budget = 1.0f, .enhanced = 1};

/* ---------- textures */

static void fx_header(int which, pvr_ptr_t texture, int width, int height, int additive, int offset)
{
	pvr_poly_cxt_t c;

	pvr_poly_cxt_txr(&c, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED, width, height, texture,
		PVR_FILTER_BILINEAR);
	c.txr.env = PVR_TXRENV_MODULATEALPHA;
	c.txr.uv_clamp = PVR_UVCLAMP_UV;
	c.gen.shading = PVR_SHADE_GOURAUD;
	c.gen.culling = PVR_CULLING_NONE;
	c.gen.fog_type = PVR_FOG_DISABLE;
	c.gen.specular = offset ? PVR_SPECULAR_ENABLE : PVR_SPECULAR_DISABLE;
	c.depth.comparison = PVR_DEPTHCMP_GREATER;
	c.depth.write = PVR_DEPTHWRITE_DISABLE;
	c.blend.src = PVR_BLEND_SRCALPHA;
	c.blend.dst = additive ? PVR_BLEND_ONE : PVR_BLEND_INVSRCALPHA;
	pvr_poly_compile(&fx.headers[which], &c);
}

/* 16-bit texels to video memory (non-twiddled ARGB4444, row by row) */
static pvr_ptr_t fx_texture(const uint16_t *texels, int count)
{
	pvr_ptr_t texture = pvr_mem_malloc(count * 2);

	if (texture)
		pvr_txr_load(texels, texture, count * 2);
	return texture;
}

/* a small hash: 0..1 */
static float hash2(int x, int y, int seed)
{
	unsigned n = (unsigned)x * 374761393u + (unsigned)y * 668265263u + (unsigned)seed * 2147483647u;

	n = (n ^ (n >> 13)) * 1274126177u;
	return (float)((n ^ (n >> 16)) & 0xFFFF) / 65535.f;
}

/* value noise at a scale, bilinear */
static float value_noise(float x, float y, int seed)
{
	int ix = (int)floorf(x), iy = (int)floorf(y);
	float fx_ = x - ix, fy = y - iy;
	float a = hash2(ix, iy, seed), b = hash2(ix + 1, iy, seed);
	float c = hash2(ix, iy + 1, seed), d = hash2(ix + 1, iy + 1, seed);

	fx_ = fx_ * fx_ * (3.f - 2.f * fx_);
	fy = fy * fy * (3.f - 2.f * fy);
	return (a + (b - a) * fx_) + ((c + (d - c) * fx_) - (a + (b - a) * fx_)) * fy;
}

/* the fireball's puff (n64_particles.c's, at twice its size): a few soft
lumps, roughened by noise, its intensity both the alpha and the grey; and
the ribbon's: soft across, tapered at both ends. Made once, kept */
static void fx_make_kept_textures(void)
{
	static const float lumps[7][3] = {
		{0.f, 0.f, 0.62f}, {0.30f, 0.12f, 0.38f}, {-0.27f, 0.20f, 0.36f}, {0.06f, -0.32f, 0.38f},
		{-0.24f, -0.24f, 0.32f}, {0.32f, -0.20f, 0.30f}, {-0.04f, 0.34f, 0.32f},
	};
	uint16_t *texels;
	int x, y, i;

	if (fx.fireball && fx.ribbon)
		return;
	texels = memalign(32, FIREBALL * FIREBALL * 2);
	if (!texels)
		return;
	for (y = 0; y < FIREBALL; y++)
	{
		for (x = 0; x < FIREBALL; x++)
		{
			float u = (x + 0.5f) / (FIREBALL / 2) - 1.f, v = (y + 0.5f) / (FIREBALL / 2) - 1.f;
			float value = 0.f, noise;
			unsigned level;

			for (i = 0; i < 7; i++)
			{
				float du = u - lumps[i][0], dv = v - lumps[i][1];
				float d = (du * du + dv * dv) / (lumps[i][2] * lumps[i][2]);

				if (d < 1.f)
					value += (1.f - d) * (1.f - d);
			}
			/* (the N64's noise at its 32 texels: the same lumps here) */
			noise = value_noise((u + 1.f) * 4.f, (v + 1.f) * 4.f, 1) * 0.6f +
				value_noise((u + 1.f) * 8.f, (v + 1.f) * 8.f, 2) * 0.4f;
			value *= 0.55f + 0.7f * noise;
			value *= n64_max_f(0.f, 1.f - (u * u + v * v)) * 1.6f;
			level = value >= 1.f ? 15 : value <= 0.f ? 0 : (unsigned)(value * 15.f + 0.5f);
			texels[y * FIREBALL + x] = (uint16_t)(level << 12 | level << 8 | level << 4 | level);
		}
	}
	if (!fx.fireball)
		fx.fireball = fx_texture(texels, FIREBALL * FIREBALL);
	for (y = 0; y < RIBBON_H; y++)
	{
		float v = (y + 0.5f) / RIBBON_H * 2.f - 1.f;
		float across = n64_max_f(0.f, 1.f - v * v);

		for (x = 0; x < RIBBON_W; x++)
		{
			float u = (x + 0.5f) / RIBBON_W;
			float along = n64_min_f(1.f, n64_min_f(u, 1.f - u) * 6.f);
			unsigned level = (unsigned)(across * along * 15.f + 0.5f);

			texels[y * RIBBON_W + x] = (uint16_t)(level << 12 | 0x0FFF);
		}
	}
	if (!fx.ribbon)
		fx.ribbon = fx_texture(texels, RIBBON_W * RIBBON_H);
	free(texels);
	if (fx.fireball)
		fx_header(H_FIRE, fx.fireball, FIREBALL, FIREBALL, 0, 1);
	if (fx.ribbon)
	{
		fx_header(H_RIBBON, fx.ribbon, RIBBON_W, RIBBON_H, 0, 0);
		fx_header(H_RIBBON_ADD, fx.ribbon, RIBBON_W, RIBBON_H, 1, 0);
	}
}

/* the atlas's I4 texels (the high nibble the left one) as the two
ARGB4444 textures */
static int fx_make_atlas(const uint8_t *i4)
{
	uint16_t *texels = memalign(32, ATLAS_WIDTH * ATLAS_HEIGHT * 2);
	int pass, i;

	if (!texels)
		return 0;
	for (pass = 0; pass < 2; pass++)
	{
		for (i = 0; i < ATLAS_WIDTH * ATLAS_HEIGHT; i++)
		{
			unsigned level = (i & 1) ? i4[i >> 1] & 15 : i4[i >> 1] >> 4;

			texels[i] = (uint16_t)(level << 12 | (pass ? level << 8 | level << 4 | level : 0x0FFF));
		}
		if (pass)
			fx.atlas_grey = fx_texture(texels, ATLAS_WIDTH * ATLAS_HEIGHT);
		else
			fx.atlas_white = fx_texture(texels, ATLAS_WIDTH * ATLAS_HEIGHT);
	}
	free(texels);
	if (!fx.atlas_white || !fx.atlas_grey)
		return 0;
	fx_header(H_ALPHA, fx.atlas_white, ATLAS_WIDTH, ATLAS_HEIGHT, 0, 0);
	fx_header(H_ADD, fx.atlas_white, ATLAS_WIDTH, ATLAS_HEIGHT, 1, 0);
	fx_header(H_FLAME, fx.atlas_grey, ATLAS_WIDTH, ATLAS_HEIGHT, 0, 1);
	return 1;
}

/* ---------- the map's file */

static void fx_free(void)
{
	if (fx.atlas_white || fx.atlas_grey)
	{
		/* (the PowerVR done with the atlas) */
		if (dc_pvr_up)
			pvr_wait_render_done();
		if (fx.atlas_white)
			pvr_mem_free(fx.atlas_white);
		if (fx.atlas_grey)
			pvr_mem_free(fx.atlas_grey);
	}
	fx.atlas_white = fx.atlas_grey = NULL;
	free(fx.file);
	fx.file = NULL;
	fx.index = NULL;
	fx.index_count = 0;
	fx.looks = NULL;
	fx.look_count = 0;
	fx.explosion_effect_count = 0;
	fx.flame_cell = 0;
	memset(fx.cell_colours, 0, sizeof(fx.cell_colours));
}

static void fx_reset(void)
{
	int i;

	memset(fx.particles, 0, sizeof(fx.particles));
	fx.live = 0;
	memset(fx.explosions, 0, sizeof(fx.explosions));
	for (i = 0; i < N64_SNIPER_RIBBONS; i++)
		fx.ribbons[i].age = N64_SNIPER_LIFE;
	for (i = 0; i < TRAILS; i++)
		fx.trails[i].age = TRAIL_LIFE;
	for (i = 0; i < GLOWS; i++)
		fx.glows[i].age = GLOW_LIFE;
}

static int fx_span(uint32_t bytes, uint32_t offset, uint32_t size)
{
	return offset <= bytes && size <= bytes - offset;
}

/* a record's shape (n64_fx_format.h's n64_fx_record_memory, in this
byte order): its event table rising from 0 to its emitter count, then
exactly that many emitters */
static int fx_record_valid(const uint8_t *record, uint32_t bytes)
{
	const uint16_t *head = (const uint16_t *)record;
	uint32_t events, emitters, first, previous = 0, i;

	if (bytes < 8)
		return 0;
	events = head[0];
	emitters = head[1];
	first = (4 + 2 * (events + 1) + 3) & ~3u;
	if (first > bytes || bytes - first != emitters * EMITTER_BYTES)
		return 0;
	for (i = 0; i <= events; i++)
	{
		uint32_t current = head[2 + i];

		if ((!i && current) || current < previous || current > emitters)
			return 0;
		previous = current;
	}
	return previous == emitters;
}

/* the file checked (the SH-4 traps on an unaligned read, the effect code
reads the emitters as they are) before any of it is used */
static const char *fx_validate(const uint8_t *data, uint32_t bytes)
{
	const struct dcf_header *h = (const struct dcf_header *)data;
	const struct dcf_index *index;
	uint32_t index_end, atlas_end, records_end, i;

	if (bytes < sizeof(*h) || memcmp(h->magic, "DCFX", 4) || h->version != 1)
		return "not this build's";
	if (h->cells > CELLS || h->flame_cell >= CELLS || h->explosions > MAXIMUM_EXPLOSION_EFFECTS)
		return "its counts";
	if (h->index_offset < sizeof(*h) || (h->index_offset & 3) || !fx_span(bytes, h->index_offset, h->count * 8u))
		return "its index";
	index_end = h->index_offset + h->count * 8u;
	if ((h->atlas_offset & 15) || h->atlas_offset < index_end ||
		!fx_span(bytes, h->atlas_offset, ATLAS_BYTES + COLOUR_BYTES))
		return "its atlas";
	atlas_end = h->atlas_offset + ATLAS_BYTES + COLOUR_BYTES;
	if ((h->explosions_offset & 3) || h->explosions_offset < atlas_end ||
		!fx_span(bytes, h->explosions_offset, h->explosions * 4u))
		return "its explosions";
	records_end = h->explosions_offset;
	if ((h->looks_offset & 3) || h->looks_offset < h->explosions_offset + h->explosions * 4u ||
		!fx_span(bytes, h->looks_offset, 4) ||
		*(const uint16_t *)(data + h->looks_offset) > MAXIMUM_LOOKS ||
		!fx_span(bytes, h->looks_offset + 4, *(const uint16_t *)(data + h->looks_offset) * sizeof(struct dcf_look)))
		return "its projectiles' looks";
	index = (const struct dcf_index *)(data + h->index_offset);
	for (i = 0; i < h->count; i++)
	{
		if ((i && index[i].tag_index <= index[i - 1].tag_index) || (index[i].offset & 15) ||
			index[i].offset < atlas_end || !fx_span(records_end, index[i].offset, index[i].size) ||
			!fx_record_valid(data + index[i].offset, index[i].size))
			return "a record";
	}
	for (i = 0; i < h->explosions; i++)
	{
		const uint8_t *e = data + h->explosions_offset + i * 4;

		if (e[2] >= N64_EXPLOSION_KINDS || e[3] ||
			(i && *(const uint16_t *)e <= *(const uint16_t *)(e - 4)))
			return "an explosion";
	}
	{
		const struct dcf_look *looks = (const struct dcf_look *)(data + h->looks_offset + 4);
		uint32_t count = *(const uint16_t *)(data + h->looks_offset);

		for (i = 1; i < count; i++)
		{
			if (looks[i].tag_index <= looks[i - 1].tag_index)
				return "a projectile's look";
		}
	}
	return NULL;
}

void n64_particles_load(const char *map_name)
{
	char path[64], line[160];
	const struct dcf_header *h;
	const char *why = NULL;
	long file, size = 0;
	int i;

	fx_free();
	fx_reset();
	if (!dc_pvr_up)
		return;
	fx_make_kept_textures();
	fx.own_cells = n64_debug_flag("dc_fx_cells");
	if (!map_name)
		return;
	snprintf(path, sizeof(path), "rom:/fx/%s.dcf", map_name);
	file = n64_file_open(path);
	if (file < 0)
	{
		n64_log("dcx: no particles file");
		return;
	}
	size = n64_file_size(file);
	fx.file = size > 0 ? memalign(32, size) : NULL;
	if (!fx.file || n64_file_read(file, fx.file, size) != size)
		why = "could not be read";
	n64_file_close(file);
	if (!why)
		why = fx_validate(fx.file, (uint32_t)size);
	h = (const struct dcf_header *)fx.file;
	if (!why && !fx_make_atlas(fx.file + h->atlas_offset))
		why = "no video memory for its atlas";
	if (why)
	{
		snprintf(line, sizeof(line), "dcx: the particles file: %s", why);
		n64_log(line);
		fx_free();
		return;
	}
	fx.index = (const struct dcf_index *)(fx.file + h->index_offset);
	fx.index_count = h->count;
	memcpy(fx.cell_colours, fx.file + h->atlas_offset + ATLAS_BYTES, sizeof(fx.cell_colours));
	fx.flame_cell = h->flame_cell;
	for (i = 0; i < h->explosions; i++)
	{
		const uint8_t *e = fx.file + h->explosions_offset + i * 4;

		fx.explosion_effects[i].tag_index = *(const uint16_t *)e;
		fx.explosion_effects[i].kind = e[2];
	}
	fx.explosion_effect_count = h->explosions;
	fx.look_count = *(const uint16_t *)(fx.file + h->looks_offset);
	fx.looks = (const struct dcf_look *)(fx.file + h->looks_offset + 4);
	snprintf(line, sizeof(line),
		"dcx: particles for %d effects of %s, %d explosions, %d projectile looks; %ld KB, %lu KB of video memory left",
		fx.index_count, map_name, fx.explosion_effect_count, fx.look_count, size / 1024,
		(unsigned long)pvr_mem_available() / 1024);
	n64_log(line);
}

void n64_particles_reserve(void)
{
	if (dc_pvr_up)
		fx_make_kept_textures();
}

/* the map's effects given back (the puff and ribbon stay): at the end of a
game (n64_game_free_map) */
void n64_particles_release(void)
{
	fx_free();
	fx_reset();
}

int n64_explosion_kind(int effect_tag_index)
{
	int i;

	for (i = 0; i < fx.explosion_effect_count; i++)
	{
		if (fx.explosion_effects[i].tag_index == effect_tag_index)
			return fx.explosion_effects[i].kind;
	}
	return -1;
}

/* an effect's event's emitters, in place (by binary search of the index) */
const struct n64_fx_emitter *n64_fx_event(int effect_tag_index, int event_index, int *count)
{
	int low = 0, high = fx.index_count - 1;

	*count = 0;
	while (low <= high)
	{
		int middle = (low + high) / 2;
		const struct dcf_index *entry = &fx.index[middle];

		if (entry->tag_index == effect_tag_index)
		{
			const uint8_t *record = fx.file + entry->offset;
			const uint16_t *head = (const uint16_t *)record;
			int events = head[0];

			if (event_index < 0 || event_index >= events)
				return NULL;
			*count = head[3 + event_index] - head[2 + event_index];
			return (const struct n64_fx_emitter *)(record + ((4 + 2 * (events + 1) + 3) & ~3)) + head[2 + event_index];
		}
		if (entry->tag_index < effect_tag_index)
			low = middle + 1;
		else
			high = middle - 1;
	}
	return NULL;
}

/* ---------- making them */

void n64_particle_spawn(const struct n64_particle_spawn *spawn)
{
	struct particle *p = &fx.particles[fx.next];

	if (fx.disabled)
		return;
	/* (a budget: a muzzle flash or an impact gets through, a stream cannot
	fill the pool) */
	if (fx.tokens < 1.f)
	{
		fx.refused++;
		return;
	}
	fx.tokens -= 1.f;
	/* (full: the oldest goes) */
	fx.next = (fx.next + 1) % MAXIMUM_PARTICLES;
	if (!p->live)
		fx.live++;
	memcpy(p->position, spawn->position, sizeof(p->position));
	memcpy(p->velocity, spawn->velocity, sizeof(p->velocity));
	p->age = 0.f;
	/* (a muzzle flash lives ~30 ms, less than a frame: at least 80, so that
	it is drawn at all) */
	p->life = spawn->life > 0.08f ? spawn->life : 0.08f;
	if (fx.enhanced && p->life < 0.11f)
		p->life = 0.11f;
	p->fade_in = spawn->fade_in;
	p->fade_out = spawn->fade_out;
	p->radius = spawn->radius;
	if (fx.enhanced && (spawn->flags & N64_PARTICLE_FIRST_PERSON))
		p->radius *= 1.08f;
	p->end_scale = spawn->end_scale;
	p->gravity = spawn->gravity;
	p->drag = spawn->drag;
	p->color = spawn->color;
	p->cell = spawn->cell;
	p->flags = spawn->flags;
	p->local_player = spawn->local_player;
	p->live = 1;
	fx.spawned++;
}

/* the flamethrower's flame (n64_particles.c's): a growing fire puff where
the projectile is, a share of the particles' budget */
void n64_flame_trail(const float *position, const float *velocity)
{
	struct n64_particle_spawn p = {0};
	int k;

	for (k = 0; k < 3; k++)
	{
		p.position[k] = position[k];
		p.velocity[k] = velocity[k] * 3.f;
	}
	p.velocity[2] += 0.12f;
	p.radius = 0.055f;
	p.end_scale = fx.enhanced ? 3.2f : 2.f;
	p.life = fx.enhanced ? 0.32f : 0.2f;
	p.fade_in = 0.025f;
	p.fade_out = 0.18f;
	p.drag = 2.f;
	p.color = 0xFFB040D0;
	p.cell = fx.flame_cell;
	p.flags = N64_PARTICLE_FLAME;
	p.local_player = -1;
	n64_particle_spawn(&p);
}

void n64_sniper_ribbon(const float *start, const float *end)
{
	int index = fx.next_ribbon;

	if (fx.disabled ||
		!sniper_ribbon_segment(start, end, fx.ribbons[index].start, fx.ribbons[index].end))
		return;
	fx.ribbons[index].age = 0.f;
	fx.next_ribbon = (index + 1) % N64_SNIPER_RIBBONS;
}

void n64_explosion(const float position[3], int kind)
{
	struct explosion *e = &fx.explosions[fx.next_explosion];

	if (fx.disabled || kind < 0 || kind >= N64_EXPLOSION_KINDS)
		return;
	/* (full: the oldest goes) */
	fx.next_explosion = (fx.next_explosion + 1) % MAXIMUM_EXPLOSIONS;
	memcpy(e->position, position, sizeof(e->position));
	e->age = 0.f;
	e->kind = (uint8_t)kind;
	e->seed = fx.explosion_seed;
	fx.explosion_seed += 37;
	e->live = 1;
}

static const struct dcf_look *fx_look(int tag_index)
{
	int low = 0, high = fx.look_count - 1;

	while (low <= high)
	{
		int middle = (low + high) / 2;

		if (fx.looks[middle].tag_index == tag_index)
			return &fx.looks[middle];
		if (fx.looks[middle].tag_index < tag_index)
			low = middle + 1;
		else
			high = middle - 1;
	}
	return NULL;
}

static uint32_t fx_argb(const uint8_t *rgba)
{
	return (uint32_t)rgba[3] << 24 | (uint32_t)rgba[0] << 16 | (uint32_t)rgba[1] << 8 | rgba[2];
}

/* a projectile's flight this tick (projectiles.c, HALO_DC): its trail's
newest segment, its glow where it now is */
void n64_fx_projectile(int projectile_tag_index, const float *start, const float *end)
{
	const struct dcf_look *look;

	if (fx.disabled || !fx.look_count || !(look = fx_look(projectile_tag_index)))
		return;
	if (look->flags & LOOK_TRAIL)
	{
		struct trail *t = &fx.trails[fx.next_trail];
		float d[3], length2 = 0.f, scale;
		int k;

		for (k = 0; k < 3; k++)
		{
			d[k] = end[k] - start[k];
			length2 += d[k] * d[k];
		}
		if (isfinite(length2) && length2 > 0.0001f)
		{
			scale = length2 > TRAIL_LENGTH * TRAIL_LENGTH ? TRAIL_LENGTH / sqrtf(length2) : 1.f;
			for (k = 0; k < 3; k++)
			{
				t->end[k] = end[k];
				t->start[k] = end[k] - d[k] * scale;
			}
			t->age = 0.f;
			t->width = look->trail_width;
			t->argb = fx_argb(look->trail);
			t->additive = (look->flags & LOOK_ADDITIVE) != 0;
			fx.next_trail = (fx.next_trail + 1) % TRAILS;
		}
	}
	if ((look->flags & LOOK_GLOW) && isfinite(end[0]) && isfinite(end[1]) && isfinite(end[2]))
	{
		struct glow *g = &fx.glows[fx.next_glow];

		memcpy(g->position, end, sizeof(g->position));
		g->age = 0.f;
		g->radius = look->glow_radius;
		g->argb = fx_argb(look->glow);
		fx.next_glow = (fx.next_glow + 1) % GLOWS;
	}
}

void n64_particles_update(float dt)
{
	int i, k;

	if (!(isfinite(dt) && dt > 0.f))
		return;
	if (dt > 0.25f)
		dt = 0.25f;
	for (i = 0; i < N64_SNIPER_RIBBONS; i++)
		if (fx.ribbons[i].age < N64_SNIPER_LIFE)
			fx.ribbons[i].age += dt;
	for (i = 0; i < TRAILS; i++)
		if (fx.trails[i].age < TRAIL_LIFE)
			fx.trails[i].age += dt;
	for (i = 0; i < GLOWS; i++)
		if (fx.glows[i].age < GLOW_LIFE)
			fx.glows[i].age += dt;
	fx.tokens += dt * SPAWNS_PER_SECOND * fx.budget;
	if (fx.tokens > SPAWN_BURST * fx.budget)
		fx.tokens = SPAWN_BURST * fx.budget;
	for (i = 0; i < MAXIMUM_EXPLOSIONS; i++)
	{
		struct explosion *e = &fx.explosions[i];

		if (e->live && (e->age += dt) >= SMOKE_START + SMOKE_TIME)
			e->live = 0;
	}
	if (!fx.live)
		return;
	for (i = 0; i < MAXIMUM_PARTICLES; i++)
	{
		struct particle *p = &fx.particles[i];
		float keep;

		if (!p->live)
			continue;
		p->age += dt;
		if (p->age >= p->life)
		{
			p->live = 0;
			fx.live--;
			continue;
		}
		if (!(p->flags & N64_PARTICLE_FIRST_PERSON))
			p->velocity[2] -= p->gravity * dt;
		keep = 1.f - p->drag * dt;
		if (keep < 0.f)
			keep = 0.f;
		for (k = 0; k < 3; k++)
		{
			p->velocity[k] *= keep;
			p->position[k] += p->velocity[k] * dt;
		}
	}
}

/* ---------- drawing */

/* the view's camera for the particles: dc_render.c's (the same eye and
axes in the world's pass and the first-person one), the world's near
plane, the fog's range, the view's rectangle on the screen (a quad wholly
outside it is not drawn: the view's own, not the screen's, so a split
screen's view sends nothing that only another view would show) */
static struct
{
	float eye[3], r[3], u[3], f[3];
	float k, cx, cy, near, far, fog_start, fog_end;
	float x0, y0, x1, y1;
	int local_player;
} V;

/* a quad of this half-width round sx, sy wholly outside the view */
static inline int fx_outside(float sx, float sy, float radius)
{
	return sx + radius < V.x0 || sx - radius > V.x1 || sy + radius < V.y0 || sy - radius > V.y1;
}

static void fx_use(int which)
{
	const pvr_poly_hdr_t *h = &fx.headers[which];

	if (fx.current != h)
	{
		dc_send_header(h);
		fx.current = h;
	}
}

static inline void fx_vertex(float x, float y, float z, float u, float v, uint32_t argb, uint32_t oargb, int last)
{
	pvr_vertex_t *d = dc_slot();

	d->flags = last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
	d->x = x;
	d->y = y;
	d->z = z;
	d->u = u;
	d->v = v;
	d->argb = argb;
	d->oargb = oargb;
	dc_commit(d);
}

/* a quad facing the camera: its corners on the screen, 1/w, the texture's
corners */
static void fx_quad(int which, float x0, float y0, float x1, float y1, float z, float u0, float v0, float u1,
	float v1, uint32_t argb, uint32_t oargb)
{
	if (!dc_stage_room(5))
		return;
	fx_use(which);
	fx_vertex(x0, y0, z, u0, v0, argb, oargb, 0);
	fx_vertex(x1, y0, z, u1, v0, argb, oargb, 0);
	fx_vertex(x0, y1, z, u0, v1, argb, oargb, 0);
	fx_vertex(x1, y1, z, u1, v1, argb, oargb, 1);
	fx.quads++;
}

/* a world position in the camera's space: across, up, ahead */
static inline void fx_camera_space(const float *p, float *x, float *y, float *z)
{
	float dx = p[0] - V.eye[0], dy = p[1] - V.eye[1], dz = p[2] - V.eye[2];

	*x = dx * V.r[0] + dy * V.r[1] + dz * V.r[2];
	*y = dx * V.u[0] + dy * V.u[1] + dz * V.u[2];
	*z = dx * V.f[0] + dy * V.f[1] + dz * V.f[2];
}

/* how much the fog leaves of something this far: 1 to its start, 0 at its
end */
static inline float fx_fog(float z)
{
	if (V.fog_end <= 0.f || z <= V.fog_start)
		return 1.f;
	return z >= V.fog_end ? 0.f : 1.f - (z - V.fog_start) / (V.fog_end - V.fog_start);
}

/* the cell's texture coordinates, half a texel in (no neighbour's bleeding
in with the bilinear filter) */
static void fx_cell(int cell, float *u0, float *v0, float *u1, float *v1)
{
	float s = (float)((cell % ATLAS_COLUMNS) * CELL), t = (float)((cell / ATLAS_COLUMNS) * CELL);

	*u0 = (s + 0.5f) * (1.f / ATLAS_WIDTH);
	*u1 = (s + CELL - 0.5f) * (1.f / ATLAS_WIDTH);
	*v0 = (t + 0.5f) * (1.f / ATLAS_HEIGHT);
	*v1 = (t + CELL - 0.5f) * (1.f / ATLAS_HEIGHT);
}

static void fx_draw_particles(int first_person)
{
	float limit = MAXIMUM_PIXELS * V.k / N64_PIXELS_PER_UNIT;
	int i;

	for (i = 0; i < MAXIMUM_PARTICLES; i++)
	{
		const struct particle *p = &fx.particles[i];
		int fp = p->flags & N64_PARTICLE_FIRST_PERSON;
		float x, y, z, sx, sy, radius, alpha, scale, u0, v0, u1, v1;
		uint32_t r, g, b, argb, oargb = 0;
		int which, cell;

		if (!p->live || fp != (first_person ? N64_PARTICLE_FIRST_PERSON : 0))
			continue;
		if (fp ? p->local_player != V.local_player :
			(p->flags & N64_PARTICLE_THIRD_PERSON_ONLY) && p->local_player == V.local_player)
			continue;
		if (fp)
		{
			x = p->position[0];
			y = p->position[1];
			z = p->position[2];
			if (z < FIRST_PERSON_NEAR * 2.f || z > FIRST_PERSON_FAR)
				continue;
		}
		else
		{
			fx_camera_space(p->position, &x, &y, &z);
			if (z < V.near * 2.f || z > V.far)
				continue;
		}
		/* (half again Halo's size: fewer, larger particles) */
		radius = p->radius * 1.5f * (1.f + (p->end_scale - 1.f) * (p->age / p->life));
		scale = V.k / z;
		sx = V.cx + x * scale;
		sy = V.cy - y * scale;
		radius *= scale;
		if (radius > limit)
			radius = limit;
		if (radius < 0.4f || fx_outside(sx, sy, radius))
			continue;
		/* fading out over its whole life, or (the enhanced effects) by its
		own fade-in and fade-out */
		alpha = 1.f - p->age / p->life;
		if (fx.enhanced)
		{
			alpha = 1.f;
			if (p->fade_in > 0.f && p->age < p->fade_in)
				alpha *= p->age / p->fade_in;
			if (p->fade_out > 0.f && p->life - p->age < p->fade_out)
				alpha *= (p->life - p->age) / p->fade_out;
			if (p->fade_out <= 0.f)
				alpha *= 1.f - p->age / p->life;
		}
		if (!fp)
			alpha *= fx_fog(z);
		alpha *= (p->flags & N64_PARTICLE_ADDITIVE) ? 1.f : (p->color & 0xFF) * (1.f / 255.f);
		if (alpha < 0.02f)
			continue;
		if (alpha > 1.f)
			alpha = 1.f;
		/* the tint times the cell's own colour */
		{
			const uint8_t *hue = fx.cell_colours[p->cell & (CELLS - 1)];

			r = n64_div255((p->color >> 24) * hue[0]);
			g = n64_div255(((p->color >> 16) & 0xFF) * hue[1]);
			b = n64_div255(((p->color >> 8) & 0xFF) * hue[2]);
		}
		argb = (uint32_t)(alpha * 255.f) << 24 | r << 16 | g << 8 | b;
		/* (one clean soft glow for every particle, in its own colour, as on
		the N64: Halo's pictures at 16x16 were noise; testing setting
		"dc_fx_cells": their own) */
		cell = fx.own_cells ? p->cell : 0;
		which = (p->flags & N64_PARTICLE_ADDITIVE) ? H_ADD : H_ALPHA;
		if (p->flags & N64_PARTICLE_FLAME)
		{
			/* the source's cloud-fire shape: a hot core, a warm edge (the
			edge's colour the offset, the core's less it the vertex's) */
			float cool = p->age / p->life;

			cell = fx.flame_cell;
			which = H_FLAME;
			argb = (uint32_t)(alpha * 255.f) << 24 | 55u << 16 | (uint32_t)(210.f - 82.f * cool) << 8 |
				(uint32_t)(190.f - 150.f * cool);
			oargb = 0xFF000000u | 200u << 16 | 32u << 8;
		}
		fx_cell(cell, &u0, &v0, &u1, &v1);
		/* (a hair nearer than the surface it sits on) */
		fx_quad(which, sx - radius, sy - radius, sx + radius, sy + radius,
			(fp ? FIRST_PERSON_Z : 1.f) / (z * 0.995f), u0, v0, u1, v1, argb, oargb);
		fx.drawn++;
	}
}

static void mix(const uint8_t *a, const uint8_t *b, float t, uint8_t *out)
{
	int k;

	for (k = 0; k < 3; k++)
		out[k] = (uint8_t)(a[k] + (b[k] - a[k]) * t);
}

/* the fire's colour as it cools: 0 the flash's, 1 the coolest */
static void fire_colour(const struct explosion_look *look, float t, uint8_t *out)
{
	if (t < 0.2f)
		mix(look->flash, look->hot, t / 0.2f, out);
	else if (t < 0.5f)
		mix(look->hot, look->warm, (t - 0.2f) / 0.3f, out);
	else
		mix(look->warm, look->cool, n64_min_f(1.f, (t - 0.5f) / 0.5f), out);
}

static void fx_draw_explosions(void)
{
	/* (an explosion at the eye fills most of the view, not more) */
	float limit = 480.f * 0.8f;
	int i, part, k;

	/* (testing setting "debug_explosions": one every two seconds ahead of
	the first view, each kind in turn; a little above the eye's height, not
	the N64's a little below: Blood Gulch's slopes hid them) */
	if (V.local_player == 0 && n64_debug_flag("debug_explosions"))
	{
		static uint64_t last;
		static int kind;
		uint64_t now = timer_ms_gettime64();

		if (!last || now - last > 2000)
		{
			float at[3];

			for (k = 0; k < 3; k++)
				at[k] = V.eye[k] + V.f[k] * 6.f + V.u[k] * 0.6f;
			n64_explosion(at, kind);
			kind = (kind + 1) % N64_EXPLOSION_KINDS;
			last = now;
		}
	}
	for (i = 0; i < MAXIMUM_EXPLOSIONS; i++)
	{
		const struct explosion *e = &fx.explosions[i];
		const struct explosion_look *look;
		float r;

		if (!e->live)
			continue;
		look = &explosion_looks[e->kind];
		r = look->radius;
		for (part = 0; part < EXPLOSION_PUFFS; part++)
		{
			/* the puff's own direction, size and turn, from the explosion's seed */
			float d[3] = {hash2(part, 1, e->seed) * 2.f - 1.f, hash2(part, 2, e->seed) * 2.f - 1.f,
				hash2(part, 3, e->seed) * 1.4f - 0.4f};
			float length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-3f;
			float size = 0.75f + 0.5f * hash2(part, 4, e->seed);
			float offset[3] = {0.f, 0.f, 0.f}, at[3], radius, alpha, t;
			float x, y, z, scale, depth, sx, sy, u0, v0, u1, v1;
			uint8_t prim[3], env[3];
			int flip;

			for (k = 0; k < 3; k++)
				d[k] /= length;
			if (part == 0)
			{
				/* the flash */
				if (e->age >= FLASH_TIME)
					continue;
				t = e->age / FLASH_TIME;
				radius = r * 1.7f * (0.6f + 0.4f * t);
				alpha = 0.95f * (1.f - t);
				memcpy(prim, look->flash, 3);
				memcpy(env, look->hot, 3);
			}
			else if (part <= FIRE_PUFFS)
			{
				/* the fireball: puffs swelling outwards and up as they cool */
				t = e->age / FIRE_TIME;
				if (t >= 1.f)
					continue;
				for (k = 0; k < 3; k++)
					offset[k] = d[k] * r * (0.2f + 0.45f * t);
				offset[2] += r * 0.45f * t;
				radius = r * 0.55f * size * (0.5f + 0.75f * sqrtf(t));
				fire_colour(look, t, prim);
				fire_colour(look, n64_min_f(1.f, t + 0.35f), env);
				alpha = t < 0.45f ? 1.f : 1.f - (t - 0.45f) / 0.55f;
			}
			else
			{
				/* the smoke: rising and spreading after the fire */
				t = (e->age - SMOKE_START) / SMOKE_TIME;
				if (t <= 0.f || t >= 1.f || look->smoke_amount <= 0.f)
					continue;
				for (k = 0; k < 3; k++)
					offset[k] = d[k] * r * 0.45f;
				offset[2] += r * (0.4f + 1.4f * t);
				radius = r * 0.6f * size * (0.7f + 0.9f * t);
				for (k = 0; k < 3; k++)
				{
					prim[k] = (uint8_t)n64_min_f(255.f, look->smoke[k] * 1.3f);
					env[k] = (uint8_t)(look->smoke[k] * 0.75f);
				}
				alpha = look->smoke_amount * 0.6f * (t < 0.15f ? t / 0.15f : 1.f - (t - 0.15f) / 0.85f);
			}
			for (k = 0; k < 3; k++)
				at[k] = e->position[k] + offset[k];
			fx_camera_space(at, &x, &y, &z);
			if (z < V.near * 2.f || z > V.far)
				continue;
			scale = V.k / z;
			radius *= scale;
			if (radius > limit)
				radius = limit;
			sx = V.cx + x * scale;
			sy = V.cy - y * scale;
			if (radius < 0.5f || fx_outside(sx, sy, radius))
				continue;
			alpha *= fx_fog(z);
			if (alpha < 0.02f)
				continue;
			/* (its depth a little nearer than its middle: the ground it sits
			on does not cut it in half) */
			depth = n64_max_f(z - radius / scale * 0.6f, V.near * 2.f);
			/* the turn: the puff mirrored either way */
			flip = (int)(hash2(part, 5, e->seed) * 4.f) & 3;
			u0 = flip & 1 ? 1.f : 0.f;
			u1 = 1.f - u0;
			v0 = flip & 2 ? 1.f : 0.f;
			v1 = 1.f - v0;
			{
				uint32_t a = (uint32_t)(n64_min_f(alpha, 1.f) * 255.f);
				uint32_t base = a << 24, offset_colour = 0xFF000000u;
				int c;

				for (c = 0; c < 3; c++)
				{
					int difference = prim[c] - env[c];

					base |= (uint32_t)(difference > 0 ? difference : 0) << (16 - 8 * c);
					offset_colour |= (uint32_t)env[c] << (16 - 8 * c);
				}
				fx_quad(H_FIRE, sx - radius, sy - radius, sx + radius, sy + radius, 1.f / depth, u0, v0, u1, v1, base,
					offset_colour);
			}
		}
	}
}

/* a line in the world as a thin quad across the screen: clipped to the
near plane and the far, its half-widths in pixels at either end (each
at least min_pixels), the ribbon's texture along it */
static void fx_line(int which, const float *start, const float *end, float half_width, float min_pixels,
	float max_pixels, uint32_t argb)
{
	float a[3], b[3], sa[2], sb[2], wa, wb, width, dx, dy, length, nx, ny, near = V.near * 1.01f;
	int side, k;

	fx_camera_space(start, &a[0], &a[1], &a[2]);
	fx_camera_space(end, &b[0], &b[1], &b[2]);
	for (side = 0; side < 2; side++)
	{
		float da = side ? V.far - a[2] : a[2] - near;
		float db = side ? V.far - b[2] : b[2] - near;

		if (da < 0.f && db < 0.f)
			return;
		if ((da < 0.f) != (db < 0.f))
		{
			float t = da / (da - db), point[3];

			for (k = 0; k < 3; k++)
				point[k] = a[k] + (b[k] - a[k]) * t;
			memcpy(da < 0.f ? a : b, point, sizeof(point));
		}
	}
	sa[0] = V.cx + V.k * a[0] / a[2];
	sa[1] = V.cy - V.k * a[1] / a[2];
	sb[0] = V.cx + V.k * b[0] / b[2];
	sb[1] = V.cy - V.k * b[1] / b[2];
	dx = sb[0] - sa[0];
	dy = sb[1] - sa[1];
	length = sqrtf(dx * dx + dy * dy);
	/* (looking along the shot: no glowing dot) */
	if (!isfinite(length) || length < 0.8f)
		return;
	nx = -dy / length;
	ny = dx / length;
	wa = n64_min_f(max_pixels, n64_max_f(min_pixels, half_width * V.k / a[2]));
	wb = n64_min_f(max_pixels, n64_max_f(min_pixels, half_width * V.k / b[2]));
	/* (its box, with its widths, wholly outside the view: not sent) */
	width = n64_max_f(wa, wb);
	if (n64_max_f(sa[0], sb[0]) + width < V.x0 || n64_min_f(sa[0], sb[0]) - width > V.x1 ||
		n64_max_f(sa[1], sb[1]) + width < V.y0 || n64_min_f(sa[1], sb[1]) - width > V.y1)
		return;
	if (!dc_stage_room(5))
		return;
	fx_use(which);
	fx_vertex(sa[0] - nx * wa, sa[1] - ny * wa, 1.f / a[2], 0.f, 0.f, argb, 0, 0);
	fx_vertex(sb[0] - nx * wb, sb[1] - ny * wb, 1.f / b[2], 1.f, 0.f, argb, 0, 0);
	fx_vertex(sa[0] + nx * wa, sa[1] + ny * wa, 1.f / a[2], 0.f, 1.f, argb, 0, 0);
	fx_vertex(sb[0] + nx * wb, sb[1] + ny * wb, 1.f / b[2], 1.f, 1.f, argb, 0, 1);
	fx.quads++;
}

/* the sniper's ribbons (pale vapour, much quieter than the impacts: the
N64's), the projectiles' trails and glows */
static void fx_draw_shots(void)
{
	int i;

	for (i = 0; i < N64_SNIPER_RIBBONS; i++)
	{
		float age = fx.ribbons[i].age, alpha, x, y, z;

		if (age >= N64_SNIPER_LIFE)
			continue;
		alpha = 1.f - age / N64_SNIPER_LIFE;
		fx_camera_space(fx.ribbons[i].end, &x, &y, &z);
		alpha *= fx_fog(z);
		/* (the N64's width: 0.009 of its pixels a unit, 0.35 to 1.1 of its
		pixels; twice those here) */
		fx_line(H_RIBBON, fx.ribbons[i].start, fx.ribbons[i].end, 0.009f, 0.7f, 2.2f,
			(uint32_t)(alpha * alpha * 76.f) << 24 | 205u << 16 | 225u << 8 | 235u);
	}
	for (i = 0; i < TRAILS; i++)
	{
		const struct trail *t = &fx.trails[i];
		float alpha, x, y, z;

		if (t->age >= TRAIL_LIFE)
			continue;
		fx_camera_space(t->end, &x, &y, &z);
		alpha = (1.f - t->age / TRAIL_LIFE) * fx_fog(z) * (float)(t->argb >> 24) * (1.f / 255.f);
		if (alpha < 0.02f)
			continue;
		fx_line(t->additive ? H_RIBBON_ADD : H_RIBBON, t->start, t->end, t->width * 0.5f, 0.6f, 6.f,
			(uint32_t)(alpha * 255.f) << 24 | (t->argb & 0xFFFFFF));
	}
	for (i = 0; i < GLOWS; i++)
	{
		const struct glow *g = &fx.glows[i];
		float x, y, z, scale, radius, core, sx, sy, alpha, u0, v0, u1, v1;
		uint32_t a;

		if (g->age >= GLOW_LIFE)
			continue;
		fx_camera_space(g->position, &x, &y, &z);
		if (z < V.near * 2.f || z > V.far)
			continue;
		alpha = fx_fog(z);
		if (alpha < 0.02f)
			continue;
		scale = V.k / z;
		radius = n64_min_f(48.f, n64_max_f(1.5f, g->radius * scale));
		sx = V.cx + x * scale;
		sy = V.cy - y * scale;
		if (fx_outside(sx, sy, radius))
			continue;
		fx_cell(0, &u0, &v0, &u1, &v1);
		a = (uint32_t)(alpha * 255.f) << 24;
		/* the bolt's colour, and a white-hot middle */
		fx_quad(H_ADD, sx - radius, sy - radius, sx + radius, sy + radius, 1.f / n64_max_f(z - g->radius, V.near * 2.f),
			u0, v0, u1, v1, a | (g->argb & 0xFFFFFF), 0);
		core = radius * 0.35f;
		fx_quad(H_ADD, sx - core, sy - core, sx + core, sy + core, 1.f / n64_max_f(z - g->radius, V.near * 2.f),
			u0, v0, u1, v1, a | 0xFFFFFF, 0);
		fx.drawn++;
	}
}

/* the view's 3D done (n64_render_game.c, after the first-person weapon):
the world's particles, explosions and shots, then the first-person ones */
void n64_render_view_particles(void)
{
	const struct n64_view *view = dc_render_view();
	uint64_t start;
	int k;

	if (fx.disabled || !view || !dc_render_frame_open() || !fx.fireball)
		return;
	start = timer_us_gettime64();
	for (k = 0; k < 3; k++)
	{
		V.eye[k] = dc_cam.eye[k];
		V.r[k] = dc_cam.r[k];
		V.u[k] = dc_cam.u[k];
		V.f[k] = dc_cam.f[k];
	}
	V.k = dc_cam.k;
	V.cx = dc_cam.cx;
	V.cy = dc_cam.cy;
	V.near = view->z_near;
	V.x0 = view->x0;
	V.y0 = view->y0;
	V.x1 = view->x1;
	V.y1 = view->y1;
	dc_render_fog_range(&V.fog_start, &V.fog_end);
	V.far = V.fog_end > 0.f && V.fog_end < view->z_far ? V.fog_end : view->z_far;
	V.local_player = view->local_player_index;
	dc_use_list(PVR_LIST_TR_POLY);
	fx.current = NULL;
	fx_draw_shots();
	fx_draw_explosions();
	if (fx.atlas_white && fx.live)
	{
		fx_draw_particles(0);
		/* (a first-person particle with its weapon: none in the chase view) */
		if (dc_first_person)
			fx_draw_particles(1);
	}
	dc_use_list(PVR_LIST_OP_POLY);
	fx.us += (unsigned long)(timer_us_gettime64() - start);
	fx.frames_drawn++;
}

/* the "dcx:" line (dc_render.c, every 120 frames): the pool, what was made
and refused, and drawn a frame */
void dc_fx_statistics_line(void)
{
	char line[200];
	unsigned long frames = fx.frames_drawn ? fx.frames_drawn : 1;
	int explosions = 0, trails = 0, glows = 0, ribbons = 0, i;

	for (i = 0; i < MAXIMUM_EXPLOSIONS; i++)
	{
		explosions += fx.explosions[i].live;
	}
	for (i = 0; i < TRAILS; i++)
		trails += fx.trails[i].age < TRAIL_LIFE;
	for (i = 0; i < GLOWS; i++)
		glows += fx.glows[i].age < GLOW_LIFE;
	for (i = 0; i < N64_SNIPER_RIBBONS; i++)
		ribbons += fx.ribbons[i].age < N64_SNIPER_LIFE;
	snprintf(line, sizeof(line),
		"dcx: particles %d live, %lu made, %lu refused, %lu drawn and %lu quads a frame; explosions %d, trails %d,"
		" glows %d, ribbons %d; %lu us a frame",
		fx.live, fx.spawned, fx.refused, fx.drawn / frames, fx.quads / frames, explosions, trails, glows, ribbons,
		fx.us / frames);
	n64_log(line);
	fx.spawned = fx.refused = fx.drawn = fx.quads = fx.us = fx.frames_drawn = 0;
}

void n64_particles_statistics(unsigned long *spawned, unsigned long *drawn, unsigned long *reads, int *live)
{
	*spawned = fx.spawned;
	*drawn = fx.drawn;
	*reads = 0;
	*live = fx.live;
}

void n64_particles_set_view_count(int views)
{
	fx.budget = views > 2 ? 0.5f : 1.f;
}

void n64_particles_set_enabled(int on)
{
	fx.disabled = !on;
}

void n64_particles_set_enhanced(int on)
{
	fx.enhanced = !!on;
}

int n64_particles_enhanced(void)
{
	return fx.enhanced;
}

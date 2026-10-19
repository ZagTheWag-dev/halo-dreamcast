/*
DC_SHADOW.C

Units' and vehicles' shadows (n64.h n64_render_shadow): Halo's dynamic object
shadows, which the Xbox rendered from each object's own lighting and the N64
never had, as a soft dark oval on the ground. The game side
(port/n64/game/n64_render_game.c, HALO_DC) finds the ground under each unit
and vehicle with a ray down through the structure, once a frame, and gives
the ground's point and plane, the object's forward, the oval's size and its
darkness; here the oval is two triangles on that plane, lifted a hair off it
(SHADOW_LIFT), on the translucent list: a 64x64 radial falloff in the HUD's
8-bit coverage palette (bank 0, dc_hud.c: entry v is (v, v, v, v)) times a
black vertex colour, blended by alpha (SRCALPHA, INVSRCALPHA), depth-tested
against the level and the models, never writing depth, so the PowerVR's
sort lays it over the ground and under whoever stands on it. It takes the
fog as the particles do (dc_fx.c): no PowerVR fog (that would add the fog's
colour to it), its darkness fading out by its distance into the fog
instead. Crossing the near plane (the viewer's own shadow, looked straight
down at) it goes through the near-plane clip (dc_render.c).

Ground that bends under the oval is not followed: where it rises the oval
goes under it, where it falls away the oval is a little off it; an oval a
unit's size on Blood Gulch's slopes does not show it. A Warthog on a ridge
can. The "dcr: objects" line counts the shadows drawn a frame and their
time here (the game side's rays are in the frame's draw time, "dcf:").
*/

#include <malloc.h>
#include <math.h>
#include <string.h>

#include "dc_render.h"

#define SHADOW_SIZE 64
#define SHADOW_LIFT 0.04f
/* (the HUD's coverage palette, dc_hud.c PALETTE_COVERAGE) */
#define SHADOW_PALETTE 0

static struct
{
	pvr_ptr_t texture;
	pvr_poly_hdr_t header __attribute__((aligned(32)));
} S;

/* a texel's place in a square twiddled texture: x's and y's bits
interleaved (the falloff is round, so which takes the odd bits does not
matter) */
static int twiddled(int x, int y)
{
	int i, out = 0;

	for (i = 0; i < 8; i++)
		out |= ((x >> i) & 1) << (2 * i + 1) | ((y >> i) & 1) << (2 * i);
	return out;
}

/* the falloff to video memory and the header, as the PowerVR comes up
(n64_render_initialize, after the HUD's palettes): kept for good, 4 KB */
void dc_shadow_reserve(void)
{
	static uint8_t texels[SHADOW_SIZE * SHADOW_SIZE] __attribute__((aligned(32)));
	pvr_poly_cxt_t c;
	int x, y;

	if (S.texture || !dc_pvr_up)
		return;
	for (y = 0; y < SHADOW_SIZE; y++)
	{
		for (x = 0; x < SHADOW_SIZE; x++)
		{
			/* dark in the middle third, smoothstep down to nothing a texel in
			from the edge (the clamp's border stays clear) */
			float dx = (x + 0.5f) / (SHADOW_SIZE / 2) - 1.f, dy = (y + 0.5f) / (SHADOW_SIZE / 2) - 1.f;
			float r = sqrtf(dx * dx + dy * dy) * (SHADOW_SIZE / 2) / (SHADOW_SIZE / 2 - 1.5f);
			float s = (1.f - r) / 0.65f;

			s = s < 0.f ? 0.f : s > 1.f ? 1.f : s;
			texels[twiddled(x, y)] = (uint8_t)(s * s * (3.f - 2.f * s) * 255.f + 0.5f);
		}
	}
	S.texture = pvr_mem_malloc(sizeof(texels));
	if (!S.texture)
		return;
	pvr_txr_load(texels, S.texture, sizeof(texels));
	pvr_poly_cxt_txr(&c, PVR_LIST_TR_POLY,
		PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(SHADOW_PALETTE) | PVR_TXRFMT_TWIDDLED, SHADOW_SIZE, SHADOW_SIZE,
		S.texture, PVR_FILTER_BILINEAR);
	c.txr.env = PVR_TXRENV_MODULATEALPHA;
	c.txr.uv_clamp = PVR_UVCLAMP_UV;
	c.gen.shading = PVR_SHADE_FLAT;
	c.gen.culling = PVR_CULLING_NONE;
	c.gen.fog_type = PVR_FOG_DISABLE;
	c.depth.comparison = PVR_DEPTHCMP_GEQUAL;
	c.depth.write = PVR_DEPTHWRITE_DISABLE;
	c.blend.src = PVR_BLEND_SRCALPHA;
	c.blend.dst = PVR_BLEND_INVSRCALPHA;
	pvr_poly_compile(&S.header, &c);
}

static inline float dot(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void n64_render_shadow(const struct n64_shadow *shadow)
{
	const float *n = shadow->normal;
	float f[3], l[3], centre[3], length, alpha, fog_start, fog_end, depth;
	dc_cv corners[4];
	const dc_cv *strip[4];
	uint32_t argb;
	uint64_t start;
	int i, k;

	if (!S.texture || !dc_render_frame_open() || dc_first_person || shadow->alpha <= 0.f)
		return;
	start = timer_us_gettime64();
	for (k = 0; k < 3; k++)
		centre[k] = shadow->point[k] + n[k] * SHADOW_LIFT;
	/* by its distance into the fog (dc_fx.c fx_fog) */
	alpha = shadow->alpha;
	dc_render_fog_range(&fog_start, &fog_end);
	depth = (centre[0] - dc_cam.eye[0]) * dc_cam.f[0] + (centre[1] - dc_cam.eye[1]) * dc_cam.f[1] +
		(centre[2] - dc_cam.eye[2]) * dc_cam.f[2];
	if (fog_end > 0.f && depth > fog_start)
		alpha *= depth >= fog_end ? 0.f : 1.f - (depth - fog_start) / (fog_end - fog_start);
	if (alpha < 1.f / 64.f ||
		dc_sphere_test(&dc_cam, centre, sqrtf(shadow->length * shadow->length + shadow->width * shadow->width)) == DC_OUT)
		goto done;
	/* the oval's axes on the ground: the forward less its part along the
	normal (straight up or down: any line across) and the normal across it */
	length = dot(shadow->forward, n);
	for (k = 0; k < 3; k++)
		f[k] = shadow->forward[k] - n[k] * length;
	length = dot(f, f);
	if (length < 1e-6f)
	{
		f[0] = n[2];
		f[1] = 0.f;
		f[2] = -n[0];
		length = dot(f, f);
		if (length < 1e-6f)
		{
			f[0] = 1.f;
			f[2] = 0.f;
			length = 1.f;
		}
	}
	length = shadow->length / sqrtf(length);
	for (k = 0; k < 3; k++)
		f[k] *= length;
	l[0] = n[1] * f[2] - n[2] * f[1];
	l[1] = n[2] * f[0] - n[0] * f[2];
	l[2] = n[0] * f[1] - n[1] * f[0];
	length = shadow->width / sqrtf(dot(l, l) > 1e-12f ? dot(l, l) : 1e-12f);
	for (k = 0; k < 3; k++)
		l[k] *= length;
	argb = (uint32_t)(alpha * 255.f + 0.5f) << 24;
	mat_load(&dc_cam.m);
	for (i = 0; i < 4; i++)
	{
		/* the strip's corners: back left, back right, front left, front right */
		float sf = (i & 2) ? 1.f : -1.f, sl = (i & 1) ? -1.f : 1.f;
		float x = centre[0] + f[0] * sf + l[0] * sl, y = centre[1] + f[1] * sf + l[1] * sl;
		float z = centre[2] + f[2] * sf + l[2] * sl, w = 1.f;

		mat_trans_nodiv(x, y, z, w);
		corners[i].x = x;
		corners[i].y = y;
		corners[i].w = w;
		corners[i].u = (i & 1) ? 1.f : 0.f;
		corners[i].v = (i & 2) ? 1.f : 0.f;
		corners[i].argb = argb;
		strip[i] = &corners[i];
	}
	dc_use_list(PVR_LIST_TR_POLY);
	if (dc_stage_room(4 * 2 + 1))
	{
		dc_send_header(&S.header);
		dc_clip_strip(strip, 4);
		dc_stat.shadows++;
	}
	dc_use_list(PVR_LIST_OP_POLY);
done:
	dc_stat.shadow_us += (unsigned long)(timer_us_gettime64() - start);
}

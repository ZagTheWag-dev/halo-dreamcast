/*
DC_RENDER.C

The Dreamcast's renderer (the n64.h renderer API of port/n64/src/n64_render.c,
over KallistiOS's PowerVR): the frame (one scene a frame, the opaque list
written straight to the tile accelerator through the store queues), each
view's camera, the fog (the PowerVR's fog table, linear from the fog's start
to its end), the frustum test, and the near-plane clip the PowerVR does not
do. The level itself is dc_level.c.

The camera matrix takes a world position p to (X, Y, Z, W) = (sx W, sy W,
W, W), W the depth ahead of the eye; KallistiOS's ftrv-and-divide gives
(sx, sy, 1/W), a PowerVR vertex (its depth test: GREATER, the nearer the
larger). sx = cx + k (r.q)/d and sy = cy - k (u.q)/d, q = p - eye,
d = f.q: n64_render.c's projection (pixels_per_unit k).
*/

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dc_render.h"

int dc_pvr_up;
pvr_dr_state_t dc_dr;
dc_camera_t dc_cam;
struct dc_statistics dc_stat;
/* the level's back faces (level.py: Halo's triangles turned to be
counter-clockwise from the front in a y-up view; the screen's y is down) */
int dc_cull = PVR_CULLING_CW;
float dc_zscale = 1.0f;
int dc_first_person;

/* the punch-through and translucent lists' vertices, staged */
#define DC_STAGE_VERTICES 8192
static pvr_vertex_t dc_pt_stage[DC_STAGE_VERTICES] __attribute__((aligned(32)));
static pvr_vertex_t dc_tr_stage[DC_STAGE_VERTICES] __attribute__((aligned(32)));
static int dc_pt_count, dc_tr_count;
pvr_vertex_t *dc_stage;
int *dc_stage_count;
int dc_stage_maximum = DC_STAGE_VERTICES;
pvr_vertex_t dc_stage_overflow;
static struct n64_view dc_last_view;

static int dc_frame_open, dc_op_open, dc_views;
static unsigned long dc_frames;
static int dc_view_count = 1;

static struct
{
	unsigned long rgb;
	float start, end;
	int dirty;
} dc_fog = {0x60708C, 0.0f, 0.0f, 1};

/* (testing setting "debug_no_fog") */
static int dc_no_fog;
int dc_no_detail;
/* testing setting "dc_no_sky": no sky model about the eye */
static int dc_no_sky;

/* The tile accelerator's vertex buffer: every list's polygons and vertices
of a frame are stored in it (a 32-byte packet sent is at most 32 bytes
stored), DC_VERTEX_BUFFER of video memory, twice (one frame stored while
the last renders: KallistiOS makes two whatever vbuf_doublebuf_disabled
says). A frame must never send more than it holds: on a
Dreamcast the excess runs over into the video memory beyond it and the
PowerVR stops (a frozen picture, the game still running: Flycast ignores
it). dc_stage_room keeps the frame's packets, every list's, inside
DC_TA_BUDGET: a batch that would not fit is left out whole (a strip cut
short is an error of its own), the HUD's (sent at the frame's end) given
DC_HUD_RESERVE of it beforehand. */
#define DC_VERTEX_BUFFER (1536 * 1024)
#define DC_HUD_RESERVE 2048
#define DC_TA_BUDGET ((int)(DC_VERTEX_BUFFER / 32 * 9 / 10) - DC_HUD_RESERVE)
int dc_op_packets;
static int dc_packets_peak, dc_packets_refused, dc_peak_op, dc_peak_pt, dc_peak_tr;
/* pvr_wait_ready's timeouts in a row: the PowerVR stopped (the screen
says so: n64_fatal) */
static int dc_render_timeouts;

/* the PowerVR's lists: opaque and translucent (16-word bins), the punch-
through list for R2's alpha-tested textures; a 512 KB vertex buffer (the
level's worst frame is about 180 KB) */
static pvr_init_params_t dc_pvr_params = {
	.opb_sizes = {PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16},
	.vertex_buf_size = DC_VERTEX_BUFFER,
	.dma_enabled = 0,
	.fsaa_enabled = 0,
	.autosort_disabled = 0,
	.opb_overflow_count = 3,
};

/* the map's sky model (-1 none) and the fog's greatest density
(n64.h dc_render_set_sky_model, dc_render_set_haze) */
static short dc_sky_model = -1;
static float dc_haze = 1.0f;

void dc_render_set_sky_model(short model_tag_index)
{
	dc_sky_model = model_tag_index;
}

void dc_render_set_haze(float maximum_density)
{
	maximum_density = maximum_density < 0.05f ? 0.05f : maximum_density > 1.0f ? 1.0f : maximum_density;
	if (maximum_density != dc_haze)
	{
		dc_haze = maximum_density;
		dc_fog.dirty = 1;
	}
}

int dc_material_list(int flags)
{
	if (flags & (MATERIAL_GLOW | MATERIAL_BLEND | MATERIAL_TRANSPARENT | MATERIAL_WATER))
		return PVR_LIST_TR_POLY;
	if (flags & MATERIAL_ALPHA_TEST)
		return PVR_LIST_PT_POLY;
	return PVR_LIST_OP_POLY;
}

/* a polygon header: Gouraud, the fog's table, the depth test; textured
(modulated by the vertices' colour, bilinear, mipmapped, repeating) or
not; on the translucent list blended (a glow added, the rest by alpha) and
not writing depth */
void dc_header_compile_list(pvr_poly_hdr_t *header, int list, pvr_ptr_t texture, int format, int size, int flags)
{
	pvr_poly_cxt_t c;

	if (texture)
	{
		pvr_poly_cxt_txr(&c, list, format | PVR_TXRFMT_TWIDDLED, size, size, texture, PVR_FILTER_BILINEAR);
		c.txr.mipmap = PVR_MIPMAP_ENABLE;
		c.txr.env = list == PVR_LIST_OP_POLY ? PVR_TXRENV_MODULATE : PVR_TXRENV_MODULATEALPHA;
		c.txr.uv_clamp = PVR_UVCLAMP_NONE;
	}
	else
		pvr_poly_cxt_col(&c, list);
	c.gen.shading = PVR_SHADE_GOURAUD;
	c.gen.fog_type = dc_fog.end > 0.0f && !dc_no_fog ? PVR_FOG_TABLE : PVR_FOG_DISABLE;
	c.gen.culling = (flags & MATERIAL_TWO_SIDED) ? PVR_CULLING_NONE : dc_cull;
	c.depth.comparison = (flags & DC_HEADER_DEPTH_EQUAL) ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_GREATER;
	c.depth.write = list == PVR_LIST_TR_POLY ? PVR_DEPTHWRITE_DISABLE : PVR_DEPTHWRITE_ENABLE;
	/* the menu's sky (tools/n64/ui_dc.py): behind everything, drawn first,
	no depth, no fog */
	if (flags & MATERIAL_SKY)
	{
		/* (a sky's translucent layers -- clouds, planets -- far out about
		the eye, behind the level by the depth test: the translucent list
		comes after it) */
		c.depth.comparison = list == PVR_LIST_TR_POLY ? PVR_DEPTHCMP_GREATER : PVR_DEPTHCMP_ALWAYS;
		c.depth.write = PVR_DEPTHWRITE_DISABLE;
		c.gen.fog_type = PVR_FOG_DISABLE;
	}
	if (list == PVR_LIST_TR_POLY && (flags & MATERIAL_GLOW) && !(flags & MATERIAL_BLEND))
	{
		c.blend.src = PVR_BLEND_ONE;
		c.blend.dst = PVR_BLEND_ONE;
		c.gen.fog_type = PVR_FOG_DISABLE;
	}
	pvr_poly_compile(header, &c);
}

void dc_header_compile(pvr_poly_hdr_t *header, pvr_ptr_t texture, int format, int size, int two_sided)
{
	dc_header_compile_list(header, PVR_LIST_OP_POLY, texture, format, size, two_sided ? MATERIAL_TWO_SIDED : 0);
}

void dc_use_list(int list)
{
	if (list == PVR_LIST_PT_POLY)
	{
		dc_stage = dc_pt_stage;
		dc_stage_count = &dc_pt_count;
	}
	else if (list == PVR_LIST_TR_POLY)
	{
		dc_stage = dc_tr_stage;
		dc_stage_count = &dc_tr_count;
	}
	else
		dc_stage = NULL;
}

int dc_stage_room(int count)
{
	if (dc_op_packets + dc_pt_count + dc_tr_count + count > DC_TA_BUDGET)
	{
		dc_packets_refused++;
		return 0;
	}
	return !dc_stage || *dc_stage_count + count <= dc_stage_maximum;
}

static void dc_compile_headers(void)
{
	dc_level_compile_headers();
	dc_model_compile_headers();
}

/* ---------- lifecycle */

void n64_render_initialize(void)
{
	extern int dc_screen_log;
	char line[96];

	if (dc_pvr_up)
		return;
	/* (the log leaves the screen: the PowerVR takes the framebuffer) */
	dc_screen_log = 0;
	if (pvr_init(&dc_pvr_params))
		n64_fatal("pvr_init failed");
	dc_pvr_up = 1;
	/* the punch-through list's alpha reference (PT_ALPHA_REF, 0x011C): a
	texel half opaque or more is drawn (KallistiOS leaves it 0) */
	PVR_SET(0x011C, 0x80);
	dc_no_fog = n64_debug_flag("debug_no_fog");
	dc_no_detail = n64_debug_flag("dc_no_detail");
	dc_no_sky = n64_debug_flag("dc_no_sky");
	dc_compile_headers();
	dc_pace_initialize();
	/* (what is kept for good first, at the bottom of video memory: the
	HUD's palettes, blip and meter slots, the effects' puff and ribbon) */
	dc_hud_reserve();
	n64_particles_reserve();
	dc_shadow_reserve();
	pvr_set_bg_color(((dc_fog.rgb >> 16) & 255) / 255.0f, ((dc_fog.rgb >> 8) & 255) / 255.0f,
		(dc_fog.rgb & 255) / 255.0f);
	snprintf(line, sizeof(line), "dcr: PowerVR up, %lu KB of video memory for textures",
		(unsigned long)pvr_mem_available() / 1024);
	n64_log(line);
}

int n64_render_screen_width(void) { return 640; }
int n64_render_screen_height(void) { return 480; }

void n64_render_set_view_count(int count)
{
	extern int n64_view_total;

	n64_view_total = count;
	dc_view_count = count > 0 ? count : 1;
	/* (more views: fewer particles made, dc_fx.c) */
	n64_particles_set_view_count(dc_view_count);
}

/* ---------- fog

The PowerVR's table fog, KallistiOS's linear table from the fog's start to
its end (pvr_fog.c); fully the fog's colour at its end, which is the clear
colour: past it nothing is seen anyway. */

void n64_render_set_fog(unsigned long rgb, float start, float end)
{
	if (rgb != dc_fog.rgb || start != dc_fog.start || end != dc_fog.end)
	{
		dc_fog.rgb = rgb;
		dc_fog.start = start;
		dc_fog.end = end;
		dc_fog.dirty = 1;
	}
}

float n64_render_visible_range(void)
{
	return dc_fog.end > 0.0f ? dc_fog.end : 200.0f;
}

void n64_render_set_sky(const unsigned long *colors)
{
	(void)colors;
}

void n64_render_set_clear_color(unsigned long rgb)
{
	(void)rgb;
}

static void dc_fog_apply(void)
{
	float r = ((dc_fog.rgb >> 16) & 255) / 255.0f;
	float g = ((dc_fog.rgb >> 8) & 255) / 255.0f;
	float b = (dc_fog.rgb & 255) / 255.0f;

	pvr_wait_render_done();
	pvr_set_bg_color(r, g, b);
	if (dc_fog.end > 0.0f)
	{
		/* (the table's densities times the colour's alpha: Halo's haze) */
		pvr_fog_table_color(dc_haze, r, g, b);
		pvr_fog_table_linear(dc_fog.start, dc_fog.end);
		{
			char line[96];

			snprintf(line, sizeof(line), "dcr: fog %06lx from %.1f to %.1f", dc_fog.rgb, dc_fog.start, dc_fog.end);
			n64_log(line);
		}
	}
	dc_compile_headers();
	dc_fog.dirty = 0;
}

/* ---------- frames */

static void dc_statistics_line(void)
{
	{
		char budget[120];

		snprintf(budget, sizeof(budget), "dcr: tile accelerator packets, frame peak %d of %d (opaque %d, punch-through %d,"
			" translucent %d), %d batches left out", dc_packets_peak, DC_TA_BUDGET, dc_peak_op, dc_peak_pt, dc_peak_tr,
			dc_packets_refused);
		n64_log(budget);
		dc_packets_peak = dc_packets_refused = 0;
	}
	static uint64_t last;
	char line[200];
	unsigned long frames = 120;
	uint64_t now = timer_us_gettime64();
	unsigned long fps100 = last && now > last ? (unsigned long)(frames * 100000000ull / (now - last)) : 0;

	/* (the frames a second measured over the 120: pvr_get_stats's
	frame_rate is the last frame's alone) */
	last = now;
	snprintf(line, sizeof(line),
		"dcr: %lu frames, %lu.%02lu fps, groups %lu (%lu PVS-culled), runs %lu (%lu clipped), vertices %lu,"
		" strip vertices %lu (detail %lu), level %lu us average %lu worst",
		dc_frames, fps100 / 100, fps100 % 100, dc_stat.groups / frames, dc_stat.groups_pvs / frames,
		dc_stat.runs / frames, dc_stat.runs_clipped / frames, dc_stat.vertices / frames,
		dc_stat.strip_vertices / frames, dc_stat.detail_vertices / frames, dc_stat.level_us / frames,
		dc_stat.level_us_worst);
	n64_log(line);
	snprintf(line, sizeof(line), "dcr: objects %lu (%lu culled, %lu far), %lu triangles, %lu us average;"
		" first person %lu triangles, %lu us; shadows %lu.%lu, %lu us",
		dc_stat.objects / frames, dc_stat.objects_culled / frames, dc_stat.objects_far / frames,
		dc_stat.model_triangles / frames, dc_stat.model_us / frames, dc_stat.first_person_triangles / frames,
		dc_stat.first_person_us / frames, dc_stat.shadows * 10 / frames / 10,
		dc_stat.shadows * 10 / frames % 10, dc_stat.shadow_us / frames);
	n64_log(line);
	memset(&dc_stat, 0, sizeof(dc_stat));
	dc_fx_statistics_line();
	dc_sound_statistics_line();
}

void n64_render_frame_begin(void)
{
	if (!dc_pvr_up || dc_frame_open)
		return;
	dc_pace_frame_begin();
	if (pvr_wait_ready() < 0)
	{
		/* (100 ms a timeout: 3 s of a stopped PowerVR) */
		if (++dc_render_timeouts == 30)
		{
			char line[160];

			snprintf(line, sizeof(line), "the PowerVR stopped rendering (frame packets peak %d of %d, %d batches left "
				"out)", dc_packets_peak, DC_TA_BUDGET, dc_packets_refused);
			n64_fatal(line);
		}
	}
	else
		dc_render_timeouts = 0;
	dc_pace_frame_ready();
	if (dc_fog.dirty)
		dc_fog_apply();
	pvr_scene_begin();
	dc_frame_open = 1;
	dc_op_open = 0;
	dc_views = 0;
	dc_pt_count = dc_tr_count = 0;
	dc_op_packets = 0;
	dc_stage = NULL;
}

static void dc_op_begin(void)
{
	if (!dc_op_open)
	{
		pvr_list_begin(PVR_LIST_OP_POLY);
		pvr_dr_init(&dc_dr);
		dc_op_open = 1;
	}
}

void n64_render_frame_end(void)
{
	if (!dc_frame_open)
		return;
	dc_pace_frame_drawn();
	if (dc_op_open)
		pvr_list_finish();
	dc_stage = NULL;
	/* the HUD's meters made this frame, between the lists (dc_hud.c) */
	dc_hud_upload();
	if (dc_pt_count)
	{
		pvr_list_begin(PVR_LIST_PT_POLY);
		pvr_prim(dc_pt_stage, dc_pt_count * sizeof(pvr_vertex_t));
		pvr_list_finish();
	}
	if (dc_tr_count || dc_hud_pending())
	{
		pvr_list_begin(PVR_LIST_TR_POLY);
		if (dc_tr_count)
			pvr_prim(dc_tr_stage, dc_tr_count * sizeof(pvr_vertex_t));
		/* the HUD over everything (dc_hud.c: 1/w past the world's and the
		first-person models', in the order it was drawn) */
		dc_hud_submit();
		pvr_list_finish();
	}
	pvr_scene_finish();
	if (dc_op_packets + dc_pt_count + dc_tr_count > dc_packets_peak)
	{
		dc_packets_peak = dc_op_packets + dc_pt_count + dc_tr_count;
		dc_peak_op = dc_op_packets;
		dc_peak_pt = dc_pt_count;
		dc_peak_tr = dc_tr_count;
	}
	dc_hud_frame_end();
	dc_frame_open = 0;
	dc_pace_frame_end();
	/* the voices' ends, the waits for the disc (dc_sound.c), as the N64
	feeds its mixer here */
	n64_sound_poll();
	if (dc_views && ++dc_frames % 120 == 0)
		dc_statistics_line();
}

void n64_render_flush(void)
{
}

const struct n64_view *dc_render_view(void)
{
	return dc_views ? &dc_last_view : NULL;
}

int dc_render_frame_open(void)
{
	return dc_frame_open;
}

void dc_render_fog_range(float *start, float *end)
{
	int on = dc_fog.end > 0.0f && !dc_no_fog;

	*start = on ? dc_fog.start : 0.0f;
	*end = on ? dc_fog.end : 0.0f;
}

/* ---------- the camera */

static void norm3(float *o, const float *a)
{
	float l = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);

	l = l > 0.0f ? 1.0f / l : 0.0f;
	o[0] = a[0] * l;
	o[1] = a[1] * l;
	o[2] = a[2] * l;
}

static void cross3(float *o, const float *a, const float *b)
{
	float x = a[1] * b[2] - a[2] * b[1];
	float y = a[2] * b[0] - a[0] * b[2];
	float z = a[0] * b[1] - a[1] * b[0];

	o[0] = x;
	o[1] = y;
	o[2] = z;
}

static float dot3(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void dc_camera_build(dc_camera_t *c, const struct n64_view *v, float near, float far)
{
	float f[3], r[3], u[3], R0[3], R1[3];
	const float *P = v->position;
	float w = (float)(v->x1 - v->x0), h = (float)(v->y1 - v->y0);
	float k, cx, cy, tw, th;
	int i, s;

	/* forward, right (forward x up: Halo's left is +y), up */
	norm3(f, v->forward);
	cross3(r, f, v->up);
	norm3(r, r);
	cross3(u, r, f);
	k = 0.5f * h / tanf(0.5f * v->vertical_field_of_view);
	cx = v->x0 + 0.5f * w;
	cy = v->y0 + 0.5f * h;
	for (i = 0; i < 3; i++)
	{
		R0[i] = k * r[i] + cx * f[i];
		R1[i] = -k * u[i] + cy * f[i];
	}
	/* (matrix_t is m[column][row]) */
	for (i = 0; i < 3; i++)
	{
		c->m[i][0] = R0[i];
		c->m[i][1] = R1[i];
		c->m[i][2] = f[i];
		c->m[i][3] = f[i];
	}
	c->m[3][0] = -dot3(R0, P);
	c->m[3][1] = -dot3(R1, P);
	c->m[3][2] = c->m[3][3] = -dot3(f, P);
	/* the side planes through the eye; near and far across forward */
	tw = 0.5f * w / k;
	th = 0.5f * h / k;
	for (s = 0; s < 2; s++)
	{
		float sign = s ? -1.0f : 1.0f;

		for (i = 0; i < 3; i++)
		{
			c->plane[s][i] = f[i] * tw + sign * r[i];
			c->plane[2 + s][i] = f[i] * th + sign * u[i];
		}
		c->plane[s][3] = dot3(c->plane[s], P);
		c->plane[2 + s][3] = dot3(c->plane[2 + s], P);
	}
	for (i = 0; i < 3; i++)
	{
		c->plane[4][i] = f[i];
		c->plane[5][i] = -f[i];
	}
	c->plane[4][3] = dot3(f, P) + near;
	c->plane[5][3] = -(dot3(f, P) + far);
	for (i = 0; i < 6; i++)
		c->plane_length[i] = sqrtf(dot3(c->plane[i], c->plane[i]));
	memcpy(c->eye, P, sizeof(c->eye));
	memcpy(c->f, f, sizeof(c->f));
	memcpy(c->r, r, sizeof(c->r));
	memcpy(c->u, u, sizeof(c->u));
	c->k = k;
	c->cx = cx;
	c->cy = cy;
	c->near = near;
	c->far = far;
}

int dc_box_test(const dc_camera_t *c, const float lo[3], const float hi[3])
{
	int result = DC_IN;
	int i;

	for (i = 0; i < 6; i++)
	{
		const float *p = c->plane[i];
		float most = p[0] * (p[0] >= 0.0f ? hi[0] : lo[0]) + p[1] * (p[1] >= 0.0f ? hi[1] : lo[1]) +
			p[2] * (p[2] >= 0.0f ? hi[2] : lo[2]);
		float least;

		if (most < p[3])
			return DC_OUT;
		least = p[0] * (p[0] >= 0.0f ? lo[0] : hi[0]) + p[1] * (p[1] >= 0.0f ? lo[1] : hi[1]) +
			p[2] * (p[2] >= 0.0f ? lo[2] : hi[2]);
		if (least < p[3])
			result |= DC_PARTIAL | (i == 4 ? DC_NEAR : 0);
	}
	return result;
}

int dc_sphere_test(const dc_camera_t *c, const float center[3], float radius)
{
	int result = DC_IN;
	int i;

	for (i = 0; i < 6; i++)
	{
		float distance = dot3(c->plane[i], center) - c->plane[i][3];
		float reach = radius * c->plane_length[i];

		if (distance < -reach)
			return DC_OUT;
		if (distance < reach)
			result |= DC_PARTIAL | (i == 4 ? DC_NEAR : 0);
	}
	return result;
}

/* the first-person models: the game's first-person near and far planes
(n64_render.c), 1/w scaled past anything of the world's */
void n64_render_first_person_begin(void)
{
	if (!dc_frame_open || dc_first_person)
		return;
	dc_camera_build(&dc_cam, &dc_last_view, 0.01171875f, 16.0f);
	dc_zscale = 1024.0f;
	dc_first_person = 1;
}

void n64_render_view(const struct n64_view *view)
{
	float far;
	uint64_t start;
	unsigned long us;

	if (!dc_frame_open)
		return;
	dc_last_view = *view;
	dc_zscale = 1.0f;
	dc_first_person = 0;
	dc_stage = NULL;
	/* (a haze, not an opaque fog: the views see to their far plane) */
	far = (dc_fog.end > 0.0f && dc_fog.end < view->z_far && dc_haze >= 1.0f) ? dc_fog.end : view->z_far;
	dc_camera_build(&dc_cam, view, view->z_near, far);
	dc_op_begin();
	/* the sky, about the eye, before the level (its solid layer without
	depth: tools/n64/models_dc.py MATERIAL_SKY) */
	if (dc_sky_model >= 0 && !dc_no_sky)
	{
		static const unsigned char permutations[16];
		float matrix[13] = {1.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, view->position[0], view->position[1],
			view->position[2]};
		struct n64_object sky;

		memset(&sky, 0, sizeof(sky));
		sky.model_tag_index = dc_sky_model;
		sky.node_count = 1;
		sky.node_matrices = matrix;
		sky.region_permutations = permutations;
		sky.head_node = -1;
		sky.center[0] = view->position[0];
		sky.center[1] = view->position[1];
		sky.center[2] = view->position[2];
		sky.radius = 1.0f;
		sky.tint[0] = sky.tint[1] = sky.tint[2] = sky.tint[3] = 0xFF;
		n64_render_object(&sky);
	}
	start = timer_us_gettime64();
	dc_level_draw(&dc_cam, view);
	us = (unsigned long)(timer_us_gettime64() - start);
	dc_stat.level_us += us;
	if (us > dc_stat.level_us_worst)
		dc_stat.level_us_worst = us;
	dc_views++;
}

/* ---------- submission */

void dc_send_header(const pvr_poly_hdr_t *header)
{
	uint32_t *d = (uint32_t *)dc_slot();
	const uint32_t *s = (const uint32_t *)header;
	int i;

	for (i = 0; i < 8; i++)
		d[i] = s[i];
	dc_commit((pvr_vertex_t *)d);
}

static inline void dc_emit(const dc_cv *c, uint32_t command)
{
	pvr_vertex_t *d = dc_slot();
	float iw = 1.0f / c->w;

	d->flags = command;
	d->x = c->x * iw;
	d->y = c->y * iw;
	d->z = iw * dc_zscale;
	d->u = c->u;
	d->v = c->v;
	d->argb = c->argb;
	d->oargb = 0;
	dc_commit(d);
}

static inline uint32_t dc_lerp_argb(uint32_t a, uint32_t b, float t)
{
	uint32_t r = 0;
	int s;

	for (s = 0; s < 32; s += 8)
	{
		float ca = (float)((a >> s) & 255), cb = (float)((b >> s) & 255);

		r |= (uint32_t)(ca + t * (cb - ca) + 0.5f) << s;
	}
	return r;
}

/* where a (in front of the near plane) to b (behind it) crosses it */
static inline dc_cv dc_cut(const dc_cv *a, const dc_cv *b)
{
	float t = (a->w - dc_cam.near) / (a->w - b->w);
	dc_cv r;

	r.x = a->x + t * (b->x - a->x);
	r.y = a->y + t * (b->y - a->y);
	r.w = dc_cam.near;
	r.u = a->u + t * (b->u - a->u);
	r.v = a->v + t * (b->v - a->v);
	r.argb = dc_lerp_argb(a->argb, b->argb, t);
	return r;
}

/* one triangle against w >= near (Sutherland-Hodgman): 0, 3 or 4 vertices,
the same winding; four as the strip 0, 1, 3, 2 */
static void dc_triangle(const dc_cv *a, const dc_cv *b, const dc_cv *c)
{
	const dc_cv *v[3] = {a, b, c};
	dc_cv o[4];
	int n = 0, i;

	for (i = 0; i < 3; i++)
	{
		const dc_cv *p = v[i], *q = v[(i + 1) % 3];
		int pin = p->w >= dc_cam.near, qin = q->w >= dc_cam.near;

		if (pin)
			o[n++] = *p;
		if (pin != qin)
			o[n++] = pin ? dc_cut(p, q) : dc_cut(q, p);
	}
	if (n == 3)
	{
		dc_emit(&o[0], PVR_CMD_VERTEX);
		dc_emit(&o[1], PVR_CMD_VERTEX);
		dc_emit(&o[2], PVR_CMD_VERTEX_EOL);
	}
	else if (n == 4)
	{
		dc_emit(&o[0], PVR_CMD_VERTEX);
		dc_emit(&o[1], PVR_CMD_VERTEX);
		dc_emit(&o[3], PVR_CMD_VERTEX);
		dc_emit(&o[2], PVR_CMD_VERTEX_EOL);
	}
}

/* a strip that may cross the near plane: whole if it is all in front, else
its runs of triangles wholly in front as strips of their own (a run that
starts on an odd triangle led by its first vertex twice, a degenerate
triangle, so that each keeps its winding), the crossing ones clipped one
by one (each odd one turned, as a strip turns it), those wholly behind
left out. (Triangle by triangle, the first-person arms' strips, which
reach back past the eye, cost twice what the rest of the weapon did.) */
void dc_clip_strip(const dc_cv *const *v, int n)
{
	float near = dc_cam.near;
	int count = 0, i, t;

#define in(k) (v[k]->w >= near)
	for (i = 0; i < n; i++)
		count += in(i);
	if (!count)
		return;
	if (count == n)
	{
		for (i = 0; i < n - 1; i++)
			dc_emit(v[i], PVR_CMD_VERTEX);
		dc_emit(v[n - 1], PVR_CMD_VERTEX_EOL);
		return;
	}
	for (t = 0; t + 2 < n;)
	{
		int end;

		if (!(in(t) && in(t + 1) && in(t + 2)))
		{
			if (in(t) || in(t + 1) || in(t + 2))
			{
				if (t & 1)
					dc_triangle(v[t + 1], v[t], v[t + 2]);
				else
					dc_triangle(v[t], v[t + 1], v[t + 2]);
			}
			t++;
			continue;
		}
		/* the run t..end-1 of triangles wholly in front */
		for (end = t + 1; end + 2 < n && in(end + 2); end++)
			;
		if (t & 1)
			dc_emit(v[t], PVR_CMD_VERTEX);
		for (i = t; i < end + 1; i++)
			dc_emit(v[i], PVR_CMD_VERTEX);
		dc_emit(v[end + 1], PVR_CMD_VERTEX_EOL);
		t = end;
	}
#undef in
}

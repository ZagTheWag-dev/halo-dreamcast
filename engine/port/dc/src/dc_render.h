/*
DC_RENDER.H

The Dreamcast renderer's shared state (dc_render.c, dc_level.c): the PowerVR
up or not, the frame's direct-render state, the view's camera (a matrix that
takes world positions to screen x, y and 1/depth in one ftrv and divide), the
fog, and the frame's counts for the statistics line.
*/

#ifndef __DC_RENDER_H
#define __DC_RENDER_H

#include <kos.h>
#include <dc/pvr.h>
#include <dc/matrix.h>

#include "n64.h"

/* the N64 renderer's material flags (port/n64/src/n64_render.c, level.py) */
#define MATERIAL_ALPHA_TEST 1
#define MATERIAL_TWO_SIDED 2
#define MATERIAL_TRANSPARENT 4
#define MATERIAL_UNTEXTURED 8
#define MATERIAL_CHANGE_COLOR 16
#define MATERIAL_GLOW 32
#define MATERIAL_SKY 64
#define MATERIAL_WATER 1024
#define MATERIAL_BLEND 2048
/* (dc_header_compile_list only: depth compared greater or equal, for an
overlay drawn on its own surface again: dc_model.c's change colour) */
#define DC_HEADER_DEPTH_EQUAL 0x10000

typedef struct
{
	matrix_t m __attribute__((aligned(32)));
	/* the frustum's planes, inside where n.p >= d: left, right, top,
	bottom, near, far */
	float plane[6][4];
	/* each plane's normal's length (the sphere tests) */
	float plane_length[6];
	float eye[3], f[3], r[3], u[3];
	float k, cx, cy, near, far;
} dc_camera_t;

/* a vertex transformed but not divided: screen x and y times depth, the
depth (world units ahead of the eye), its texture coordinates and colour */
typedef struct
{
	float x, y, w, u, v;
	uint32_t argb;
} dc_cv;

enum
{
	DC_OUT = 0,
	DC_IN = 1,
	DC_PARTIAL = 2,
	DC_NEAR = 4,
};

/* testing setting "dc_no_detail": no detail pass over the terrain */
extern int dc_no_detail;

struct dc_statistics
{
	unsigned long groups, groups_pvs, runs, runs_clipped, vertices, strip_vertices;
	/* the terrain's detail pass's strip vertices (dc_level.c) */
	unsigned long detail_vertices;
	unsigned long level_us, level_us_worst;
	unsigned long objects, objects_culled, objects_far, model_triangles, model_us;
	/* of which the first-person weapon's */
	unsigned long first_person_triangles, first_person_us;
	/* units' and vehicles' shadows (dc_shadow.c) */
	unsigned long shadows, shadow_us;
};

extern int dc_pvr_up;
extern pvr_dr_state_t dc_dr;
extern dc_camera_t dc_cam;
extern struct dc_statistics dc_stat;
extern int dc_cull;
/* 1/w's factor (1024 for the first-person models: nearer than the world) */
extern float dc_zscale;
extern int dc_first_person;

/* the list vertices go to: the opaque list straight to the tile
accelerator, the punch-through and translucent lists staged in RAM (sent
at the frame's end) */
extern pvr_vertex_t *dc_stage;
extern int *dc_stage_count;
extern int dc_stage_maximum;
extern pvr_vertex_t dc_stage_overflow;
/* the opaque list's 32-byte packets sent this frame (the tile accelerator's
vertex buffer: dc_stage_room keeps every list's together inside it) */
extern int dc_op_packets;

static inline pvr_vertex_t *dc_slot(void)
{
	if (!dc_stage)
	{
		dc_op_packets++;
		return pvr_dr_target(dc_dr);
	}
	if (*dc_stage_count >= dc_stage_maximum)
		return &dc_stage_overflow;
	return &dc_stage[(*dc_stage_count)++];
}

static inline void dc_commit(pvr_vertex_t *vertex)
{
	if (!dc_stage)
		pvr_dr_commit(vertex);
}

void dc_use_list(int list);
/* whether the current list has room for this many more vertices */
int dc_stage_room(int count);

void dc_send_header(const pvr_poly_hdr_t *header);

/* the frame's pace (dc_pace.c): the display's refreshes, the "dcf:" line's
times at the frame's start, after pvr_wait_ready, drawn, and sent */
extern unsigned long long dc_idle_us;
/* the game going to sleep and awake again (dc_main.c: the log written out
meanwhile) */
void dc_idle_begin(void);
void dc_idle_end(void);
void dc_pace_initialize(void);
void dc_pace_frame_begin(void);
void dc_pace_frame_ready(void);
void dc_pace_frame_drawn(void);
void dc_pace_frame_end(void);
/* the menu's frame (dc_menu.c): asleep until the refresh two after the last
one's (30 frames a second; at once when late) */
void dc_pace_menu_frame(void);
int dc_box_test(const dc_camera_t *camera, const float minimum[3], const float maximum[3]);
void dc_clip_strip(const dc_cv *const *strip, int count);
void dc_header_compile(pvr_poly_hdr_t *header, pvr_ptr_t texture, int format, int size, int two_sided);
/* a header for a list: the material's flags (MATERIAL_*) choose two-sided,
additive (glow) or alpha blending on the translucent list */
void dc_header_compile_list(pvr_poly_hdr_t *header, int list, pvr_ptr_t texture, int format, int size, int flags);
/* the list a material's flags put it in */
int dc_material_list(int flags);
/* a sphere against the view's frustum: DC_OUT, DC_IN, or DC_PARTIAL with
DC_NEAR when it crosses the near plane */
int dc_sphere_test(const dc_camera_t *camera, const float center[3], float radius);

void dc_level_draw(const dc_camera_t *camera, const struct n64_view *view);
/* the level's material headers again (the fog changed) */
void dc_level_compile_headers(void);
void dc_model_compile_headers(void);
int dc_model_load(const char *map_name);
void dc_model_unload(void);

/* the view being drawn (the last n64_render_view's), whether a frame is
open, the fog's range (0, 0: none) */
const struct n64_view *dc_render_view(void);
int dc_render_frame_open(void);
void dc_render_fog_range(float *start, float *end);
/* the particles' statistics line (dc_fx.c) */
void dc_fx_statistics_line(void);
/* the sound's statistics line (dc_sound.c) */
void dc_sound_statistics_line(void);
/* the title music's (dc_sound.c), from the menu */
void dc_music_statistics_line(void);

/* the shadows' falloff texture, kept for good, as the PowerVR comes up
(dc_shadow.c; after dc_hud_reserve: it is in the HUD's coverage palette) */
void dc_shadow_reserve(void);

/* the HUD (dc_hud.c): its meters' textures loaded (between the frame's
lists), whether it has primitives this frame, sent into the open
translucent list after the world's, and the frame's end */
/* the HUD's lasting textures and palettes, as the PowerVR comes up */
void dc_hud_reserve(void);
void dc_hud_upload(void);
int dc_hud_pending(void);
void dc_hud_submit(void);
void dc_hud_frame_end(void);
/* the menu's drawing (dc_menu.c) with the HUD's: outside the views, in the
640x480 screen's pixels, colours straight ARGB; a sprite's part in its own
texels */
int dc_hud_draw_begin(void);
void dc_hud_draw_end(void);
int dc_hud_sprite_size(int sprite, int *width, int *height);
void dc_hud_sprite(int sprite, float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1,
	unsigned long argb);
void dc_hud_box(float x0, float y0, float x1, float y1, unsigned long argb);

#endif /* __DC_RENDER_H */

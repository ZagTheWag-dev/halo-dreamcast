/*
DC_INTRO.C

Halo's boot intro (the Xbox disc's bink/intro.bik: the Microsoft and Bungie
logos), before the title the first time the menu comes up: its pictures
(intro/intro.dcv, tools/n64/intro_dc.py: a VQ texture a picture, each on
sectors of its own) read from the disc a picture at a time, straight
through, into two textures taken in turn (one drawn while the other is
loaded); its sound (intro/intro.dca) read into RAM first and played by the
music's thread (dc_sound.c dc_music_play_memory), so that the drive only
reads the pictures. The pictures follow the sound's clock (a picture late
is skipped, never the sound). START or A on any pad ends it.

The PowerVR's own scenes, the game's frame not open: the intro runs before
the menu's first frame (dc_menu.c n64_menu_run).
*/

#include <kos.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "dc_render.h"

#ifndef DC_FILE_ROOT
#define DC_FILE_ROOT "/cd"
#endif

int dc_music_play_memory(const void *data, unsigned long size);
int dc_music_finished(void);

struct dcv_header
{
	char magic[4];
	uint32_t rate, count;
	uint16_t width, height;
	uint32_t format, frame_bytes, stride, data_offset;
};

typedef char dcv_header_size_assert[sizeof(struct dcv_header) == 32 ? 1 : -1];

/* any pad's START or A, pressed (not held since before) */
static int dc_intro_skip(void)
{
	static int held = 1;
	int down = 0;

	MAPLE_FOREACH_BEGIN(MAPLE_FUNC_CONTROLLER, cont_state_t, state)
		if (state->buttons & (CONT_START | CONT_A))
			down = 1;
	MAPLE_FOREACH_END()
	if (!down)
	{
		held = 0;
		return 0;
	}
	return !held;
}

static void dc_intro_draw(const pvr_poly_hdr_t *header)
{
	pvr_vertex_t v;

	pvr_wait_ready();
	pvr_scene_begin();
	pvr_list_begin(PVR_LIST_OP_POLY);
	pvr_prim((void *)header, sizeof(*header));
	v.flags = PVR_CMD_VERTEX;
	v.z = 1.0f;
	v.argb = 0xFFFFFFFF;
	v.oargb = 0;
	v.x = 0.0f; v.y = 480.0f; v.u = 0.0f; v.v = 1.0f;
	pvr_prim(&v, sizeof(v));
	v.x = 0.0f; v.y = 0.0f; v.u = 0.0f; v.v = 0.0f;
	pvr_prim(&v, sizeof(v));
	v.x = 640.0f; v.y = 480.0f; v.u = 1.0f; v.v = 1.0f;
	pvr_prim(&v, sizeof(v));
	v.flags = PVR_CMD_VERTEX_EOL;
	v.x = 640.0f; v.y = 0.0f; v.u = 1.0f; v.v = 0.0f;
	pvr_prim(&v, sizeof(v));
	pvr_list_finish();
	pvr_scene_finish();
}

void dc_intro_play(void)
{
	struct dcv_header h;
	pvr_poly_hdr_t headers[2];
	pvr_ptr_t texture[2] = {NULL, NULL};
	uint8_t *picture = NULL, *sound = NULL;
	file_t video = -1, audio;
	long sound_size = 0;
	unsigned long shown = 0, skipped = 0;
	uint64_t start;
	int current = -1, which = 0, i;
	char line[160];

	if (!dc_pvr_up || n64_debug_flag("debug_no_intro"))
		return;
	video = fs_open(DC_FILE_ROOT "/intro/intro.dcv", O_RDONLY);
	if (video < 0)
	{
		n64_log("dcv: no intro");
		return;
	}
	if (fs_read(video, &h, sizeof(h)) != sizeof(h) || memcmp(h.magic, "DCV1", 4) || !h.rate || !h.count ||
		h.stride < h.frame_bytes || h.stride % 2048 || h.data_offset % 2048)
	{
		n64_log("dcv: the intro is not this build's");
		fs_close(video);
		return;
	}
	picture = memalign(32, h.stride);
	texture[0] = pvr_mem_malloc(h.frame_bytes);
	texture[1] = pvr_mem_malloc(h.frame_bytes);
	if (!picture || !texture[0] || !texture[1])
	{
		n64_log("dcv: no memory for the intro");
		goto done;
	}
	for (i = 0; i < 2; i++)
	{
		pvr_poly_cxt_t c;

		pvr_poly_cxt_txr(&c, PVR_LIST_OP_POLY, (int)h.format, h.width, h.height, texture[i], PVR_FILTER_BILINEAR);
		c.gen.culling = PVR_CULLING_NONE;
		c.gen.fog_type = PVR_FOG_DISABLE;
		c.depth.comparison = PVR_DEPTHCMP_ALWAYS;
		c.txr.env = PVR_TXRENV_REPLACE;
		pvr_poly_compile(&headers[i], &c);
	}
	pvr_set_bg_color(0.0f, 0.0f, 0.0f);

	/* the sound into RAM, then started (the pictures' clock) */
	audio = fs_open(DC_FILE_ROOT "/intro/intro.dca", O_RDONLY);
	if (audio >= 0)
	{
		sound_size = (long)fs_total(audio);
		sound = sound_size > 0 ? memalign(32, (sound_size + 2047) & ~2047L) : NULL;
		if (sound && fs_read(audio, sound, sound_size) != sound_size)
		{
			free(sound);
			sound = NULL;
		}
		fs_close(audio);
	}
	if (sound && !dc_music_play_memory(sound, sound_size))
	{
		free(sound);
		sound = NULL;
	}

	start = timer_us_gettime64();
	for (;;)
	{
		uint64_t now = timer_us_gettime64();
		int due = (int)((now - start) * h.rate / 1000000ULL);
		uint32_t at;

		if (due >= (int)h.count || dc_intro_skip())
			break;
		if (due <= current)
		{
			/* (the next picture not due: asleep till it is) */
			uint64_t next = start + (uint64_t)(current + 1) * 1000000ULL / h.rate;

			if (next > now + 2000)
				thd_sleep((int)((next - now) / 1000));
			continue;
		}
		skipped += due - current - 1;
		current = due;
		at = h.data_offset + (uint32_t)current * h.stride;
		if (fs_seek(video, at, SEEK_SET) != (off_t)at || fs_read(video, picture, h.stride) != (ssize_t)h.stride)
			break;
		/* (the texture loaded last drawn two scenes ago: rendered by now) */
		pvr_wait_render_done();
		pvr_txr_load(picture, texture[which], h.frame_bytes);
		dc_intro_draw(&headers[which]);
		which ^= 1;
		shown++;
	}
	snprintf(line, sizeof(line), "dcv: intro, %lu of %lu pictures shown (%lu late), %lu ms", shown,
		(unsigned long)h.count, skipped, (unsigned long)((timer_us_gettime64() - start) / 1000));
	n64_log(line);
done:
	if (sound)
		n64_music_stop();
	pvr_wait_render_done();
	for (i = 0; i < 2; i++)
	{
		if (texture[i])
			pvr_mem_free(texture[i]);
	}
	free(picture);
	free(sound);
	fs_close(video);
}

/*
DC_PACE.C

The frame's pace: the display's refreshes and the frame times line.

The game paces itself the Xbox's way on the Dreamcast (main.c
main_update_time and main_rasterizer_throttle with
rasterizer_globals.framerate_throttle on: port/n64/game/n64_render_game.c):
each frame is given a refresh to be shown at, two after the last one's at
30 frames a second, its game time is that many refreshes, and the frame is
held back until the refresh before, then sent (pvr_scene_finish). The
PowerVR renders it in a few milliseconds and KallistiOS flips to it on the
next refresh (pvr_irq.c: a render done flips at the vertical blank): every
frame on the screen for two refreshes, the game's 30 ticks a second exactly
one a frame. The game's count of refreshes
(rasterizer_globals.frame_and_vertical_blank_index) is moved on by a
KallistiOS vertical blank handler here, and the wait for it sleeps until the
interrupt (genwait), the CPU free for the other threads.

Before, the game slept for what was left of 33 milliseconds (Sleep: thd_sleep,
which wakes only on KallistiOS's 10 ms scheduler tick) and pvr_wait_ready
waited for the refresh on top of that: the two beat against each other, 20
and 30 frames a second by turns. Even a precise sleep (n64_wait_microseconds
now) is not the display's pace: 33 ms frames are 30.3 a second against the
television's 29.97, a few frames in 120 on the screen for one refresh and
some for three. (Testing setting "dc_sleep_pacing": that way again.)

A refresh is 1/60 of a second of game time (1/50 on PAL), as on the Xbox:
one tick a frame, every frame, though NTSC refreshes 59.94 times a second
(Flycast about 59.8): the game runs that much slower than the clock, 150
ticks in 5005 ms (5016 in Flycast), which nobody sees; timing the ticks by
the clock instead would run two ticks in a frame now and then, which
everybody would.

Every 120 frames a "dcf:" line: the frame rate measured over them, how many
refreshes each frame was on the screen (the hardware's framebuffer address
watched at each vertical blank), and where a frame's time went, average and
worst in microseconds: the game (input, the ticks, the director: from one
frame's end to the next one's start), the drawing (the views' level, objects
and HUD on the CPU, the opaque list going straight to the tile accelerator),
the send (the punch-through and translucent lists and the HUD at the frame's
end), pvr_wait_ready, the idle wait for the refresh, and the PowerVR's own
render time of the last frame.
*/

#include <stdio.h>
#include <string.h>

#include "dc_render.h"

/* time the game spent asleep or waiting (n64_wait_microseconds, the
refresh wait): taken out of the phase it fell in */
unsigned long long dc_idle_us;

static volatile unsigned long dc_vblanks;
static volatile int dc_vblank_waiting;
static void (*dc_vblank_callback)(void);
static int dc_vblank_handle = -1;

/* frames on the screen for 1, 2, 3, 4 and more refreshes */
#define DC_HELD_COUNTS 5
static volatile unsigned long dc_held[DC_HELD_COUNTS];
static uint32 dc_shown_address;
static unsigned long dc_shown_vblank;

static struct
{
	uint64 frame_end, begin, ready, idle_at_end, idle_at_ready;
	uint64 window_start;
	unsigned long frames;
	unsigned long game, game_worst, draw, draw_worst, send, send_worst;
	unsigned long wait_ready, wait_ready_worst, idle, idle_worst, render, render_worst;
} dc_pace;

/* ---------- the display's refreshes */

/* in the vertical blank interrupt, beside KallistiOS's own (which flips to a
rendered frame): the framebuffer address changed, a frame is newly on the
screen, and the last one's time there is counted (whichever handler runs
first, the same refresh each time) */
static void dc_vertical_blank(uint32 code, void *data)
{
	uint32 address = PVR_GET(PVR_FB_ADDR);

	(void)code;
	(void)data;
	dc_vblanks++;
	if (address != dc_shown_address)
	{
		unsigned long held = dc_vblanks - dc_shown_vblank;

		if (dc_shown_vblank && held)
			dc_held[held < DC_HELD_COUNTS ? held - 1 : DC_HELD_COUNTS - 1]++;
		dc_shown_address = address;
		dc_shown_vblank = dc_vblanks;
	}
	if (dc_vblank_callback)
		dc_vblank_callback();
	/* the game waiting for this refresh: woken and run now, ahead of the
	thread that was running (a thread woken from an interrupt otherwise
	waits for the scheduler's next 10 ms tick; pvr_irq.c does the same) */
	if (dc_vblank_waiting)
	{
		dc_vblank_waiting = 0;
		genwait_wake_all((void *)&dc_vblanks);
		thd_schedule(0, 0);
	}
}

void dc_pace_initialize(void)
{
	if (dc_vblank_handle < 0)
		dc_vblank_handle = vblank_handler_add(dc_vertical_blank, NULL);
	dc_pace.window_start = timer_us_gettime64();
}

void n64_render_set_vertical_blank(void (*callback)(void))
{
	dc_vblank_callback = callback;
}

/* 50 on a PAL television, 60 otherwise (VGA, NTSC) */
int n64_render_refresh_rate(void)
{
	return vid_mode && vid_mode->cable_type != CT_VGA && (vid_mode->flags & VID_PAL) ? 50 : 60;
}

/* asleep until *count reaches target or the next refresh (at most
milliseconds): the test and the sleep with the interrupts off, so that the
refresh cannot fall between them */
void n64_render_wait_vertical_blank(volatile unsigned long long *count, unsigned long long target,
	unsigned long milliseconds)
{
	uint64 start = timer_us_gettime64();
	int flags = irq_disable();

	if (*count < target)
	{
		dc_vblank_waiting = 1;
		/* (the log written out meanwhile: dc_main.c) */
		dc_idle_begin();
		genwait_wait((void *)&dc_vblanks, "vertical blank", milliseconds ? milliseconds : 1, NULL);
		dc_idle_end();
	}
	irq_restore(flags);
	dc_idle_us += timer_us_gettime64() - start;
}

/* the menu's frames (dc_menu.c, outside the game's pace): each at the
refresh two after the last one's, asleep until it as the game's are; a
frame that came late starts the count again from now */
void dc_pace_menu_frame(void)
{
	static unsigned long last;
	unsigned long target = last + 2;
	uint64 start = timer_us_gettime64();
	int flags;

	if (!last || (long)(dc_vblanks - target) >= 0)
	{
		last = dc_vblanks;
		return;
	}
	flags = irq_disable();
	while ((long)(dc_vblanks - target) < 0)
	{
		dc_vblank_waiting = 1;
		dc_idle_begin();
		genwait_wait((void *)&dc_vblanks, "menu frame", 40, NULL);
		dc_idle_end();
	}
	irq_restore(flags);
	last = target;
	dc_idle_us += timer_us_gettime64() - start;
}

/* ---------- the frame times */

static void dc_pace_add(unsigned long *sum, unsigned long *worst, unsigned long long us)
{
	unsigned long value = us > 0xFFFFFFFFull ? 0xFFFFFFFFul : (unsigned long)us;

	*sum += value;
	if (value > *worst)
		*worst = value;
}

/* n64_render_frame_begin, before pvr_wait_ready */
void dc_pace_frame_begin(void)
{
	dc_pace.begin = timer_us_gettime64();
	if (dc_pace.frame_end)
	{
		unsigned long long idle = dc_idle_us - dc_pace.idle_at_end;
		unsigned long long span = dc_pace.begin - dc_pace.frame_end;

		dc_pace_add(&dc_pace.game, &dc_pace.game_worst, span > idle ? span - idle : 0);
	}
}

/* after pvr_wait_ready */
void dc_pace_frame_ready(void)
{
	dc_pace.ready = timer_us_gettime64();
	dc_pace_add(&dc_pace.wait_ready, &dc_pace.wait_ready_worst, dc_pace.ready - dc_pace.begin);
	dc_pace.idle_at_ready = dc_idle_us;
}

/* n64_render_frame_end's start: the drawing done */
void dc_pace_frame_drawn(void)
{
	uint64 now = timer_us_gettime64();
	unsigned long long idle = dc_idle_us - dc_pace.idle_at_ready;
	unsigned long long span = now - dc_pace.ready;

	dc_pace_add(&dc_pace.draw, &dc_pace.draw_worst, span > idle ? span - idle : 0);
	dc_pace.frame_end = now;
}

/* n64_render_frame_end's end: the frame sent */
void dc_pace_frame_end(void)
{
	pvr_stats_t stats;
	uint64 now = timer_us_gettime64();

	dc_pace_add(&dc_pace.send, &dc_pace.send_worst, now - dc_pace.frame_end);
	pvr_get_stats(&stats);
	/* (before the first render the time is not set) */
	if (stats.rnd_last_time < 1000000000ull)
		dc_pace_add(&dc_pace.render, &dc_pace.render_worst, stats.rnd_last_time / 1000);
	dc_pace_add(&dc_pace.idle, &dc_pace.idle_worst, dc_idle_us - dc_pace.idle_at_end);
	dc_pace.frame_end = now;
	dc_pace.idle_at_end = dc_idle_us;
	if (++dc_pace.frames >= 120)
	{
		char line[300];
		unsigned long frames = dc_pace.frames;
		uint64 span = now - dc_pace.window_start;
		unsigned long held[DC_HELD_COUNTS];
		int i;

		for (i = 0; i < DC_HELD_COUNTS; i++)
		{
			held[i] = dc_held[i];
			dc_held[i] = 0;
		}
		snprintf(line, sizeof(line),
			"dcf: %lu.%02lu fps over %lu ms, refreshes a frame 1:%lu 2:%lu 3:%lu 4:%lu 5+:%lu;"
			" us average/worst: game %lu/%lu draw %lu/%lu send %lu/%lu ready %lu/%lu idle %lu/%lu"
			" render %lu/%lu",
			(unsigned long)(frames * 1000000ull / span), (unsigned long)(frames * 100000000ull / span % 100),
			(unsigned long)(span / 1000), held[0], held[1], held[2], held[3], held[4],
			dc_pace.game / frames, dc_pace.game_worst, dc_pace.draw / frames, dc_pace.draw_worst,
			dc_pace.send / frames, dc_pace.send_worst, dc_pace.wait_ready / frames, dc_pace.wait_ready_worst,
			dc_pace.idle / frames, dc_pace.idle_worst, dc_pace.render / frames, dc_pace.render_worst);
		n64_log(line);
		memset(&dc_pace, 0, sizeof(dc_pace));
		dc_pace.frame_end = now;
		dc_pace.idle_at_end = dc_idle_us;
		dc_pace.window_start = now;
	}
}

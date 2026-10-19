/*
DC_MENU.C

What can be played (port/n64/src/n64_menu.c's tables and the functions the
game reads them through), and the Dreamcast's menus: Halo's own, the Xbox's
shell (source/interface/ui_widget*.c over ui.map's widgets, pictures, fonts
and sounds: maps/ui.map, hud/ui.dch and snd/ui.dcs from tools/n64/convert.py
--dc --ui), run by port/dc/game/dc_shell.c, over the scene behind Halo's
main menu: ui.map's space sky and the ring, Halo's title camera gliding from
point to point (levels/ui.dcm, levels/ui.dcs: tools/n64/ui_dc.py); Halo's
title music from the disc (n64_music_start, dc_sound.c).

The menus are the Xbox's: the main menu, MULTIPLAYER, SPLIT SCREEN (press A
to join, a profile each, the map, the game type, the pregame lobby and its
countdown), EDIT GAMETYPES, SETTINGS (profiles and their controls, the
virtual keyboard); what the Dreamcast has not (the campaign, system link,
the Xbox's demos) is there as the Xbox shows it without them. The players'
choices become the game's (dc_shell_take_game): the players and their pads,
teams and colours, the map (Blood Gulch, the one there is: the others are
on the map list as on the Xbox, and refused), the game type as it is, and
bots to make four.

The first time, the boot intro (dc_intro.c) and the main menu; after a
game, the map select screen with the same players (as the Xbox's after a
split screen game); after the pause menu's MAIN MENU, the main menu.

Each frame: the pads read as the Xbox pad they are (xdk_input.c
dc_input_menu: A B X Y START, the D-pad, the stick) through Halo's own
input, its events and its widgets (dc_shell_frame), then the scene and the
widgets drawn (dc_shell_draw), at 30 frames a second by the display's
refreshes (dc_pace_menu_frame: asleep meanwhile, so the log's writer runs).

Testing setting "debug_menu=<script>": a press every 20 frames, one
character each: u d l r (D-pad), a b x y s (A, B, X, Y, START) on port A,
'.' none; the script goes on where it left off each time the menus come
back, so one script can choose a game, wait out its end (debug_game_ticks)
and go on.
*/

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dc_render.h"

/* port/dc/game/dc_shell.c's */
int dc_shell_open(int after_game);
void dc_shell_close(void);
void dc_shell_frame(void);
void dc_shell_draw(void);
int dc_shell_game_ready(void);
void dc_shell_take_game(struct n64_menu_choice *choice);
/* xdk_input.c's: the game's gamepads' ports */
extern int n64_gamepad_ports[4];

/* ---------- what can be played: Blood Gulch alone (the maps' table keeps
the N64's shape: the game reads it through n64_menu_map_*) */

static const struct
{
	const char *folder, *name;
} maps[] = {
#ifdef HALO_N64_CAMPAIGN
	/* (the campaign disc: its level, n64_campaign.c) */
	{"a30", "HALO"},
#else
	{"bloodgulch", "BLOOD GULCH"},
#endif
};
#define MAP_COUNT ((int)(sizeof(maps) / sizeof(maps[0])))

/* the game types (n64_game.c reads them by number: dc_shell.c picks the
one of the variant Halo's menus chose, by its engine and teams) */
static const struct
{
	const char *variant, *name;
	int teams;
	int bot_match;
} game_types[] = {
	{"slayer", "SLAYER", 0},
	{"team_slayer", "TEAM SLAYER", 1},
	/* (slayer with everyone starting with a pistol and an assault rifle:
	port/n64/game/n64_game.c) */
	{"slayer_pro", "SLAYER PRO", 0},
	{"team_slayer_pro", "TEAM SLAYER PRO", 1},
	{"ctf", "CAPTURE THE FLAG", 1},
	{"king", "KING OF THE HILL", 0},
	{"team_king", "TEAM KING", 1},
	{"oddball", "ODDBALL", 0},
	{"team_oddball", "TEAM ODDBALL", 1},
	{"race", "RACE", 0},
	{"team_race", "TEAM RACE", 1},
	{"slayer", "BOT MATCH", 0, 1},
};
#define GAME_TYPE_COUNT ((int)(sizeof(game_types) / sizeof(game_types[0])))

const char *n64_menu_map_folder(int map) { return map >= 0 && map < MAP_COUNT ? maps[map].folder : maps[0].folder; }
int n64_menu_map_count(void) { return MAP_COUNT; }
const char *n64_menu_map_name(int map) { return map >= 0 && map < MAP_COUNT ? maps[map].name : maps[0].name; }
const char *n64_menu_game_variant(int type) { return type >= 0 && type < GAME_TYPE_COUNT ? game_types[type].variant : "slayer"; }
int n64_menu_game_teams(int type) { return type >= 0 && type < GAME_TYPE_COUNT && game_types[type].teams; }
int n64_menu_game_bots(int type) { return type >= 0 && type < GAME_TYPE_COUNT && game_types[type].bot_match; }

int n64_menu_map_from_folder(const char *folder)
{
	int i;

	for (i = 0; i < MAP_COUNT; i++)
	{
		if (!strcmp(maps[i].folder, folder))
			return i;
	}
	return -1;
}

/* ---------- the menus' things */

/* n64_menu.c's 640x480 screen (the PowerVR's own: a pixel a unit) */
#define SCREEN_W 640
#define SCREEN_H 480
#define TEXT_COLOR 0xFF8CC0FFu
#define TEXT_BRIGHT 0xFFFFFFFFu

static struct
{
	struct n64_font *large, *small;
	/* the frames drawn (the script), the flight's clock */
	unsigned long frame;
	unsigned long long start_us;
	int scene;
	/* the menus have been up (after a game: its map select screen next) */
	int opened;
} menu;

/* ---------- the scene behind Halo's main menu

ui.map's space sky and the ring (levels/ui.dcm and levels/ui.dcs:
tools/n64/ui_dc.py), seen as Halo's title sees them (port/n64/src/
n64_menu.c's port of it): the ui scenario's script ring_loop sets the
camera to uicam, then to point_2 ... point_11 in turn, round and round,
each with a 400-tick transition (13 s) and the next one 200 ticks (6.7 s)
later, so that the camera never quite arrives; Halo's observer glides it
(camera/observer.c: from where it is, with its velocity and acceleration, a
quintic to rest at the point in the time left, planned again every frame).
In real time, whatever the frame rate. */

#define CAMERA_INTERVAL (200.f / 30.f)
#define CAMERA_TRANSITION (400.f / 30.f)
#define SCENE_POINTS 16

static const char *const camera_path[] = {
	"uicam", "point_2", "point_3", "point_4", "point_5", "point_6",
	"point_7", "point_8", "point_9", "point_10", "point_11",
};
#define CAMERA_PATH_LENGTH ((int)(sizeof(camera_path) / sizeof(camera_path[0])))

struct dcs_header
{
	char magic[4];
	uint16_t sky_model, ring_model;
	float ring[13];
	uint32_t point_count;
};

static struct
{
	int loaded;
	int sky_model, ring_model;
	float ring[13];
	/* each camera point: position, forward, up, horizontal field of view */
	char point_names[SCENE_POINTS][32];
	float points[SCENE_POINTS][10];
	int point_count;
} scene;

static struct
{
	int started;
	float position[3], forward[3], up[3], field_of_view;
	float velocity[3], acceleration[3], spin_velocity[3], spin_acceleration[3];
	float time;
	unsigned long long last_us;
} camera;

static int scene_load(void)
{
	struct dcs_header h;
	long file = n64_file_open("rom:/levels/ui.dcs");
	uint32_t i;

	scene.loaded = 0;
	scene.point_count = 0;
	if (file < 0)
		return 0;
	if (n64_file_read(file, &h, sizeof(h)) == sizeof(h) && !memcmp(h.magic, "DCS1", 4))
	{
		scene.sky_model = h.sky_model == 0xFFFF ? -1 : h.sky_model;
		scene.ring_model = h.ring_model == 0xFFFF ? -1 : h.ring_model;
		memcpy(scene.ring, h.ring, sizeof(scene.ring));
		for (i = 0; i < h.point_count && scene.point_count < SCENE_POINTS; i++)
		{
			if (n64_file_read(file, scene.point_names[scene.point_count], 32) != 32 ||
				n64_file_read(file, scene.points[scene.point_count], 40) != 40)
				break;
			scene.point_names[scene.point_count][31] = 0;
			scene.point_count++;
		}
		scene.loaded = 1;
	}
	n64_file_close(file);
	return scene.loaded;
}

static const float *camera_point(const char *name)
{
	int i;

	for (i = 0; i < scene.point_count; i++)
	{
		if (!strcmp(scene.point_names[i], name))
			return scene.points[i];
	}
	return scene.point_count ? scene.points[0] : NULL;
}

/* one value's step toward its target (observer.c's polynomial): the
displacement to go, the velocity and acceleration (updated), the time left
and the step's; returns how far to move */
static float glide(float displacement, float *velocity, float *acceleration, float remaining, float dt)
{
	float i1, i2, i3, i4, i5, v, a, b, c, r, r2, delta;

	if (remaining <= dt || remaining <= 0.f)
	{
		*velocity = *acceleration = 0.f;
		return displacement;
	}
	i1 = 1.f / remaining;
	i2 = i1 * i1;
	i3 = i2 * i1;
	i4 = i3 * i1;
	i5 = i4 * i1;
	/* (Halo's velocity runs against the time left) */
	v = -*velocity;
	a = *acceleration * i3 * 0.5f - (v * i4 * 3.f + displacement * i5 * 6.f);
	b = v * i3 * 7.f + displacement * i4 * 15.f - *acceleration * i2;
	c = *acceleration * i1 * 0.5f - (v * i2 * 4.f + displacement * i3 * 10.f);
	r = remaining - dt;
	r2 = r * r;
	delta = a * r2 * r2 * r + b * r2 * r2 + c * r2 * r + displacement;
	*velocity = -(5.f * a * r2 * r2 + 4.f * b * r2 * r + 3.f * c * r2);
	*acceleration = 20.f * a * r2 * r + 12.f * b * r2 + 6.f * c * r;
	return delta;
}

static void cross(const float *a, const float *b, float *out)
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static void normalize(float *v)
{
	float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (length > 1e-6f)
	{
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}
}

/* v turned about the unit axis by the angle (Rodrigues) */
static void rotate(float *v, const float *axis, float angle)
{
	float k_v[3], s = sinf(angle), c = cosf(angle), dot = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
	int i;

	cross(axis, v, k_v);
	for (i = 0; i < 3; i++)
		v[i] = v[i] * c + k_v[i] * s + axis[i] * dot * (1.f - c);
}

/* the rotation (axis times angle) from the frame forward, up to the point's */
static void rotation_to(const float *forward, const float *up, const float *point, float *out)
{
	float left[3], target_left[3], m[3][3];
	const float *target_forward = point + 3, *target_up = point + 6;
	float angle, s;
	int i, j;

	cross(up, forward, left);
	cross(target_up, target_forward, target_left);
	for (i = 0; i < 3; i++)
	{
		for (j = 0; j < 3; j++)
			m[i][j] = target_forward[i] * forward[j] + target_left[i] * left[j] + target_up[i] * up[j];
	}
	angle = acosf(fmaxf(-1.f, fminf(1.f, (m[0][0] + m[1][1] + m[2][2] - 1.f) * 0.5f)));
	s = sinf(angle);
	if (angle < 1e-4f || s < 1e-4f)
	{
		out[0] = out[1] = out[2] = 0.f;
		return;
	}
	out[0] = (m[2][1] - m[1][2]) / (2.f * s) * angle;
	out[1] = (m[0][2] - m[2][0]) / (2.f * s) * angle;
	out[2] = (m[1][0] - m[0][1]) / (2.f * s) * angle;
}

static void update_camera(void)
{
	unsigned long long now = n64_microseconds();
	const float *point;
	float dt, since, remaining, turn[3], step[3], angle;
	int command, k;

	if (!scene.point_count)
		return;
	if (!camera.started)
	{
		point = camera_point(camera_path[0]);
		memset(&camera, 0, sizeof(camera));
		memcpy(camera.position, point, sizeof(camera.position));
		memcpy(camera.forward, point + 3, sizeof(camera.forward));
		memcpy(camera.up, point + 6, sizeof(camera.up));
		camera.field_of_view = point[9];
		camera.last_us = now;
		camera.started = 1;
	}
	dt = (float)(now - camera.last_us) * 1e-6f;
	camera.last_us = now;
	/* (a long stall, a game in between: no leap) */
	dt = fminf(fmaxf(dt, 0.f), 0.1f);
	camera.time += dt;
	command = (int)(camera.time / CAMERA_INTERVAL);
	since = camera.time - command * CAMERA_INTERVAL;
	remaining = CAMERA_TRANSITION - since;
	point = camera_point(camera_path[command % CAMERA_PATH_LENGTH]);
	for (k = 0; k < 3; k++)
		camera.position[k] += glide(point[k] - camera.position[k], &camera.velocity[k], &camera.acceleration[k],
			remaining, dt);
	rotation_to(camera.forward, camera.up, point, turn);
	for (k = 0; k < 3; k++)
		step[k] = glide(turn[k], &camera.spin_velocity[k], &camera.spin_acceleration[k], remaining, dt);
	angle = sqrtf(step[0] * step[0] + step[1] * step[1] + step[2] * step[2]);
	if (angle > 1e-6f)
	{
		float axis[3] = {step[0] / angle, step[1] / angle, step[2] / angle};
		float left[3];

		rotate(camera.forward, axis, angle);
		rotate(camera.up, axis, angle);
		normalize(camera.forward);
		cross(camera.up, camera.forward, left);
		cross(camera.forward, left, camera.up);
		normalize(camera.up);
	}
	if (point[9] > 0.1f)
		camera.field_of_view = point[9];
}

/* the scene: the camera's view, the sky about the eye (its batches drawn
without depth: ui_dc.py MATERIAL_SKY), the ring */
static void draw_scene(void)
{
	static const unsigned char permutations[16];
	struct n64_view view;
	float *c = camera.position;
	int k;

	update_camera();
	memset(&view, 0, sizeof(view));
	for (k = 0; k < 3; k++)
	{
		view.position[k] = camera.position[k];
		view.forward[k] = camera.forward[k];
		view.up[k] = camera.up[k];
	}
	/* (the camera point's field of view is horizontal) */
	view.vertical_field_of_view = camera.field_of_view > 0.1f ?
		2.f * atanf(tanf(camera.field_of_view * 0.5f) * 0.75f) : 0.9f;
	view.z_near = 0.02f;
	view.z_far = 400.f;
	view.x1 = SCREEN_W;
	view.y1 = SCREEN_H;
	view.local_player_index = -1;
	n64_render_view(&view);
	if (scene.sky_model >= 0)
	{
		float matrix[13] = {1.f, 1, 0, 0, 0, 1, 0, 0, 0, 1, c[0], c[1], c[2]};
		struct n64_object sky = {
			.model_tag_index = (short)scene.sky_model, .node_count = 1, .node_matrices = matrix,
			.region_permutations = permutations, .region_count = 0,
			.center = {c[0], c[1], c[2]}, .radius = 100.f, .tint = {0xFF, 0xFF, 0xFF, 0xFF},
		};

		n64_render_object(&sky);
	}
	if (scene.ring_model >= 0)
	{
		struct n64_object ring = {
			.model_tag_index = (short)scene.ring_model, .node_count = 1, .node_matrices = scene.ring,
			.region_permutations = permutations, .region_count = 0,
			.center = {scene.ring[10], scene.ring[11], scene.ring[12]}, .radius = 40.f,
			.tint = {0xFF, 0xFF, 0xFF, 0xFF},
		};

		n64_render_object(&ring);
	}
}

/* ---------- drawing */

/* text with Halo's drop shadow (rasterizer_text.c: black at the text's
alpha, a pixel down and right); y the baseline */
static void text_centered(struct n64_font *font, int center, int y, unsigned long color, const char *text)
{
	int x = center - n64_text_width(font, text) / 2;

	n64_text_draw(font, x + 1, y + 1, color & 0xFF000000u, text);
	n64_text_draw(font, x, y, color, text);
}

/* the game loading: a band over the scene */
static void draw_loading(const struct n64_menu_choice *c)
{
	char text[64];

	static const char *const difficulties[4] = {"EASY", "NORMAL", "HEROIC", "LEGENDARY"};

	if (c->campaign)
		snprintf(text, sizeof(text), "%s  -  %s", maps[0].name, difficulties[c->difficulty & 3]);
	else
		snprintf(text, sizeof(text), "%s ON %s", game_types[c->game_type].name, maps[0].name);
	dc_hud_box(0.f, 0.f, (float)SCREEN_W, (float)SCREEN_H, 0x80000818u);
	dc_hud_box(0.f, 200.f, (float)SCREEN_W, 280.f, 0xC0000818u);
	text_centered(menu.large, SCREEN_W / 2, 236, TEXT_BRIGHT, "LOADING");
	text_centered(menu.small, SCREEN_W / 2, 264, TEXT_COLOR, text);
}

/* one frame: the flight, then Halo's widgets over it (or the loading band) */
static void menu_frame(const struct n64_menu_choice *c, int loading)
{
	dc_pace_menu_frame();
	n64_render_frame_begin();
	if (menu.scene)
		draw_scene();
	if (dc_hud_draw_begin())
	{
		if (loading)
			draw_loading(c);
		else
			dc_shell_draw();
		dc_hud_draw_end();
	}
	n64_render_frame_end();
	if (++menu.frame % 300 == 0)
		dc_music_statistics_line();
}

/* ---------- testing: the scripted presses ("debug_menu") */

static char script[64];
static int script_position = -1;

static void scripted_press(void)
{
	unsigned short buttons = 0;

	if (script_position < 0)
		script_position = n64_debug_text("debug_menu", script, sizeof(script)) ? 0 : (int)sizeof(script);
	if (script_position >= (int)sizeof(script) || !script[script_position] || menu.frame % 20)
		return;
	switch (script[script_position++])
	{
	case 'u': buttons = DC_PAD_UP; break;
	case 'd': buttons = DC_PAD_DOWN; break;
	case 'l': buttons = DC_PAD_LEFT; break;
	case 'r': buttons = DC_PAD_RIGHT; break;
	case 'a': buttons = DC_PAD_A; break;
	case 'b': buttons = DC_PAD_B; break;
	case 'x': buttons = DC_PAD_X; break;
	case 'y': buttons = DC_PAD_Y; break;
	case 's': buttons = DC_PAD_START; break;
	}
	if (buttons)
		dc_input_inject(0, buttons, 100);
}

/* ---------- the menus */

static void menu_load(void)
{
	char line[120];

	n64_render_initialize();
	/* ui.map's pictures and fonts, as a HUD (the game's map gives it back
	when it sets its own) */
	n64_hud_set_map("ui");
	menu.large = n64_font_load("large");
	menu.small = n64_font_load("small");
	/* the ring in space behind it (no level, no fog: black space) */
	n64_render_load_scene(NULL);
	menu.scene = scene_load() && dc_model_load("ui");
	n64_render_set_fog(0x000000, 0.f, 0.f);
	if (!menu.start_us)
		menu.start_us = n64_microseconds();
	/* (Halo's menus start it too: ui_start_main_menu_music) */
	n64_music_start();
	snprintf(line, sizeof(line), "dcu: menu up, fonts %s %s, %lu KB of video memory left",
		menu.large ? "large" : "-", menu.small ? "small" : "-", (unsigned long)pvr_mem_available() / 1024);
	n64_log(line);
}

/* the menus' things given back before the game's map comes in: the music,
the level, the pictures and fonts (the PowerVR done with them) */
static void menu_unload(void)
{
	n64_music_stop();
	n64_render_load_scene(NULL);
	menu.scene = 0;
	n64_hud_release();
	menu.large = menu.small = NULL;
}

/* runs the menus until a game is chosen; the game's results (after one)
are logged, the Xbox's own postgame screens being the game's */
int n64_menu_run(struct n64_menu_choice *c, const char *const *results, int result_count)
{
	int after_game = menu.opened && !c->to_title, i;
	char line[160];

	for (i = 0; i < result_count; i++)
	{
		snprintf(line, sizeof(line), "dcu: result %s", results[i]);
		n64_log(line);
	}
	c->to_title = 0;
	if (c->game_type < 0 || c->game_type >= GAME_TYPE_COUNT)
		c->game_type = 0;
	/* the boot intro, the first time only (dc_intro.c) */
	{
		extern void dc_intro_play(void);
		static int intro_played;

		if (!intro_played)
		{
			intro_played = 1;
			dc_intro_play();
		}
	}
	/* (the menus' pads are the ports: controller n is port n) */
	for (i = 0; i < 4; i++)
		n64_gamepad_ports[i] = i;
	n64_input_blocked = 1;
	dc_input_menu = 1;
	n64_input_clear_pressed();
	menu_load();
	if (!dc_shell_open(after_game))
		n64_fatal("Halo's menus are not on the disc (maps/ui.map: build-halo-dc.bat converts ui.map)");
	menu.opened = 1;
	for (;;)
	{
		scripted_press();
		/* (every Jump Pack still in the menus) */
		n64_input_rumble_menu_frame();
		dc_shell_frame();
		if (dc_shell_game_ready())
		{
			dc_shell_take_game(c);
			snprintf(line, sizeof(line), "dcu: %s on %s, score %d, %d players, %d bots",
				c->campaign ? "the campaign" : game_types[c->game_type].name, maps[0].name, c->score, c->player_count,
				c->bots);
			n64_log(line);
			/* (the loading band stays on the screen while the map loads) */
			menu_frame(c, 1);
			menu_frame(c, 1);
			dc_shell_close();
			menu_unload();
			n64_input_blocked = 0;
			dc_input_menu = 0;
			n64_input_clear_pressed();
			return 1;
		}
		menu_frame(c, 0);
	}
}

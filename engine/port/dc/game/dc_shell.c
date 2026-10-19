/*
DC_SHELL.C

What Halo's own menus (source/interface/ui_widget*.c, the Xbox's shell, run
by port/dc/src/dc_menu.c over ui.map) ask of the parts of the game the
Dreamcast leaves out.

Split screen. On the Xbox a split screen game is a network game of the box
with itself: a server and a client (networking/*.c), the players joining
through the client, the server counting down to the game. Here a local
lobby answers the shell's calls as they do, from the same record (struct
network_game, as ui_widget_game_data_input_functions.c reads it): the map
and the game variant chosen, the Dreamcast as the one machine, the players
who joined (each one's profile's name and colour, controller and team), and
the pregame countdown as network_server_manager.c keeps it -- 11 seconds
(0:10), 5 less for each player who joins (1 at least), A 5 less, X 5 more
(31 at most). The Dreamcast fills the game with bots, so one player is
enough to count down, and a team game needs no one on the other team.
When the countdown runs out the game is ready (dc_shell_game_ready): the
menu takes it (dc_shell_take_game) and port/n64/game/n64_game.c plays it,
the variant as it is (n64_game_set_shell_variant).

network_game_is_active is TRUE while the lobby is up and the menus run
(dc_shell_open to dc_shell_close), so that game_engine_playlist_begin does
not reset a map the menus are not on; in a game it is FALSE, as for any
local game.

The menus themselves (dc_shell_open, dc_shell_frame, dc_shell_draw,
dc_shell_close: port/dc/src/dc_menu.c's loop): ui.map's tags loaded as the
map's, with the scenario's and the HUD's globals Halo's menus read; the
pads through Halo's input and event manager to process_ui_widgets; the
widgets drawn by render_ui_widgets onto the HUD's quads (dc_hud_widgets).

The rest: system link, the Xbox demos, the dashboard, Bink movies (the boot
intro is dc_intro.c's, before the menus), the end credits and Halo's
scripts are not on the Dreamcast; the profile files' writer threads run at
once (create_thread); the title music is music/title.dca (n64_music_start:
scripted_looping_sound_start of sound\music\title1\title1).
*/

#include "cseries.h"

#include "cache/cache_files.h"
#include "game/game_engine.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "input/input.h"
#include "interface/event_manager.h"
#include "interface/hud.h"
#include "interface/interface.h"
#include "interface/player_ui.h"
#include "interface/ui_widget.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "main/main.h"
#include "render/render.h"
#include "saved games/player_profile.h"
#include "saved games/saved_game_files.h"
#include "sound/game_sound.h"
#include "tag_files/tag_files.h"
#include "text/unicode.h"

#include "../../n64/src/n64.h"
#include "errors.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct network_game_client;

boolean network_game_is_splitscreen_local(void);
boolean network_player_is_valid(struct network_player *player);
boolean network_game_client_request_remove_player(struct network_game_client *client, struct network_player *player);

/* ---------- the lobby's record (ui_widget_game_data_input_functions.c's
struct network_game, its layout from port/include/halo_session_limits.h) */

#define LOBBY_MACHINES HALO_PORT_MAXIMUM_NETWORK_MACHINES
#define LOBBY_PLAYERS HALO_PORT_MAXIMUM_NETWORK_PLAYERS
#define LOBBY_LOCAL_PLAYERS 4

/* network_server_manager.c's countdown (milliseconds) */
#define SPLITSCREEN_COUNTDOWN_TIME 10999
#define COUNTDOWN_TIME 30999
#define COUNTDOWN_ADJUSTMENT 5000
#define MINIMUM_COUNTDOWN_TIME 999

/* network_game_client_request_start_time_change's requests (the server's
countdown events) */
enum
{
	_lobby_slower,
	_lobby_faster,
	_lobby_stop,
	_lobby_start_immediately,
	_lobby_no_event
};

struct lobby_machine
{
	wchar_t name[32];
	char machine_index;
	byte pad41[3];
};

struct lobby_game
{
	byte pad000[0x24];
	char map_name[0x80];
	struct game_variant variant;
	byte pad10C;
	byte game_mode;
	byte maximum_player_count;
	byte pad10F;
	short difficulty;
	short machine_count;
	struct lobby_machine machines[LOBBY_MACHINES];
	short player_count;
	struct network_player players[LOBBY_PLAYERS];
	byte tail[HALO_PORT_NETWORK_GAME_SIZE - HALO_PORT_NETWORK_GAME_PLAYERS_END];
};

typedef char lobby_variant_offset_assert[offsetof(struct lobby_game, variant) == 0xA4 ? 1 : -1];
typedef char lobby_machines_offset_assert[
	offsetof(struct lobby_game, machines) == HALO_PORT_NETWORK_GAME_MACHINES_OFFSET ? 1 : -1];
typedef char lobby_players_offset_assert[
	offsetof(struct lobby_game, players) == HALO_PORT_NETWORK_GAME_PLAYERS_OFFSET ? 1 : -1];
typedef char lobby_size_assert[sizeof(struct lobby_game) == HALO_PORT_NETWORK_GAME_SIZE ? 1 : -1];
typedef char lobby_player_size_assert[sizeof(struct network_player) == HALO_PORT_NETWORK_PLAYER_SIZE ? 1 : -1];

static struct
{
	boolean server, client;
	/* the menus are up (dc_menu.c) */
	boolean menus_up;
	boolean accept_remote_connections, quickstart;
	/* the countdown: running, held by the shell (the map and game type
	screens), when it runs out (system_milliseconds) */
	boolean counting, paused;
	unsigned long countdown_end;
	/* the countdown ran out: the game is ready for the menu; a campaign
	level chosen (the campaign disc) */
	boolean ready, campaign;
	struct lobby_game game;
} lobby;

/* (the server and the client are the lobby: what the shell holds is only
compared with NULL) */
static byte server_handle, client_handle;

/* the advertised games of system link: none (never valid) */
static byte no_advertised_games[64][0xE4];

static void lobby_clear_players(
	void)
{
	long i;

	for (i = 0; i < LOBBY_PLAYERS; i++)
	{
		csmemset(&lobby.game.players[i], 0, sizeof(lobby.game.players[i]));
		lobby.game.players[i].machine_index = NONE;
		lobby.game.players[i].controller_index = NONE;
		lobby.game.players[i].team_index = NONE;
		lobby.game.players[i].player_list_index = NONE;
	}
	lobby.game.player_count = 0;
}

/* the record afresh: the Dreamcast the one machine, no players, the map
and the variant the shell chose (main.c's multiplayer map, player_ui.c's
variant: game_engine_playlist_next's) */
static void lobby_new_game(
	void)
{
	struct game_variant variant;
	char const *map_name = main_get_multiplayer_map_name();
	long i;

	csmemset(&lobby.game, 0, sizeof(lobby.game));
	for (i = 0; i < LOBBY_MACHINES; i++)
		lobby.game.machines[i].machine_index = NONE;
	ascii_to_wide("Dreamcast", lobby.game.machines[0].name, sizeof(lobby.game.machines[0].name));
	lobby.game.machines[0].machine_index = 0;
	lobby.game.machine_count = 1;
	lobby.game.maximum_player_count = LOBBY_LOCAL_PLAYERS;
	lobby_clear_players();
	if (map_name && map_name[0])
		csstrncpy(lobby.game.map_name, map_name, sizeof(lobby.game.map_name) - 1);
	if (player_ui_game_variant_specified(&variant))
		lobby.game.variant = variant;
	lobby.counting = lobby.paused = lobby.ready = FALSE;
}

static long lobby_time_remaining(
	void)
{
	long remaining = (long)(lobby.countdown_end - system_milliseconds());

	return remaining > 0 ? remaining : 0;
}

static void lobby_set_time_remaining(
	long milliseconds)
{
	lobby.countdown_end = system_milliseconds() + (unsigned long)milliseconds;
}

/* network_game_server_update_countdown, with the Dreamcast's rule: any
player at all may count down (the bots make up the rest) */
static void lobby_update_countdown(
	short event)
{
	if (!lobby.server || lobby.paused)
		return;
	if (lobby.game.player_count < 1 && event != _lobby_stop)
		return;
	if (lobby.counting)
	{
		switch (event)
		{
		case _lobby_slower:
			lobby_set_time_remaining(MIN(lobby_time_remaining() + COUNTDOWN_ADJUSTMENT, COUNTDOWN_TIME));
			break;
		case _lobby_faster:
			if (lobby_time_remaining() > MINIMUM_COUNTDOWN_TIME)
				lobby_set_time_remaining(MAX(lobby_time_remaining() - COUNTDOWN_ADJUSTMENT, MINIMUM_COUNTDOWN_TIME));
			break;
		case _lobby_stop:
			lobby.counting = FALSE;
			break;
		case _lobby_start_immediately:
			lobby_set_time_remaining(0);
			break;
		}
	}
	else if (event == _lobby_start_immediately)
	{
		lobby.counting = TRUE;
		lobby_set_time_remaining(0);
	}
	else if (event != _lobby_stop)
	{
		lobby.counting = TRUE;
		lobby_set_time_remaining(network_game_is_splitscreen_local() ? SPLITSCREEN_COUNTDOWN_TIME : COUNTDOWN_TIME);
	}
}

/* (a joining player's name and colour when their profile has none:
network_server_manager.c's get_unique_random_name and
get_unique_random_color) */
static boolean lobby_name_unique(
	wchar_t const *name)
{
	long i;

	for (i = 0; i < LOBBY_PLAYERS; i++)
	{
		if (network_player_is_valid(&lobby.game.players[i]) && !ustrcmp(lobby.game.players[i].name, name))
			return FALSE;
	}
	return TRUE;
}

static void lobby_random_name(
	struct network_player *player)
{
	extern wchar_t const *network_game_get_random_player_name(void);
	wchar_t const *name = L"Player";
	long attempt;

	for (attempt = 0; attempt < 32; attempt++)
	{
		name = network_game_get_random_player_name();
		if (name[0] && lobby_name_unique(name))
			break;
	}
	ustrncpy(player->name, name, NUMBEROF(player->name) - 1);
	player->name[NUMBEROF(player->name) - 1] = 0;
}

static void lobby_random_color(
	struct network_player *player)
{
	long attempt, color = 0, i;

	for (attempt = 0; attempt < 32; attempt++)
	{
		boolean unique = TRUE;

		color = attempt < 16 ? player_profile_get_random_good_color() : player_profile_get_random_color();
		for (i = 0; i < LOBBY_PLAYERS && unique; i++)
		{
			if (network_player_is_valid(&lobby.game.players[i]) && lobby.game.players[i].primary_color_index == color)
				unique = FALSE;
		}
		if (unique)
			break;
	}
	player->primary_color_index = (short)color;
}

/* the team a joining player starts on: the smaller one (red if even) */
static char lobby_team_for_new_player(
	void)
{
	long counts[2] = {0, 0}, i;

	for (i = 0; i < LOBBY_PLAYERS; i++)
	{
		struct network_player const *player = &lobby.game.players[i];

		if (network_player_is_valid((struct network_player *)player) && player->team_index >= 0 && player->team_index < 2)
			counts[player->team_index]++;
	}
	return counts[1] < counts[0] ? 1 : 0;
}

/* ---------- the menus' side (port/dc/src/dc_menu.c) */

/* Halo's menus are drawing (n64_hud_game.c draws their bitmaps as the
widgets blend them: dc_hud.c) */
extern int dc_hud_widgets;

/* the menus up: ui.map's tags as the loaded map's (maps/ui.map:
tools/n64/convert.py --dc --ui), its sounds (the clicks); the main menu the
first time, after a game the map select screen with the players still
joined (network_game_reset_to_pregame_ui: the Xbox's after a split screen
game). 0 if ui.map is not there */
int dc_shell_open(
	int after_game)
{
	char line[96];
	long scenario_index = scenario_tags_load("levels\\ui\\ui");

	if (scenario_index == NONE)
	{
		n64_log("dcs: no maps/ui.map: no menus");
		return 0;
	}
	/* (the scenario's and the HUD's globals, as scenario_load and
	hud_initialize_for_new_map set them: the menus read the interface's
	bitmaps, the buttons' icons in their text, through them) */
	global_scenario_index = scenario_index;
	global_scenario = scenario_definition_get(scenario_index);
	global_game_globals = game_globals_definition_get(tag_loaded(GAME_GLOBALS_TAG, "globals\\globals"));
	hud_globals = (struct hud_globals_definition *)tag_get('hudg', interface_get_tag_index(_interface_hud_globals));
	game_sound_initialize_for_new_map();
	main_menu_active(TRUE);
	lobby.menus_up = TRUE;
	lobby.ready = FALSE;
	/* (the map list starts on Blood Gulch, the one there is: the last map
	played, as the Xbox's starts) */
	{
		char last_map[256];

		if (!saved_game_file_retrieve_last_used_multiplayer_map(last_map))
			saved_game_file_remember_last_used_multiplayer_map("levels\\test\\bloodgulch\\bloodgulch");
	}
	if (after_game && lobby.server && lobby.client)
		network_game_reset_to_pregame_ui();
	else
		main_screen_shell_load();
	snprintf(line, sizeof(line), "dcs: Halo's menus up (%s)", after_game ? "after a game" : "the main menu");
	n64_log(line);
	return 1;
}

/* the menus down before the game's map comes in: every widget closed,
ui.map's sounds and tags given back */
void dc_shell_close(
	void)
{
	ui_widgets_close_all();
	main_menu_active(FALSE);
	lobby.menus_up = FALSE;
	game_sound_dispose_from_old_map();
	scenario_tags_unload();
	global_scenario_index = NONE;
	global_scenario = NULL;
	global_game_globals = NULL;
	hud_globals = NULL;
}

/* a frame of the menus' input, as main.c's loop has it: the pads, their
events, the widgets; then the lobby's countdown (and the game, when it
runs out) */
void dc_shell_frame(
	void)
{
	input_frame_begin();
	input_update();
	event_manager_update();
	process_ui_widgets();
	input_frame_end();
	if (lobby.server && lobby.client)
	{
		lobby_update_countdown(_lobby_no_event);
		if (lobby.counting && !lobby_time_remaining() && lobby.game.player_count > 0)
			lobby.ready = TRUE;
	}
}

/* the widgets over the frame (640x480: the Dreamcast's pixels) */
void dc_shell_draw(
	void)
{
	rectangle2d bounds;

	bounds.x0 = 0;
	bounds.y0 = 0;
	bounds.x1 = 640;
	bounds.y1 = 480;
	render.camera.viewport_bounds = bounds;
	render.camera.window_bounds = bounds;
	dc_hud_widgets = TRUE;
	render_ui_widgets(0, &bounds);
	dc_hud_widgets = FALSE;
}

int dc_shell_game_ready(
	void)
{
	return lobby.ready || lobby.campaign;
}

/* the campaign disc: a level started from Halo's campaign menus (its one
level, at the difficulty they set: ui_widget_event_handler_functions.c
dc_campaign_level) */
void dc_shell_start_campaign(
	void)
{
	lobby.campaign = TRUE;
}

/* a local player's name in the game (main.c create_local_players): the one
they joined the lobby with, on that controller; FALSE if none */
boolean dc_shell_player_name(
	short port,
	wchar_t *name)
{
	long i;

	for (i = 0; i < LOBBY_PLAYERS; i++)
	{
		struct network_player const *player = &lobby.game.players[i];

		if (network_player_is_valid((struct network_player *)player) && player->controller_index == port && player->name[0])
		{
			ustrncpy(name, player->name, NUMBEROF(player->name) - 1);
			name[NUMBEROF(player->name) - 1] = 0;
			return TRUE;
		}
	}
	return FALSE;
}

/* the DC menu's game type for a variant's engine and teams (dc_menu.c's
table: Slayer, Team Slayer, CTF, King, Team King, Oddball, Team Oddball,
Race, Team Race) */
static int lobby_game_type(
	long engine,
	boolean teams)
{
	switch (engine)
	{
	case game_engine_ctf: return 4;
	case game_engine_king: return teams ? 6 : 5;
	case game_engine_oddball: return teams ? 8 : 7;
	case game_engine_race: return teams ? 10 : 9;
	}
	return teams ? 1 : 0;
}

/* the ready game as the DC menu's choice: the players by controller, their
teams and colours, the map (Blood Gulch, the one there is), the variant
(n64_game.c plays it as it is), bots to make four */
void dc_shell_take_game(
	struct n64_menu_choice *choice)
{
	extern void n64_game_set_shell_variant(struct game_variant const *variant);
	struct game_variant const *variant = &lobby.game.variant;
	boolean teams = variant->universal_variant.teams != FALSE;
	char line[160];
	long controller, i;

#ifdef HALO_N64_CAMPAIGN
	/* (the campaign disc's level: one player, the first pad's, at the
	difficulty the menus set) */
	if (lobby.campaign)
	{
		extern short global_difficulty_level;
		short controller_index = player_ui_get_single_player_local_player_controller(0);

		lobby.campaign = FALSE;
		choice->campaign = 1;
		choice->map = 0;
		choice->firefight = 0;
		choice->to_title = 0;
		choice->player_count = 1;
		choice->ports[0] = controller_index >= 0 && controller_index < 4 ? controller_index : 0;
		choice->teams[0] = 0;
		choice->colors[0] = 2;
		choice->difficulty = PIN(global_difficulty_level, 0, 3);
		snprintf(line, sizeof(line), "dcs: the campaign, difficulty %d, port %d", choice->difficulty, choice->ports[0]);
		n64_log(line);
		return;
	}
#endif

	choice->player_count = 0;
	for (controller = 0; controller < LOBBY_LOCAL_PLAYERS; controller++)
	{
		for (i = 0; i < LOBBY_PLAYERS; i++)
		{
			struct network_player const *player = &lobby.game.players[i];

			if (network_player_is_valid((struct network_player *)player) && player->controller_index == controller)
			{
				int n = choice->player_count++;

				choice->ports[n] = controller;
				choice->teams[n] = teams ? PIN(player->team_index, 0, 1) : 0;
				choice->colors[n] = teams ? (choice->teams[n] ? 3 : 2) : player->primary_color_index;
				break;
			}
		}
	}
	if (!choice->player_count)
	{
		choice->player_count = 1;
		choice->ports[0] = 0;
		choice->teams[0] = 0;
		choice->colors[0] = 2;
	}
	choice->map = MAX(0, n64_menu_map_from_folder(tag_name_strip_path(lobby.game.map_name)));
	choice->game_type = lobby_game_type(variant->game_engine_index, teams);
	choice->score = variant->universal_variant.score_to_win;
	choice->bots = MAX(1, 4 - choice->player_count);
	choice->firefight = 0;
	choice->campaign = 0;
	choice->to_title = 0;
	n64_game_set_shell_variant(variant);
	snprintf(line, sizeof(line), "dcs: %s, engine %ld%s, score %d, %d players, %d bots", lobby.game.map_name,
		(long)variant->game_engine_index, teams ? " (teams)" : "", choice->score, choice->player_count, choice->bots);
	n64_log(line);
	lobby.ready = FALSE;
	lobby.counting = FALSE;
}

/* ---------- networking/network_game_globals.c's */

boolean network_game_is_active(
	void)
{
	return lobby.menus_up && lobby.server && lobby.client;
}

struct network_game *network_game_get_game(
	void)
{
	return lobby.client ? (struct network_game *)&lobby.game : NULL;
}

boolean network_game_player_is_local(
	struct network_player *player)
{
	return network_player_is_valid(player) && player->machine_index == 0;
}

void network_game_accept_remote_connections(
	boolean accept_remote_connections)
{
	lobby.accept_remote_connections = accept_remote_connections;
}

boolean network_game_should_accept_remote_connections(
	void)
{
	return lobby.accept_remote_connections;
}

/* (the Dreamcast's only kind) */
boolean network_game_is_splitscreen_local(
	void)
{
	return TRUE;
}

void network_game_set_quickstart_local(
	void)
{
	lobby.quickstart = TRUE;
}

boolean network_game_is_quickstart_local(
	void)
{
	return lobby.quickstart;
}

struct network_game_server *global_network_game_server_get(
	void)
{
	return lobby.server ? (struct network_game_server *)&server_handle : NULL;
}

boolean create_global_network_game_server(
	void)
{
	lobby_new_game();
	lobby.server = TRUE;
	return TRUE;
}

void dispose_global_network_game_server(
	void)
{
	lobby.server = FALSE;
	lobby.counting = lobby.paused = lobby.ready = FALSE;
	lobby.quickstart = FALSE;
}

struct network_game_client *global_network_game_client_get(
	void)
{
	return lobby.client ? (struct network_game_client *)&client_handle : NULL;
}

boolean create_global_network_game_client(
	void)
{
	lobby.client = TRUE;
	return TRUE;
}

void dispose_global_network_game_client(
	void)
{
	lobby.client = FALSE;
	lobby_clear_players();
}

short network_game_client_get_local_machine_index(
	void)
{
	return lobby.client ? 0 : NONE;
}

void network_game_client_local_player_quit(
	short controller_index)
{
	long i;

	for (i = 0; i < LOBBY_PLAYERS; i++)
	{
		if (network_player_is_valid(&lobby.game.players[i]) && lobby.game.players[i].controller_index == controller_index)
			network_game_client_request_remove_player(NULL, &lobby.game.players[i]);
	}
}

void network_game_abort(
	void)
{
	dispose_global_network_game_client();
	dispose_global_network_game_server();
}

void network_game_generate_join_game_token(
	byte *join_token)
{
	csmemset(join_token, 0, 0x12);
}

/* ---------- networking/network_game_manager.c's */

boolean network_player_is_valid(
	struct network_player *player)
{
	return player && player->controller_index >= 0 && player->controller_index < LOBBY_LOCAL_PLAYERS &&
		player->machine_index >= 0 && (long)player->machine_index < LOBBY_MACHINES;
}

void network_event(
	char *format,
	...)
{
	char line[200];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	error(_error_silent, "network: %s", line);
}

/* ---------- networking/network_client_manager.c's */

short network_game_client_get_state(
	struct network_game_client *client,
	short *state_data)
{
	(void)client;
	if (state_data)
		*state_data = 0;
	/* (_network_game_client_state_pregame) */
	return lobby.client ? 2 : 0;
}

struct network_game *network_game_client_get_game(
	struct network_game_client *client)
{
	(void)client;
	return (struct network_game *)&lobby.game;
}

short network_game_client_get_machine_index(
	struct network_game_client *client)
{
	(void)client;
	return 0;
}

struct network_machine *network_game_client_get_machine(
	struct network_game_client *client)
{
	(void)client;
	return (struct network_machine *)&lobby.game.machines[0];
}

boolean network_game_client_advertised_game_is_valid(
	struct network_advertised_game *advertised_game)
{
	(void)advertised_game;
	return FALSE;
}

struct network_advertised_game *network_game_client_get_available_games(
	struct network_game_client *client)
{
	(void)client;
	return (struct network_advertised_game *)no_advertised_games;
}

short network_game_client_get_seconds_to_game_start(
	struct network_game_client *client)
{
	(void)client;
	if (!lobby.server || !lobby.counting)
		return NONE;
	return (short)(lobby_time_remaining() / 1000);
}

/* a local player joins: their active profile's name and colour (player_ui.c),
their controller, a team (the smaller); the countdown 5 seconds shorter */
boolean network_game_client_add_player(
	struct network_game_client *client,
	short local_player_index)
{
	struct player_profile profile;
	struct network_player *player = NULL;
	long i;

	(void)client;
	if (!lobby.client || local_player_index < 0 || local_player_index >= LOBBY_LOCAL_PLAYERS)
		return FALSE;
	for (i = 0; i < LOBBY_PLAYERS; i++)
	{
		if (network_player_is_valid(&lobby.game.players[i]) &&
			lobby.game.players[i].controller_index == local_player_index)
		{
			return TRUE;
		}
		if (!player && !network_player_is_valid(&lobby.game.players[i]))
			player = &lobby.game.players[i];
	}
	if (!player)
		return FALSE;
	csmemset(&profile, 0, sizeof(profile));
	player_ui_get_active_player_profile(local_player_index, &profile);
	csmemset(player, 0, sizeof(*player));
	/* (no one yet, while the others' names and colours are compared) */
	player->machine_index = NONE;
	player->controller_index = NONE;
	ustrncpy(player->name, profile.player_name, NUMBEROF(player->name) - 1);
	player->name[NUMBEROF(player->name) - 1] = 0;
	player->primary_color_index = profile.primary_color_index;
	/* (a default profile has neither: the Xbox's server gives a random name
	no one else has, and a colour: network_server_manager.c) */
	if (!player->name[0] || !lobby_name_unique(player->name))
		lobby_random_name(player);
	if (player->primary_color_index == NONE)
		lobby_random_color(player);
	player->icon_index = 0;
	player->machine_index = 0;
	player->controller_index = (char)local_player_index;
	player->team_index = lobby_team_for_new_player();
	player->player_list_index = (char)(player - lobby.game.players);
	lobby.game.player_count++;
	lobby_update_countdown(_lobby_faster);
	return TRUE;
}

boolean network_game_client_request_remove_player(
	struct network_game_client *client,
	struct network_player *player)
{
	(void)client;
	if (!network_player_is_valid(player))
		return FALSE;
	player->machine_index = NONE;
	player->controller_index = NONE;
	player->team_index = NONE;
	lobby.game.player_count = (short)MAX(0, lobby.game.player_count - 1);
	lobby_update_countdown(lobby.game.player_count ? _lobby_slower : _lobby_stop);
	return TRUE;
}

/* a player's record changed by the shell (their team: swap player team) */
boolean network_game_client_update_local_player_data(
	struct network_game_client *client,
	struct network_player *player)
{
	long i;

	(void)client;
	for (i = 0; i < LOBBY_PLAYERS; i++)
	{
		if (network_player_is_valid(&lobby.game.players[i]) &&
			lobby.game.players[i].controller_index == player->controller_index)
		{
			lobby.game.players[i].team_index = player->team_index;
			return TRUE;
		}
	}
	return FALSE;
}

boolean network_game_client_request_start_time_change(
	struct network_game_client *client,
	short request_type)
{
	(void)client;
	lobby_update_countdown(request_type);
	return TRUE;
}

boolean network_game_client_initiate_join_game(
	struct network_game_client *client,
	struct network_advertised_game *game,
	void *join_parameters,
	void *address)
{
	(void)client;
	(void)game;
	(void)join_parameters;
	(void)address;
	return FALSE;
}

/* ---------- networking/network_server_manager.c's */

void network_game_server_open_game(
	struct network_game_server *server)
{
	(void)server;
}

void network_game_server_pause_countdown(
	struct network_game_server *server,
	boolean pause_countdown)
{
	(void)server;
	if (pause_countdown)
		lobby.counting = FALSE;
	lobby.paused = pause_countdown;
}

void network_game_server_change_map_name(
	struct network_game_server *server,
	char const *map_name)
{
	(void)server;
	csstrncpy(lobby.game.map_name, map_name, sizeof(lobby.game.map_name) - 1);
	lobby.game.map_name[sizeof(lobby.game.map_name) - 1] = 0;
}

void network_game_server_change_game_variant(
	struct network_game_server *server,
	struct game_variant *variant)
{
	(void)server;
	lobby.game.variant = *variant;
}

/* ---------- system link (bungie_net/network/transport_*.c): no network */

boolean transport_network_available(
	void)
{
	return FALSE;
}

void transport_client_start(
	void const *xnaddr,
	void const *key,
	void const *key_id,
	word port)
{
	(void)xnaddr;
	(void)key;
	(void)key_id;
	(void)port;
}

/* ---------- bungie_net/common/thread.c's: run at once */

struct thread_reference
{
	boolean exited;
};

struct mutex_reference
{
	long count;
};

static struct thread_reference finished_thread = {TRUE};
static struct mutex_reference shell_mutexes[8];
static long shell_mutex_count;

boolean create_thread(
	word flags,
	unsigned long (__stdcall *function)(void *),
	void *function_input,
	struct thread_reference **thread_reference)
{
	(void)flags;
	if (function)
		function(function_input);
	if (thread_reference)
		*thread_reference = &finished_thread;
	return TRUE;
}

boolean thread_has_exited(
	struct thread_reference *thread_reference)
{
	(void)thread_reference;
	return TRUE;
}

void dispose_thread(
	struct thread_reference *thread_reference)
{
	(void)thread_reference;
}

boolean create_mutex(
	struct mutex_reference **mutex_reference)
{
	*mutex_reference = &shell_mutexes[shell_mutex_count < (long)NUMBEROF(shell_mutexes) ? shell_mutex_count++ : 0];
	return TRUE;
}

boolean take_mutex(
	struct mutex_reference *mutex_reference,
	unsigned long timeout_ms)
{
	(void)timeout_ms;
	if (mutex_reference)
		mutex_reference->count++;
	return TRUE;
}

void release_mutex(
	struct mutex_reference *mutex_reference)
{
	if (mutex_reference && mutex_reference->count > 0)
		mutex_reference->count--;
}

void dispose_mutex(
	struct mutex_reference *mutex_reference)
{
	(void)mutex_reference;
}

/* ---------- the rest */

/* (the boot intro is dc_intro.c's, before the menus) */
boolean bink_playback_in_progress(
	void)
{
	return FALSE;
}

void bink_playback_start(
	char const *full_pathname,
	unsigned long flags)
{
	(void)full_pathname;
	(void)flags;
}

boolean bink_playback_ui_rendering_inhibited(
	void)
{
	return FALSE;
}

/* a box of one colour over the screen (cinematics.c's, which the Dreamcast
leaves out: the menus' fade to black) */
void draw_quad(
	rectangle2d *rectangle,
	pixel32 color)
{
	extern void dc_hud_box(float x0, float y0, float x1, float y1, unsigned long argb);

	dc_hud_box(rectangle->x0, rectangle->y0, rectangle->x1, rectangle->y1, color);
}

void game_state_write_to_persistent_storage(
	void *buffer,
	unsigned long *header_checksum,
	long header_size,
	long buffer_size)
{
	(void)buffer;
	(void)header_checksum;
	(void)header_size;
	(void)buffer_size;
}

void game_state_create_persistent_storage(
	char const *path)
{
	(void)path;
}

char const *game_state_get_persistent_storage_filename(
	void)
{
	return "";
}

struct halo_ui_pointer;

int halo_ui_pointer_update(
	int menus_active,
	struct halo_ui_pointer *pointer)
{
	(void)menus_active;
	(void)pointer;
	return 0;
}

boolean hs_evaluate_by_name(
	char const *name)
{
	(void)name;
	return FALSE;
}

/* the main menu's music (ui_start_main_menu_music: sound\music\title1\
title1): Halo's title music, music/title.dca */
void scripted_looping_sound_start(
	long sound_index,
	long source_object_index,
	real gain)
{
	(void)sound_index;
	(void)source_object_index;
	(void)gain;
	n64_music_start();
}

void scripted_looping_sound_stop(
	long sound_index)
{
	(void)sound_index;
	n64_music_stop();
}

void sound_pause(
	boolean paused)
{
	n64_sound_pause(paused);
}

void xbox_dashboard_launch(
	void)
{
}

boolean xbox_demos_available(
	void)
{
	return FALSE;
}

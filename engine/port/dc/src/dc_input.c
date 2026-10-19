/*
DC_INPUT.C

The four controller ports (KallistiOS's maple controllers) for the
XDK-facing gamepad layer (port/n64/src/xdk_input.c), and their Jump Packs:
port/n64/src/n64_input.c's API. Each Dreamcast pad is read as an N64 pad
(struct n64_pad, N64_BUTTON_*), its buttons laid out so that the N64's
schemes (n64_controls, n64.h) play on it unchanged.

The Dreamcast pad has one stick, A B X Y, the D-pad, START and the two
analog triggers: eleven inputs for the N64's fourteen. With the left thumb
on the stick and the right on A B X Y, only the triggers can be pressed
while looking and moving, so they take fire and grenades (Halo's own right
and left triggers); the D-pad (the left thumb off the stick) takes jump,
melee, reload and the weapon switch; START, tapped, zooms, and held with the
D-pad gives the rest. The standard scheme (N64_SCHEME_STANDARD):

    stick           look                       (N64 stick)
    Y / A           forward / back             (C up / C down)
    X / B           strafe left / right        (C left / C right)
    R trigger       fire                       (Z)
    L trigger       throw a grenade            (R)
    D-pad up        jump                       (A)
    D-pad right     melee                      (L)
    D-pad left      reload / action; held,
                    pick up / swap             (B)
    D-pad down      switch weapon              (D-pad up)
    START tapped    zoom                       (D-pad left, a short press)
    START + D-down  crouch (toggled)           (D-pad down)
    START + D-up    flashlight                 (D-pad right: the white button)
    START + D-left  switch grenades            (N64_BUTTON_BLACK: the black button)
    START + D-right zoom, held                 (D-pad left)
    START held      START (the pause menu)

Testing setting "dc_scheme=north": the north scheme instead, the stick
walking and strafing and A B X Y looking (Y up, A down, X left, B right);
the rest as above. "dc_invert": look inverted; "dc_crouch_hold": crouch
held (START + D-down) rather than toggled; "dc_no_rumble": no Jump Pack.

A pad with more (an arcade stick, a six-button pad) has its extra buttons
read too: Z zooms, C crouches, D switches grenades.

The pause menu (port/n64/game/n64_game.c n64_game_pause_frame) and the
main menu (dc_menu.c) read the buttons pressed since they last asked
(n64_input_take_pressed) in the Dreamcast's own sense, not the game's: A
selects, B goes back, the D-pad moves; START opens the pause menu only
after a hold (a tap zooms); with n64_input_blocked (the pause menu up, the
main menu) a press is START at once. The pause menu's screens are
dc_hud.c's (n64_hud_pause, n64_hud_pause_controls). While it is up, START
resumes at once. Testing setting "dc_no_pause": START held opens nothing;
"dc_pause_test": the pause screens without hands, port A's menu buttons
pressed on a timetable from the first poll (PAUSE_TEST below: START at 25
s, A at 33 s -- the controls page --, B at 41 s, START at 49 s).

The Jump Pack (a puru puru in the controller's slot 1 or 2): Halo's two
motor speeds (from XInputSetState, once a frame) as one strength, the
heavy motor's whole and the light one's three quarters (as on the N64),
sent as a continuous vibration of 1-7 when it changes. KallistiOS waits
for the pack's answer (purupuru_rumble: up to a frame), so the sends are a
thread's of their own, never the game's; it stops a pack the game has not
asked about for a quarter of a second (a map loading, the game stopped).
*/

#include <kos.h>
#include <string.h>

#include "n64.h"

#define PORT_COUNT 4

/* the stick: -128 to 127 (KallistiOS's, up negative); the N64's reads
about 80 at full tilt (xdk_input.c stick), the Dreamcast's about 100-127.
A little dead centre for a worn stick (Halo has its own besides,
GAMEPAD_STICK_DEAD_RANGE) */
#define STICK_DEAD 8
#define STICK_FULL 104
#define N64_STICK_FULL 80
#define N64_STICK_MAX 85

/* the triggers, 0-255: pressed from this far, released below that */
#define TRIGGER_ON 96
#define TRIGGER_OFF 48

/* START: a press shorter than this (with no D-pad) zooms; one held this
long (with no D-pad) is START; the zoom's press lasts this long (a few of
the game's 30 Hz ticks: Halo zooms on the first tick it sees one) */
#define START_TAP_US 350000ULL
#define START_HOLD_US 600000ULL
#define ZOOM_PULSE_US 120000ULL

struct dc_port
{
	struct n64_pad pad;           /* as n64_input_get reports it */
	struct dc_raw_pad raw;        /* as the pad is (dc_input_raw) */
	unsigned short menu_buttons;  /* the menu's N64_BUTTON_*, last poll */
	unsigned char ltrig_on, rtrig_on;
	unsigned char start_down;     /* START held */
	unsigned char start_chorded;  /* (this hold) used with the D-pad, or resumed a pause */
	unsigned char start_long;     /* (this hold) became START */
	unsigned long long start_time;
	unsigned long long zoom_until;
};

static struct dc_port ports[PORT_COUNT];
/* (testing: dc_input_inject's buttons, held until then) */
static unsigned short injected[PORT_COUNT];
static unsigned long long injected_until[PORT_COUNT];

void dc_input_inject(int port, unsigned short buttons, unsigned milliseconds)
{
	if (port < 0 || port >= PORT_COUNT)
		return;
	injected[port] = buttons;
	injected_until[port] = n64_microseconds() + milliseconds * 1000ULL;
}
/* START, A, B and D-pad pressed since n64_input_take_pressed last asked, each
port (the pause menu's: a frame at 10 fps is longer than a quick press) */
static unsigned short pressed_latch[PORT_COUNT];
static int set_up, pause_enabled;

/* testing setting "dc_pause_test": port A's menu presses, seconds from the
first poll */
static const struct
{
	unsigned seconds;
	unsigned short buttons;
} PAUSE_TEST[] = {
	{25, N64_BUTTON_START}, {33, N64_BUTTON_A}, {41, N64_BUTTON_B}, {49, N64_BUTTON_START},
};
static int pause_test = -1;
static unsigned long long pause_test_start;

/* the testing settings (dc_files.c dc_settings), once */
static void set_up_controls(void)
{
	char text[16];
	int north = n64_debug_text("dc_scheme", text, sizeof(text)) && !strcmp(text, "north");
	int port;

	set_up = 1;
	pause_enabled = !n64_debug_flag("dc_no_pause");
	pause_test = n64_debug_flag("dc_pause_test") ? 0 : -1;
	pause_test_start = n64_microseconds();
	for (port = 0; port < PORT_COUNT; port++)
	{
		struct n64_controls *controls = &n64_controls[port];

		if (north)
			controls->scheme = N64_SCHEME_NORTH;
		/* (crouch on a chord: toggled, unless asked otherwise; a jump
		stands back up, xdk_input.c) */
		controls->toggle_crouch = !n64_debug_flag("dc_crouch_hold");
		if (n64_debug_flag("dc_invert"))
			controls->inverted = 1;
		if (n64_debug_flag("dc_no_rumble"))
			controls->no_rumble = 1;
	}
}

static signed char stick(int value)
{
	int size = value < 0 ? -value : value;

	if (size <= STICK_DEAD)
		return 0;
	size = (size - STICK_DEAD) * N64_STICK_FULL / (STICK_FULL - STICK_DEAD);
	if (size > N64_STICK_MAX)
		size = N64_STICK_MAX;
	return (signed char)(value < 0 ? -size : size);
}

static void disconnect(struct dc_port *port)
{
	memset(port, 0, sizeof(*port));
}

/* one port's pad, at time now */
static void read_port(int index, unsigned long long now)
{
	struct dc_port *port = &ports[index];
	maple_device_t *device = maple_enum_dev(index, 0);
	cont_state_t state, *status;
	unsigned short buttons = 0, menu = 0;
	uint32_t dpad;
	int start;

	/* (maple_dev_status waits for the first report: asked only after it) */
	if (!device || !(device->info.functions & MAPLE_FUNC_CONTROLLER) || !device->status_valid ||
		!(status = (cont_state_t *)maple_dev_status(device)))
	{
		disconnect(port);
		return;
	}
	/* (a copy: the maple interrupt rewrites the status every frame) */
	state = *status;

	/* the menu's buttons, the Dreamcast's own; the first report is no press */
	if (state.buttons & CONT_A) menu |= N64_BUTTON_A;
	if (state.buttons & CONT_B) menu |= N64_BUTTON_B;
	if (state.buttons & CONT_DPAD_UP) menu |= N64_BUTTON_D_UP;
	if (state.buttons & CONT_DPAD_DOWN) menu |= N64_BUTTON_D_DOWN;
	if (state.buttons & CONT_DPAD_LEFT) menu |= N64_BUTTON_D_LEFT;
	if (state.buttons & CONT_DPAD_RIGHT) menu |= N64_BUTTON_D_RIGHT;
	if (!port->pad.connected)
		port->menu_buttons = menu;
	pressed_latch[index] |= menu & ~port->menu_buttons;
	port->menu_buttons = menu;

	port->pad.connected = 1;
	port->pad.stick_x = stick(state.joyx);
	port->pad.stick_y = stick(-state.joyy);

	/* (Halo's menus: the pad as it is) */
	port->raw.buttons = 0;
	if (state.buttons & CONT_A) port->raw.buttons |= DC_PAD_A;
	if (state.buttons & CONT_B) port->raw.buttons |= DC_PAD_B;
	if (state.buttons & CONT_X) port->raw.buttons |= DC_PAD_X;
	if (state.buttons & CONT_Y) port->raw.buttons |= DC_PAD_Y;
	if (state.buttons & CONT_START) port->raw.buttons |= DC_PAD_START;
	if (state.buttons & CONT_DPAD_UP) port->raw.buttons |= DC_PAD_UP;
	if (state.buttons & CONT_DPAD_DOWN) port->raw.buttons |= DC_PAD_DOWN;
	if (state.buttons & CONT_DPAD_LEFT) port->raw.buttons |= DC_PAD_LEFT;
	if (state.buttons & CONT_DPAD_RIGHT) port->raw.buttons |= DC_PAD_RIGHT;
	port->raw.stick_x = (signed char)(state.joyx < -128 ? -128 : state.joyx > 127 ? 127 : state.joyx);
	port->raw.stick_y = (signed char)(-state.joyy < -128 ? -128 : -state.joyy > 127 ? 127 : -state.joyy);
	port->raw.left_trigger = (unsigned char)state.ltrig;
	port->raw.right_trigger = (unsigned char)state.rtrig;
	if (now < injected_until[index])
		port->raw.buttons |= injected[index];

	/* A B X Y: the C buttons */
	if (state.buttons & CONT_Y) buttons |= N64_BUTTON_C_UP;
	if (state.buttons & CONT_A) buttons |= N64_BUTTON_C_DOWN;
	if (state.buttons & CONT_X) buttons |= N64_BUTTON_C_LEFT;
	if (state.buttons & CONT_B) buttons |= N64_BUTTON_C_RIGHT;

	/* the triggers: fire (Z) and grenades (R) */
	port->rtrig_on = state.rtrig >= (port->rtrig_on ? TRIGGER_OFF : TRIGGER_ON);
	port->ltrig_on = state.ltrig >= (port->ltrig_on ? TRIGGER_OFF : TRIGGER_ON);
	if (port->rtrig_on) buttons |= N64_BUTTON_Z;
	if (port->ltrig_on) buttons |= N64_BUTTON_R;

	dpad = state.buttons & (CONT_DPAD_UP | CONT_DPAD_DOWN | CONT_DPAD_LEFT | CONT_DPAD_RIGHT);
	start = (state.buttons & CONT_START) != 0;
	if (start)
	{
		if (!port->start_down)
		{
			port->start_down = 1;
			port->start_chorded = 0;
			port->start_long = 0;
			port->start_time = now;
			/* (the pause menu up: START resumes at once, and is spent) */
			if (n64_input_blocked)
			{
				pressed_latch[index] |= N64_BUTTON_START;
				port->start_chorded = 1;
			}
		}
		/* START and the D-pad: crouch, flashlight, grenades, zoom */
		if (dpad)
			port->start_chorded = 1;
		if (dpad & CONT_DPAD_DOWN) buttons |= N64_BUTTON_D_DOWN;
		if (dpad & CONT_DPAD_UP) buttons |= N64_BUTTON_D_RIGHT;
		if (dpad & CONT_DPAD_LEFT) buttons |= N64_BUTTON_BLACK;
		if (dpad & CONT_DPAD_RIGHT) buttons |= N64_BUTTON_D_LEFT;
		/* START held alone: START (the pause menu's) */
		if (!port->start_chorded && now - port->start_time >= START_HOLD_US)
		{
			if (!port->start_long)
			{
				port->start_long = 1;
				if (pause_enabled)
					pressed_latch[index] |= N64_BUTTON_START;
			}
			buttons |= N64_BUTTON_START;
		}
	}
	else
	{
		/* START tapped alone: zoom */
		if (port->start_down && !port->start_chorded && !port->start_long &&
			now - port->start_time < START_TAP_US)
		{
			port->zoom_until = now + ZOOM_PULSE_US;
		}
		port->start_down = 0;
		if (dpad & CONT_DPAD_UP) buttons |= N64_BUTTON_A;
		if (dpad & CONT_DPAD_RIGHT) buttons |= N64_BUTTON_L;
		if (dpad & CONT_DPAD_LEFT) buttons |= N64_BUTTON_B;
		if (dpad & CONT_DPAD_DOWN) buttons |= N64_BUTTON_D_UP;
	}
	if (now < port->zoom_until)
		buttons |= N64_BUTTON_D_LEFT;

	/* a pad with more buttons: Z zooms, C crouches, D switches grenades */
	if ((state.buttons & CONT_Z) && cont_has_capabilities(device, CONT_CAPABILITY_Z) > 0)
		buttons |= N64_BUTTON_D_LEFT;
	if ((state.buttons & CONT_C) && cont_has_capabilities(device, CONT_CAPABILITY_C) > 0)
		buttons |= N64_BUTTON_D_DOWN;
	if ((state.buttons & CONT_D) && cont_has_capabilities(device, CONT_CAPABILITY_D) > 0)
		buttons |= N64_BUTTON_BLACK;

	port->pad.buttons = buttons;
}

void n64_input_poll(void)
{
	unsigned long long now = n64_microseconds();
	int port;

	if (!set_up)
		set_up_controls();
	for (port = 0; port < PORT_COUNT; port++)
		read_port(port, now);
	if (pause_test >= 0 && pause_test < (int)(sizeof(PAUSE_TEST) / sizeof(PAUSE_TEST[0])) &&
		now - pause_test_start >= PAUSE_TEST[pause_test].seconds * 1000000ULL)
	{
		pressed_latch[0] |= PAUSE_TEST[pause_test++].buttons;
	}
}

void n64_input_get(int port, struct n64_pad *pad)
{
	if (port < 0 || port >= PORT_COUNT)
	{
		memset(pad, 0, sizeof(*pad));
		return;
	}
	*pad = ports[port].pad;
}

int dc_input_raw(int port, struct dc_raw_pad *pad)
{
	if (port < 0 || port >= PORT_COUNT || !ports[port].pad.connected)
	{
		memset(pad, 0, sizeof(*pad));
		return 0;
	}
	*pad = ports[port].raw;
	return 1;
}

unsigned short n64_input_take_pressed(int port)
{
	unsigned short pressed = pressed_latch[port & 3];

	pressed_latch[port & 3] = 0;
	return pressed;
}

void n64_input_clear_pressed(void)
{
	memset(pressed_latch, 0, sizeof(pressed_latch));
}

/* ---------- the buttons' names

What Halo's text calls an Xbox button (hud_messaging.c's icons, in their
order: A B X Y, black, white, the triggers, the D-pad, START, BACK, the
thumbs, the sticks), named as the Dreamcast pad has it for a local
player's scheme: the Xbox button is the N64 button xdk_input.c makes it
from (pad_to_gamepad, the scheme's and the fire swap's changes included),
and that is the pad's input read_port above makes it from. The prompts
(the pick-up, the vehicles', the scores') name the pad's buttons, not the
Xbox's. NULL: none (the Xbox's own D-pad, which the game never reads) */

static const char *n64_button_name(unsigned short button)
{
	switch (button)
	{
	case N64_BUTTON_A: return "D-PAD UP";
	case N64_BUTTON_B: return "D-PAD LEFT";
	case N64_BUTTON_L: return "D-PAD RIGHT";
	case N64_BUTTON_D_UP: return "D-PAD DOWN";
	case N64_BUTTON_Z: return "R TRIGGER";
	case N64_BUTTON_R: return "L TRIGGER";
	case N64_BUTTON_D_DOWN: return "START+D-PAD DOWN";
	case N64_BUTTON_D_RIGHT: return "START+D-PAD UP";
	case N64_BUTTON_BLACK: return "START+D-PAD LEFT";
	case N64_BUTTON_D_LEFT: return "START";
	case N64_BUTTON_START: return "START (HELD)";
	}
	return NULL;
}

const char *n64_input_button_name(int local, int xbox_button)
{
	extern int n64_gamepad_ports[4];
	const struct n64_controls *controls = &n64_controls[n64_gamepad_ports[local & 3] & 3];
	unsigned short jump = N64_BUTTON_A, grenade = N64_BUTTON_R, fire = N64_BUTTON_Z;
	int scheme = controls->scheme;

	/* (as pad_to_gamepad works them out) */
	if (scheme == N64_SCHEME_GOLDEN_STYLE)
		grenade = N64_BUTTON_D_LEFT;
	if (scheme == N64_SCHEME_BUMPER_JUMPER)
	{
		jump = N64_BUTTON_R;
		grenade = N64_BUTTON_A;
	}
	if (controls->swap_fire)
	{
		fire = grenade;
		grenade = N64_BUTTON_Z;
	}
	switch (xbox_button)
	{
	case 0: return n64_button_name(jump);                 /* A: jump */
	case 1: return n64_button_name(N64_BUTTON_L);         /* B: melee */
	case 2: return n64_button_name(N64_BUTTON_B);         /* X: action, reload */
	case 3: return n64_button_name(N64_BUTTON_D_UP);      /* Y: switch weapon */
	case 4: return n64_button_name(N64_BUTTON_BLACK);     /* black: switch grenades */
	case 5: return n64_button_name(N64_BUTTON_D_RIGHT);   /* white: flashlight */
	case 6: return n64_button_name(grenade);              /* left trigger */
	case 7: return n64_button_name(fire);                 /* right trigger */
	case 12: case 13: return n64_button_name(N64_BUTTON_START); /* START, BACK: the pause menu, the scores */
	case 14: return n64_button_name(N64_BUTTON_D_DOWN);   /* left thumb: crouch */
	case 15: return n64_button_name(N64_BUTTON_D_LEFT);   /* right thumb: zoom */
	/* the left stick moves, the right one looks */
	case 16: return scheme == N64_SCHEME_STANDARD || scheme == N64_SCHEME_BUMPER_JUMPER ? "A B X Y" : "STICK";
	case 17: return scheme == N64_SCHEME_NORTH ? "A B X Y" : "STICK";
	}
	return NULL;
}

/* ---------- the Jump Packs */

/* a strength below this: off (a motor barely turning is felt as nothing) */
#define RUMBLE_FLOOR 0.08f
/* the vibration's frequency: the heavy motor's lower, the light one's
higher (most packs take 4-59) */
#define RUMBLE_FREQUENCY_HEAVY 16
#define RUMBLE_FREQUENCY_LIGHT 32
/* a pack not asked about for this long is stopped */
#define RUMBLE_STALE_US 250000ULL

/* the vibration each port wants: its strength (1-7, 0 off) and frequency
in one word (written by the game, read by the rumble thread), and when */
#define RUMBLE_WANT(level, frequency) ((level) | ((frequency) << 8))
static volatile int rumble_want[PORT_COUNT];
static volatile unsigned long long rumble_time[PORT_COUNT];
static int rumble_pulse[PORT_COUNT];
static kthread_t *rumble_thread;

static maple_device_t *jump_pack(int port)
{
	int unit;

	for (unit = 1; unit < MAPLE_UNIT_COUNT; unit++)
	{
		maple_device_t *device = maple_enum_dev(port, unit);

		if (device && (device->info.functions & MAPLE_FUNC_PURUPURU))
			return device;
	}
	return NULL;
}

/* the sends, every 30 ms, only of a change (each waits for the pack) */
static void *rumble_main(void *argument)
{
	maple_device_t *sent_device[PORT_COUNT] = {NULL};
	int sent[PORT_COUNT];
	int port;

	(void)argument;
	for (port = 0; port < PORT_COUNT; port++)
		sent[port] = -1;
	for (;;)
	{
		unsigned long long now;

		thd_sleep(30);
		now = timer_us_gettime64();
		for (port = 0; port < PORT_COUNT; port++)
		{
			maple_device_t *device = jump_pack(port);
			int want = rumble_want[port];
			purupuru_effect_t effect;

			if (now - rumble_time[port] > RUMBLE_STALE_US)
				want = 0;
			if (!device)
			{
				sent_device[port] = NULL;
				continue;
			}
			/* (a pack plugged in: told, whatever was last sent) */
			if (device != sent_device[port])
			{
				sent_device[port] = device;
				sent[port] = -1;
			}
			if (want == sent[port])
				continue;
			/* (motor 1 always: 0 is an error on Sega's packs; a strength
			of 0 stops it) */
			effect.raw = 0;
			effect.motor = 1;
			if (want & 0xFF)
			{
				effect.cont = true;
				effect.fpow = want & 7;
				effect.freq = (uint8_t)(want >> 8);
			}
			if (purupuru_rumble(device, &effect) == MAPLE_EOK)
				sent[port] = want;
		}
	}
	return NULL;
}

static void rumble_set(int port, int want)
{
	rumble_time[port] = timer_us_gettime64();
	rumble_want[port] = want;
	if (want && !rumble_thread)
		rumble_thread = thd_create(true, rumble_main, NULL);
}

void n64_input_rumble(int port, float left, float right)
{
	float strength;
	int level;

	if (port < 0 || port >= PORT_COUNT)
		return;
	strength = left > right * 0.75f ? left : right * 0.75f;
	if (n64_controls[port].no_rumble || strength < RUMBLE_FLOOR)
	{
		rumble_set(port, 0);
		return;
	}
	if (strength > 1.f)
		strength = 1.f;
	level = 1 + (int)(strength * 6.f + 0.5f);
	if (level > 7)
		level = 7;
	rumble_set(port, RUMBLE_WANT(level, left >= right * 0.75f ? RUMBLE_FREQUENCY_HEAVY : RUMBLE_FREQUENCY_LIGHT));
}

int n64_input_rumble_present(int port)
{
	return port >= 0 && port < PORT_COUNT && jump_pack(port) != NULL;
}

void n64_input_rumble_pulse(int port, int frames)
{
	if (port >= 0 && port < PORT_COUNT)
		rumble_pulse[port] = frames;
}

/* once a frame outside a game (the menu): every pack off, but for the buzz
a player gets turning it on */
void n64_input_rumble_menu_frame(void)
{
	int port;

	for (port = 0; port < PORT_COUNT; port++)
	{
		rumble_set(port, rumble_pulse[port] > 0 ? RUMBLE_WANT(7, RUMBLE_FREQUENCY_HEAVY) : 0);
		if (rumble_pulse[port] > 0)
			rumble_pulse[port]--;
	}
}

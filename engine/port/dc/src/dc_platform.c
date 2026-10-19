/*
DC_PLATFORM.C

The n64.h functions whose do-nothing placeholders (dc_stubs_generated.c)
the game cannot take: a value it reads back, or an output it uses unset.
Each answers as the N64's own does: one 640x480 screen at the HUD's scale.
Replaced one by one as the Dreamcast's renderer and sound arrive (the
particles' are dc_fx.c's now, the sound's dc_sound.c's).
*/

#include <stddef.h>

#include "n64.h"

/* ---------- the HUD's layout (640x480, one to one: dc_hud.c draws it) */

float n64_render_layout_scale_x(void) { return 1.0f; }
float n64_render_layout_scale_y(void) { return 1.0f; }
float n64_render_hud_squeeze(void) { return 1.0f; }

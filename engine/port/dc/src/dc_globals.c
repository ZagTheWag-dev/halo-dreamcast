/*
DC_GLOBALS.C

The N64 platform layer's variables the game side reads (defined in
port/n64/src/n64_*.c on the N64): the controller settings, the profiler's
zone, the frame statistics the update rates follow.
*/

#include "n64.h"

struct n64_controls n64_controls[4];
volatile int n64_profile_zone;
/* 0: animations and physics at the full rate (the N64 halves them) */
int n64_half_animation_rate;
unsigned long n64_last_tick_us;
int n64_view_total;

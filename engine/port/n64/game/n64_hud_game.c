/*
N64_HUD_GAME.C

The HUD as the game draws it: the rasterizer calls interface/hud*.c,
motion_sensor.c and game_engine.c make, turned into the N64 HUD's (n64.h,
port/n64/src/n64_hud.c). The game lays the HUD out on a 640x480 screen;
the N64 shows 320x240, and the HUD's bitmaps are half size
(tools/n64/hud.py), so positions and texels both halve.

A bitmap the converter exported carries its sprite's number + 1 in
pixels_offset; the others have none, and the game skips them
(_texture_cache_bitmap_get_hardware_format returns NULL).

The Dreamcast (HALO_DC) shows the game's 640x480 as it is (the layout's
scale 1, its sprites at their own size: tools/n64/hud_dc.py) and draws
Halo's text as Halo does: draw_string.c lays each string out in the font,
colour, justification, tabs and clip the game gave it, and each glyph goes
to n64_hud_glyph with its drop shadow (rasterizer_text.c's), its number in
the Dreamcast's font atlas carried in the font character's pixels offset.
*/

#include "cseries.h"

#include "bitmaps/bitmap_group.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"

#include "../src/n64.h"

#ifdef HALO_DC
#include "math/integer_math.h"
#include "text/draw_string.h"
#endif

#include <string.h>

/* motion_sensor.c's (the sensor's centre and its scale) */
extern real_point2d center_point;
extern real scale[];

/* interface/hud_draw.c's meter record (the meter shader's inputs) */
struct n64_meter_parameters
{
	pixel32 gradient_min_color;
	pixel32 gradient_max_color;
	pixel32 background_color;
	pixel32 flash_color;
	boolean flash_color_is_negative;
	boolean tint_mode_2;
	byte pad12[2];
	pixel32 tint_color;
	real gradient;
};

/* the game's 640x480 layout in the N64's pixels (half, at 320x240): across,
squeezed about the view's middle with the anamorphic picture
(n64_render_hud_squeeze); without it, the same scaling as before to the bit */
static real hud_x(
	real x)
{
	real scale = n64_render_layout_scale_x();
	real squeeze = n64_render_hud_squeeze();
	real middle;

	if (squeeze == 1.f)
		return x * scale;
	middle = (render.camera.viewport_bounds.x0 + render.camera.viewport_bounds.x1) * 0.5f * scale;
	return middle + (x * scale - middle) * squeeze;
}

static real hud_y(
	real y)
{
	return y * n64_render_layout_scale_y();
}

void *_texture_cache_bitmap_get_hardware_format(
	struct bitmap_data *bitmap,
	boolean block,
	boolean load)
{
	(void)block;
	(void)load;
	return bitmap && bitmap->pixels_offset > 0 ? bitmap : NULL;
}

/* the view's HUD began before interface_draw_screen (n64_render_game.c) */
void rasterizer_hud_begin(
	void)
{
	return;
}

void rasterizer_hud_end(
	void)
{
	return;
}

#ifdef HALO_DC
/* an upright quad of the secondary map, from screen y0 to y1, its texels
t0 to t1 (half-size, as n64_hud_quad takes them) */
static void dc_overlay_quad(
	short sprite,
	real origin_x,
	real origin_y,
	struct dynamic_screen_vertex const *vertices,
	real y0,
	real y1,
	real s0,
	real s1,
	real t0,
	real t1,
	unsigned long color)
{
	struct n64_hud_quad quad;
	short i;

	csmemset(&quad, 0, sizeof(quad));
	quad.sprite = sprite;
	for (i = 0; i < 4; i++)
	{
		quad.x[i] = hud_x(origin_x + vertices[i].position.x);
		quad.y[i] = hud_y(origin_y + (i > 1 ? y1 : y0));
		quad.s[i] = ((i + 1) & 2) ? s1 : s0;
		quad.t[i] = i > 1 ? t1 : t0;
	}
	quad.color = color;
	n64_hud_quad(&quad);
}

/* Halo's multitexture overlays (hud_draw.c hud_draw_multitexture_overlay:
the sniper's angle ticks), which the N64 drew as their first map alone --
the sniper's mask, a white bar each side of the screen all the time. The
second map is what shows (the ticks, through the mask: map0 x map1), at
its own scale and offset (scrolled by the aim's pitch), wrapped, faded by
both maps' fades (the ticks' fade is the zoom: none unzoomed). The mask's
own shape (its soft ends) is left out. Upright quads only; 1 when done */
static boolean dc_multitexture_quad(
	struct rasterizer_dynamic_screen_geometry_parameters *parameters,
	struct dynamic_screen_vertex *vertices)
{
	struct bitmap_data *primary = parameters->map[0], *secondary = parameters->map[1];
	real fade = 1.f, texel_u, texel_v, origin_x, origin_y;
	real n_s[4], n_t[4], s0, s1, t0, t1, shift, y0, y1;
	unsigned long color, alpha;
	short i;

	if (!primary || !secondary || parameters->meter_parameters)
		return FALSE;
	if (parameters->map_fade[0])
		fade *= *parameters->map_fade[0];
	if (parameters->map_fade[1])
		fade *= *parameters->map_fade[1];
	alpha = (unsigned long)(((vertices[0].color >> 24) & 255) * PIN(fade, 0.f, 1.f) + 0.5f);
	if (alpha < 2 || secondary->pixels_offset <= 0 || !primary->width || !primary->height)
		return TRUE;
	if (vertices[0].position.y != vertices[1].position.y || vertices[1].position.x != vertices[2].position.x)
		return FALSE;
	color = (vertices[0].color & 0x00FFFFFF) | alpha << 24;
	/* the first map's coordinates 0 to 1, then the second's */
	texel_u = TEST_FLAG(primary->flags, 4) ? 1.f : primary->width;
	texel_v = TEST_FLAG(primary->flags, 4) ? 1.f : primary->height;
	texel_u *= parameters->map_texture_scale[0].i ? parameters->map_texture_scale[0].i : 1.f;
	texel_v *= parameters->map_texture_scale[0].j ? parameters->map_texture_scale[0].j : 1.f;
	for (i = 0; i < 4; i++)
	{
		n_s[i] = vertices[i].texture_coordinates.x * texel_u / primary->width;
		n_t[i] = vertices[i].texture_coordinates.y * texel_v / primary->height;
		n_s[i] = n_s[i] * parameters->map_scale[1].i + (parameters->map_offset[1] ? parameters->map_offset[1]->x : 0.f);
		n_t[i] = n_t[i] * parameters->map_scale[1].j + (parameters->map_offset[1] ? parameters->map_offset[1]->y : 0.f);
	}
	/* (wrapped: shifted to start in the texture, split where it wraps) */
	s0 = n_s[0];
	s1 = n_s[1];
	shift = (real)floor(MIN(s0, s1));
	s0 = (s0 - shift) * secondary->width * 0.5f;
	s1 = (s1 - shift) * secondary->width * 0.5f;
	t0 = n_t[0];
	t1 = n_t[2];
	origin_x = render.camera.viewport_bounds.x0 + (parameters->offset ? parameters->offset->i : 0.f);
	origin_y = render.camera.viewport_bounds.y0 + (parameters->offset ? parameters->offset->j : 0.f);
	y0 = vertices[0].position.y;
	y1 = vertices[2].position.y;
	if (t1 < t0)
		return FALSE;
	shift = (real)floor(t0);
	t0 -= shift;
	t1 -= shift;
	while (t1 > t0 + 0.0001f && y1 > y0)
	{
		real end = MIN(t1, (real)floor(t0) + 1.f);
		real y_end = t1 > t0 ? y0 + (y1 - y0) * (end - t0) / (t1 - t0) : y1;
		real base = (real)floor(t0);

		dc_overlay_quad((short)(secondary->pixels_offset - 1), origin_x, origin_y, vertices, y0, y_end, s0, s1,
			(t0 - base) * secondary->height * 0.5f, (end - base) * secondary->height * 0.5f, color);
		y0 = y_end;
		t0 = end;
	}
	return TRUE;
}

/* Halo's menus are drawing (port/dc/src/dc_menu.c, round render_ui_widgets) */
extern int dc_hud_widgets;

/* a widget's bitmap (ui_widget.c draw_bitmap_in_rect): the widget's own
(the third map under "nifty render fx"'s plasma shimmer, which is left
out), its coordinates 0 to 1 whatever its layout, blended straight as the
widgets are -- its colour premultiplied, as the shell's sprites are
(tools/n64/hud_dc.py UiHudExporter) */
static void dc_widget_quad(
	struct rasterizer_dynamic_screen_geometry_parameters *parameters,
	struct dynamic_screen_vertex *vertices)
{
	struct bitmap_data *bitmap = parameters->doing_plasma_effect ? parameters->map[2] : parameters->map[0];
	struct n64_hud_quad quad;
	unsigned long color = vertices[0].color, alpha = color >> 24;
	short i;

	if (!bitmap || bitmap->pixels_offset <= 0 || !alpha)
		return;
	csmemset(&quad, 0, sizeof(quad));
	quad.sprite = (short)(bitmap->pixels_offset - 1);
	for (i = 0; i < 4; i++)
	{
		quad.x[i] = hud_x(render.camera.viewport_bounds.x0 + vertices[i].position.x);
		quad.y[i] = hud_y(render.camera.viewport_bounds.y0 + vertices[i].position.y);
		quad.s[i] = vertices[i].texture_coordinates.x * bitmap->width * 0.5f;
		quad.t[i] = vertices[i].texture_coordinates.y * bitmap->height * 0.5f;
	}
	quad.color = alpha << 24 | (((color >> 16) & 255) * alpha / 255) << 16 | (((color >> 8) & 255) * alpha / 255) << 8 |
		(color & 255) * alpha / 255;
	n64_hud_quad(&quad);
}
#endif

void rasterizer_psuedo_dynamic_screen_quad_draw(
	struct rasterizer_dynamic_screen_geometry_parameters *parameters,
	struct dynamic_screen_vertex *vertices)
{
	struct bitmap_data *bitmap = parameters->map[0];
	struct n64_hud_quad quad;
	real origin_x, origin_y;
	real texel_u, texel_v;
	short i;

#ifdef HALO_DC
	if (dc_hud_widgets)
	{
		dc_widget_quad(parameters, vertices);
		return;
	}
	if (dc_multitexture_quad(parameters, vertices))
		return;
#endif

	if (!bitmap || bitmap->pixels_offset <= 0)
		return;
	quad.sprite = (short)(bitmap->pixels_offset - 1);
	origin_x = render.camera.viewport_bounds.x0 + (parameters->offset ? parameters->offset->i : 0.f);
	origin_y = render.camera.viewport_bounds.y0 + (parameters->offset ? parameters->offset->j : 0.f);
	/* linear (interface) bitmaps are addressed in texels, swizzled ones from
	0 to 1; either way scaled by map_texture_scale first */
	if (TEST_FLAG(bitmap->flags, 4)) /* _bitmap_linear_bit */
	{
		texel_u = 1.f;
		texel_v = 1.f;
	}
	else
	{
		texel_u = bitmap->width;
		texel_v = bitmap->height;
	}
	texel_u *= parameters->map_texture_scale[0].i ? parameters->map_texture_scale[0].i : 1.f;
	texel_v *= parameters->map_texture_scale[0].j ? parameters->map_texture_scale[0].j : 1.f;
	for (i = 0; i < 4; i++)
	{
		quad.x[i] = hud_x(origin_x + vertices[i].position.x);
		quad.y[i] = hud_y(origin_y + vertices[i].position.y);
		quad.s[i] = vertices[i].texture_coordinates.x * texel_u * 0.5f;
		quad.t[i] = vertices[i].texture_coordinates.y * texel_v * 0.5f;
	}
	quad.color = vertices[0].color;
	quad.meter = parameters->meter_parameters != NULL;
	if (quad.meter)
	{
		struct n64_meter_parameters const *meter = parameters->meter_parameters;

		quad.minimum_color = meter->gradient_min_color;
		quad.maximum_color = meter->gradient_max_color;
		quad.flash_color = meter->flash_color_is_negative ? meter->flash_color & 0xFF000000 : meter->flash_color;
		quad.background_color = meter->background_color;
		quad.tint_color = meter->tint_color;
	}
	n64_hud_quad(&quad);
	return;
}

/* ---------- text */

/* The native actor scan owns eligibility and the selected weapon. This only
labels its real hold-to-pick-up action in the current split-screen viewport;
B remains Xbox X in every preset, including both Golden schemes. */
extern short n64_hud_original_action_icon(short x, short baseline, pixel32 color, short *bottom);
void n64_hud_game_weapon_prompt(char const *name, int swap)
{
	real virtual_x = render.camera.viewport_bounds.x0 + 16;
	real virtual_y = render.camera.viewport_bounds.y0 +
		(render.camera.viewport_bounds.y1 - render.camera.viewport_bounds.y0) * 0.64f;
	int x = (int)hud_x(virtual_x), y = (int)hud_y(virtual_y);
	real text_scale = n64_render_layout_scale_x() * n64_render_hud_squeeze();
	short icon_x, after_icon, icon_bottom = (short)virtual_y;
	int name_y = y + 16;
#ifdef HALO_DC
	/* (the Dreamcast's: the pad's action button named, not Halo's Xbox
	X icon -- D-pad left in dc_input.c's schemes) */
	{
		char text[64];
		char const *action = n64_input_button_name(0, 2);

		snprintf(text, sizeof(text), "HOLD %s %s", action ? action : "D-PAD LEFT", swap ? "TO SWAP FOR" : "TO PICK UP");
		n64_hud_text(x, y, 0xFF8CC0FFu, text);
		if (name_y < y + n64_hud_text_height() + 2)
			name_y = y + n64_hud_text_height() + 2;
		n64_hud_text(x, name_y, 0xFFFFFFFFu, name);
		(void)icon_x;
		(void)after_icon;
		(void)icon_bottom;
		(void)text_scale;
		return;
	}
#endif
	/* Keep the existing HOLD / action / weapon flow and the original font.
	   Pixel advance is measured after the existing lazy font load. */
	n64_hud_text(x, y, 0xFF8CC0FFu, "HOLD ");
	if (name_y < y + n64_hud_text_height() + 2)
		name_y = y + n64_hud_text_height() + 2;
	icon_x = (short)(virtual_x + n64_hud_text_width("HOLD ") / text_scale);
	after_icon = n64_hud_original_action_icon(icon_x, (short)(virtual_y + 24), 0xFF8CC0FFu, &icon_bottom);
	if (after_icon > icon_x)
	{
		real bottom_y = hud_y(icon_bottom);
		int below_icon = (int)bottom_y;
		if ((real)below_icon < bottom_y) below_icon++;
		if (name_y < below_icon + 2) name_y = below_icon + 2;
		n64_hud_text((int)hud_x(after_icon + 3), y, 0xFF8CC0FFu,
			swap ? " TO SWAP FOR" : " TO PICK UP");
	}
	else
		n64_hud_text((int)hud_x(icon_x), y, 0xFF8CC0FFu,
			swap ? "B TO SWAP FOR" : "B TO PICK UP");
	/* Follow the actual authored icon and original font at every HUD scale. */
	n64_hud_text(x, name_y, 0xFFFFFFFFu, name);
}

#ifdef HALO_DC
/* a font character (font_group.c's, private there): pixels_offset is its
glyph's number + 1 on the Dreamcast (tools/n64/hud_dc.py) */
struct dc_font_character
{
	word character;
	short character_width;
	short bitmap_width;
	short bitmap_height;
	short bitmap_origin_x;
	short bitmap_origin_y;
	short hardware_character_index;
	word pad;
	long pixels_offset;
};

/* draw_string.c's glyph, where it lays it out (the view's own pixels):
its shadow first, black at its alpha a pixel down and right, then the
glyph (rasterizer_text.c rasterizer_draw_character_with_dropshadow) */
static void dc_draw_character(
	struct parse_string_state *state,
	struct font_header *font,
	struct font_character *character,
	pixel32 color,
	short x0,
	short y0,
	short x,
	short y,
	short dx,
	short dy)
{
	struct dc_font_character const *glyph = (struct dc_font_character const *)character;
	real left = render.camera.viewport_bounds.x0 + x0, top = render.camera.viewport_bounds.y0 + y0;

	(void)state;
	(void)font;
	if (glyph->pixels_offset <= 0)
		return;
	n64_hud_glyph((int)glyph->pixels_offset - 1, hud_x(left + 1.f), hud_y(top + 1.f), x, y, dx, dy,
		color & 0xFF000000);
	n64_hud_glyph((int)glyph->pixels_offset - 1, hud_x(left), hud_y(top), x, y, dx, dy, color);
}

/* the bounds and the clip as rasterizer_text.c's (the window's, the view's,
in the view's own pixels) */
static void dc_text_bounds(
	rectangle2d const *bounds,
	rectangle2d const *clip,
	rectangle2d *window_bounds,
	rectangle2d *viewport_bounds)
{
	if (!bounds)
	{
		*window_bounds = render.camera.window_bounds;
		offset_rectangle2d(window_bounds, -render.camera.viewport_bounds.x0, -render.camera.viewport_bounds.y0);
	}
	else
		*window_bounds = *bounds;
	*viewport_bounds = render.camera.viewport_bounds;
	offset_rectangle2d(viewport_bounds, -render.camera.viewport_bounds.x0, -render.camera.viewport_bounds.y0);
	if (clip)
	{
		viewport_bounds->x0 = MAX(clip->x0, 0);
		viewport_bounds->y0 = MAX(clip->y0, 0);
		viewport_bounds->x1 = MIN(viewport_bounds->x1, clip->x1);
		viewport_bounds->y1 = MIN(viewport_bounds->y1, clip->y1);
	}
}

void rasterizer_draw_string(
	rectangle2d const *bounds,
	rectangle2d const *clip,
	point2d *cursor_reference,
	short height_adjust,
	char const *string)
{
	rectangle2d window_bounds, viewport_bounds;

	if (!string || !*string)
		return;
	dc_text_bounds(bounds, clip, &window_bounds, &viewport_bounds);
	draw_string(dc_draw_character, &window_bounds, cursor_reference, &viewport_bounds, height_adjust, string);
	return;
}

void rasterizer_draw_unicode_string(
	rectangle2d const *bounds,
	rectangle2d const *clip,
	point2d *cursor_reference,
	short height_adjust,
	wchar_t const *string)
{
	rectangle2d window_bounds, viewport_bounds;

	if (!string || !*string)
		return;
	dc_text_bounds(bounds, clip, &window_bounds, &viewport_bounds);
	draw_unicode_string(dc_draw_character, &window_bounds, cursor_reference, &viewport_bounds, height_adjust, string);
	return;
}

/* Halo's messages with buttons in them ("%action", "%back": ui_widget.c
draw_string_and_hack_in_icons, which the N64 build leaves out) -- the
respawn countdown, the postgame's "quit" and "pick game" --, each button
named as the Dreamcast pad has it where Halo drew its Xbox icon: the
game's buttons and controls as port/dc/src/dc_input.c lays them out
(n64_input_button_name: the Xbox button, button below, for the first local
player's scheme -- the Dreamcast plays one), the menus' A and B and the
D-pad as they are */
static struct
{
	char const *name;
	char const *label;
	short button; /* hud_messaging.c's icon (_icon_a_button = 0...), or NONE */
} const dc_icon_labels[] = {
	{"a-button", "A", NONE}, {"b-button", "B", NONE}, {"x-button", "X", NONE}, {"y-button", "Y", NONE},
	{"black-button", NULL, 4}, {"white-button", NULL, 5},
	{"left-trigger", NULL, 6}, {"right-trigger", NULL, 7},
	{"dpad-up", "UP", NONE}, {"dpad-down", "DOWN", NONE}, {"dpad-left", "LEFT", NONE}, {"dpad-right", "RIGHT", NONE},
	{"start-button", NULL, 12}, {"back-button", NULL, 13},
	{"left-thumb", NULL, 14}, {"right-thumb", NULL, 15},
	{"left-stick", NULL, 16}, {"right-stick", NULL, 17},
	{"action", NULL, 2}, {"throw-grenade", NULL, 6}, {"primary-trigger", NULL, 7},
	{"integrated-light", NULL, 5}, {"jump", NULL, 0}, {"use-equipment", NULL, 4},
	{"rotate-weapons", NULL, 3}, {"rotate-grenades", NULL, 4}, {"crouch", NULL, 14},
	{"zoom", NULL, 15}, {"accept", "A", NONE}, {"back", "B", NONE}, {"move", NULL, 16}, {"look", NULL, 17},
};

void n64_hud_game_icon_string(
	rectangle2d *bounds,
	wchar_t const *string)
{
	wchar_t text[256];
	short length = 0;

	while (*string && length < (short)NUMBEROF(text) - 1)
	{
		short i, found = NONE, size = 0;

		if (*string == '%')
		{
			for (i = 0; i < (short)NUMBEROF(dc_icon_labels) && found == NONE; i++)
			{
				char const *name = dc_icon_labels[i].name;

				for (size = 0; name[size] && string[1 + size] == (wchar_t)name[size]; size++)
					;
				if (!name[size])
					found = i;
			}
		}
		if (found != NONE)
		{
			char const *label = dc_icon_labels[found].label;

			if (!label)
				label = n64_input_button_name(0, dc_icon_labels[found].button);
			if (!label)
				label = "?";

			for (; *label && length < (short)NUMBEROF(text) - 1; label++)
				text[length++] = (wchar_t)*label;
			string += 1 + size;
		}
		else
			text[length++] = *string++;
	}
	text[length] = 0;
	rasterizer_draw_unicode_string(bounds, NULL, NULL, 0, text);
	return;
}
#else
static void draw_text(
	rectangle2d const *bounds,
	char const *text)
{
	rectangle2d window = render.camera.window_bounds;
	short x, y;

	if (!bounds)
		bounds = &window;
	x = (short)hud_x(bounds->x0);
	y = (short)hud_y(bounds->y0);
	n64_hud_text(x, y, 0xFFFFFFFF, text);
}

void rasterizer_draw_string(
	rectangle2d const *bounds,
	rectangle2d const *clip,
	point2d *cursor_reference,
	short height_adjust,
	char const *string)
{
	(void)clip;
	(void)cursor_reference;
	(void)height_adjust;
	if (string && *string)
		draw_text(bounds, string);
	return;
}

void rasterizer_draw_unicode_string(
	rectangle2d const *bounds,
	rectangle2d const *clip,
	point2d *cursor_reference,
	short height_adjust,
	wchar_t const *string)
{
	char text[128];
	short i;

	(void)clip;
	(void)cursor_reference;
	(void)height_adjust;
	if (!string)
		return;
	for (i = 0; i < (short)sizeof(text) - 1 && string[i]; i++)
		text[i] = string[i] < 128 ? (char)string[i] : '?';
	text[i] = 0;
	if (i)
		draw_text(bounds, text);
	return;
}
#endif

void rasterizer_text_set_shadow_color(pixel32 shadow_color) { (void)shadow_color; return; }
void rasterizer_text_cache_flush(void) { return; }

/* ---------- the motion sensor: blips in the sensor's -1..1 space (the Xbox
drew them into a texture it laid over the sensor) */

#define MOTION_SENSOR_RADIUS 32.f

void rasterizer_hud_motion_sensor_blip_begin(void) { return; }

void rasterizer_hud_motion_sensor_blip_draw(
	real_point2d const *position,
	real intensity,
	real size,
	real_rgb_color const *color,
	boolean large_blip)
{
	real radius = MOTION_SENSOR_RADIUS * scale[0];
	real x = center_point.x + render.camera.viewport_bounds.x0 + position->x * -0.03125f * radius;
	real y = center_point.y + render.camera.viewport_bounds.y0 + position->y * 0.03125f * radius;
	unsigned long argb =
		((unsigned long)PIN(color->red * intensity * 255.f, 0.f, 255.f) << 16) |
		((unsigned long)PIN(color->green * intensity * 255.f, 0.f, 255.f) << 8) |
		(unsigned long)PIN(color->blue * intensity * 255.f, 0.f, 255.f);

	(void)large_blip;
	{
		/* (its size in 320x240's pixels, then the screen's) */
		real pixels = MAX(2.f, size * 0.0625f * radius);

		n64_hud_blip(hud_x(x), hud_y(y), pixels * n64_render_layout_scale_x() * 2.f * n64_render_hud_squeeze(),
			pixels * n64_render_layout_scale_y() * 2.f, argb);
	}
	return;
}

void rasterizer_hud_motion_sensor_blip_end(
	real_point2d const *center,
	real sweep)
{
	(void)center;
	(void)sweep;
	return;
}

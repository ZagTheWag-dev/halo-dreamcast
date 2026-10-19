/*
DC_LEVEL.C

The level (levels/<map>.dcl, tools/n64/level_dc.py): loaded whole, used in
place (little-endian, the converter's sections aligned), drawn by PVS group,
frustum-tested by group and by run; a run wholly beyond the near plane is
transformed and divided in one ftrv each vertex, one crossing it goes
through the near-plane clip (dc_render.c).

The textures go to video memory at load (their data then given back to
the heap); each material has its polygon header (its texture modulated by
the vertices' light), sent when the material changes. A material with no
texture has its colour multiplied into its vertices' light at load.
*/

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "dc_render.h"

struct dcl_header
{
	char magic[4];
	uint32_t version;
	uint16_t texture_count, material_count, group_count, run_count;
	uint32_t owner_count, vertex_count, index_count;
	uint32_t textures_offset, materials_offset, groups_offset, owners_offset, runs_offset,
		positions_offset, attributes_offset, indices_offset, texdata_offset, texdata_size;
	float minimum[3], maximum[3];
	uint32_t flags;
};

struct dcl_texture
{
	uint32_t offset, size;
	uint16_t width, height;
	uint32_t format, flags, mean_argb, reserved[2];
};

struct dcl_material
{
	int16_t texture;
	uint16_t flags;
	uint32_t flat_argb;
	uint32_t reserved[2];
};

struct dcl_group
{
	float minimum[3], maximum[3];
	uint16_t first_run, run_count, first_owner, owner_count;
};

struct dcl_run
{
	uint16_t material, vertex_count;
	uint32_t first_vertex, first_index, index_count;
	float minimum[3], maximum[3];
};

struct dcl_position
{
	float x, y, z, pad;
};

struct dcl_attribute
{
	float u, v;
	uint32_t argb;
};

typedef char dcl_header_size_assert[sizeof(struct dcl_header) == 96 ? 1 : -1];
typedef char dcl_texture_size_assert[sizeof(struct dcl_texture) == 32 ? 1 : -1];
typedef char dcl_material_size_assert[sizeof(struct dcl_material) == 16 ? 1 : -1];
typedef char dcl_group_size_assert[sizeof(struct dcl_group) == 32 ? 1 : -1];
typedef char dcl_run_size_assert[sizeof(struct dcl_run) == 40 ? 1 : -1];

#define DC_RUN_VERTICES 64
#define DC_STRIP_MAX 256
#define DC_LEVEL_TEXTURES 64
#define DC_LEVEL_MATERIALS 128
/* the detail pass: the terrain runs within this many units of the eye (the
detail no more than a texel a pixel past it), at most so many a view */
#define DC_DETAIL_DISTANCE 40.0f
/* (a material whose detail scale is negative -- not terrain: cliffs,
walls, tools/n64/level_dc.py near_detail -- only this near) */
#define DC_NEAR_DETAIL_DISTANCE 15.0f
#define DC_DETAIL_RUNS 512
/* the alpha-tested runs a view draws (the punch-through list, after the
opaque ones: a30's forest cards, grates) */
#define DC_CUTOUT_RUNS 512
/* the water runs a view draws (the translucent list: a30's rivers and
waterfalls, tools/n64/level_dc.py chicago_water) */
#define DC_WATER_RUNS 256

/* dcl_texture.format (tools/n64/level_dc.py) */
static const int dc_texture_formats[] = {PVR_TXRFMT_RGB565, PVR_TXRFMT_ARGB1555, PVR_TXRFMT_ARGB4444};
#define DC_FORMAT_COUNT ((int)(sizeof(dc_texture_formats) / sizeof(dc_texture_formats[0])))
/* (with the format: VQ compressed, tools/n64/level_dc.py FORMAT_VQ) */
#define DC_FORMAT_VQ 0x100

/* a texture's PowerVR format word */
static int dc_texture_format(uint32_t format)
{
	return dc_texture_formats[format & 0xFF] | ((format & DC_FORMAT_VQ) ? PVR_TXRFMT_VQ_ENABLE : 0);
}

static struct
{
	uint8_t *data;
	unsigned long size;
	const struct dcl_header *h;
	const struct dcl_material *materials;
	const struct dcl_group *groups;
	const uint16_t *owners;
	const struct dcl_run *runs;
	const struct dcl_position *positions;
	struct dcl_attribute *attributes;
	const uint16_t *indices;
	const struct dcl_texture *textures;
	pvr_ptr_t vram[DC_LEVEL_TEXTURES];
	unsigned long vram_bytes;
	pvr_poly_hdr_t headers[DC_LEVEL_MATERIALS];
	/* a terrain's detail pass (tools/n64/level_dc.py detail_texture: the
	material's reserved[0] its detail texture + 1, reserved[1] its scale):
	its header, and the runs drawn this view near enough for it */
	pvr_poly_hdr_t detail_headers[DC_LEVEL_MATERIALS];
	uint8_t has_detail[DC_LEVEL_MATERIALS];
	uint16_t detail_runs[DC_DETAIL_RUNS];
	int detail_run_count;
	uint16_t cutout_runs[DC_CUTOUT_RUNS];
	int cutout_run_count;
	uint16_t water_runs[DC_WATER_RUNS];
	int water_run_count;
} L;

static dc_cv dc_cvs[DC_RUN_VERTICES];

void n64_render_unload_level(void)
{
	int i;

	dc_model_unload();
	if (L.data)
		pvr_wait_render_done();
	for (i = 0; i < DC_LEVEL_TEXTURES; i++)
	{
		if (L.vram[i])
			pvr_mem_free(L.vram[i]);
	}
	free(L.data);
	memset(&L, 0, sizeof(L));
}

void dc_level_compile_headers(void)
{
	int m;

	if (!L.data)
		return;
	for (m = 0; m < L.h->material_count; m++)
	{
		const struct dcl_material *material = &L.materials[m];
		int t = material->texture;
		int two_sided = (material->flags & MATERIAL_TWO_SIDED) != 0;

		/* (alpha-tested: the punch-through list, its texture's alpha
		cutting it out; before, on the opaque list, a30's forest cards drew
		as walls of their texture's background) */
		/* (water: untextured, blended by its vertices' alpha, unlit by
		the depth it is drawn over) */
		if (material->flags & MATERIAL_WATER)
			dc_header_compile_list(&L.headers[m], PVR_LIST_TR_POLY, NULL, 0, 0, MATERIAL_TWO_SIDED | MATERIAL_WATER);
		else if (t >= 0 && L.vram[t] && (material->flags & MATERIAL_ALPHA_TEST))
			dc_header_compile_list(&L.headers[m], PVR_LIST_PT_POLY, L.vram[t], dc_texture_format(L.textures[t].format),
				L.textures[t].width, material->flags & (MATERIAL_TWO_SIDED | MATERIAL_ALPHA_TEST));
		else if (t >= 0 && L.vram[t])
			dc_header_compile(&L.headers[m], L.vram[t], dc_texture_format(L.textures[t].format), L.textures[t].width,
				two_sided);
		else
			dc_header_compile(&L.headers[m], NULL, 0, 0, two_sided);
		/* the detail: twice the frame's colour times the detail's texel
		(the frame times the texel, plus the texel times the frame), on the
		surface already drawn (its depth equal), unfogged */
		L.has_detail[m] = 0;
		{
			int d = (int)material->reserved[0] - 1;

			if (d >= 0 && d < L.h->texture_count && L.vram[d])
			{
				pvr_poly_cxt_t c;

				pvr_poly_cxt_txr(&c, PVR_LIST_TR_POLY, dc_texture_format(L.textures[d].format) | PVR_TXRFMT_TWIDDLED,
					L.textures[d].width, L.textures[d].width, L.vram[d], PVR_FILTER_BILINEAR);
				c.txr.mipmap = PVR_MIPMAP_ENABLE;
				c.txr.env = PVR_TXRENV_MODULATE;
				c.gen.shading = PVR_SHADE_FLAT;
				c.gen.fog_type = PVR_FOG_DISABLE;
				c.gen.culling = two_sided ? PVR_CULLING_NONE : PVR_CULLING_CW;
				c.depth.comparison = PVR_DEPTHCMP_GEQUAL;
				c.depth.write = PVR_DEPTHWRITE_DISABLE;
				c.blend.src = PVR_BLEND_DESTCOLOR;
				/* (the PowerVR's "other colour": as the destination factor, the source's) */
				c.blend.dst = PVR_BLEND_DESTCOLOR;
				pvr_poly_compile(&L.detail_headers[m], &c);
				L.has_detail[m] = 1;
			}
		}
	}
}

static int dc_level_read(const char *map_name);

/* the menu's scene (dc_menu.c): a map's level alone, no models (the N64's
is the ui scenario's sky and ring: levels/<name>.mdl); NULL: the level and
the models given back */
void n64_render_load_scene(const char *name)
{
	n64_render_unload_level();
	if (name)
		dc_level_read(name);
}

static int dcl_section(unsigned long offset, unsigned long count, unsigned long size, unsigned long align)
{
	return offset % align == 0 && offset <= L.size && count * size <= L.size - offset;
}

static uint32_t dc_modulate(uint32_t a, uint32_t b)
{
	uint32_t r = a & 0xFF000000u;
	int s;

	for (s = 0; s < 24; s += 8)
	{
		uint32_t c = (((a >> s) & 255) * ((b >> s) & 255) + 127) / 255;

		r |= c << s;
	}
	return r;
}

int n64_render_load_level(const char *map_name, int low)
{
	(void)low;
	n64_render_unload_level();
	dc_model_load(map_name);
	return dc_level_read(map_name);
}

/* levels/<map>.dcl into RAM, its textures into video memory */
static int dc_level_read(const char *map_name)
{
	char path[64], line[160];
	const struct dcl_header *h;
	long file, size;
	unsigned long i;

	snprintf(path, sizeof(path), "rom:/levels/%s.dcl", map_name);
	file = n64_file_open(path);
	if (file < 0)
	{
		n64_log("dcl: no level file");
		return 0;
	}
	/* (the file but its texture data, its last section, into RAM: the
	textures go from the disc to video memory, one at a time) */
	size = n64_file_size(file);
	{
		struct dcl_header head;
		long kept = size;

		if (n64_file_read(file, &head, sizeof(head)) == (long)sizeof(head) && head.texdata_offset <= (uint32_t)size &&
			head.texdata_offset >= sizeof(head))
			kept = (long)head.texdata_offset;
		L.data = memalign(32, kept);
		if (!L.data || n64_file_seek(file, 0) < 0 || n64_file_read(file, L.data, kept) != kept)
		{
			n64_file_close(file);
			free(L.data);
			L.data = NULL;
			n64_log("dcl: could not read the level");
			return 0;
		}
	}
	/* (the sections checked against the whole file) */
	L.size = (unsigned long)size;
	h = L.h = (const struct dcl_header *)L.data;
	if (memcmp(h->magic, "DCL1", 4) || h->version != 1 ||
		h->texture_count > DC_LEVEL_TEXTURES || h->material_count > DC_LEVEL_MATERIALS ||
		!dcl_section(h->textures_offset, h->texture_count, sizeof(struct dcl_texture), 4) ||
		!dcl_section(h->texdata_offset, h->texdata_size, 1, 32) ||
		!dcl_section(h->materials_offset, h->material_count, sizeof(struct dcl_material), 4) ||
		!dcl_section(h->groups_offset, h->group_count, sizeof(struct dcl_group), 4) ||
		!dcl_section(h->owners_offset, h->owner_count, 2, 2) ||
		!dcl_section(h->runs_offset, h->run_count, sizeof(struct dcl_run), 4) ||
		!dcl_section(h->positions_offset, h->vertex_count, sizeof(struct dcl_position), 32) ||
		!dcl_section(h->attributes_offset, h->vertex_count, sizeof(struct dcl_attribute), 32) ||
		!dcl_section(h->indices_offset, h->index_count, 2, 2))
	{
		n64_file_close(file);
		n64_log("dcl: the level file is not this build's");
		n64_render_unload_level();
		return 0;
	}
	/* the textures from the disc to video memory, through a buffer the
	largest one's size */
	L.textures = (const struct dcl_texture *)(L.data + h->textures_offset);
	{
		unsigned long biggest = 32;
		void *bounce;

		for (i = 0; i < h->texture_count; i++)
		{
			const struct dcl_texture *t = &L.textures[i];

			if ((int)(t->format & 0xFF) >= DC_FORMAT_COUNT || t->width != t->height ||
				t->offset + t->size > h->texdata_size)
			{
				n64_file_close(file);
				n64_log("dcl: a texture out of bounds");
				n64_render_unload_level();
				return 0;
			}
			if (t->size > biggest)
				biggest = t->size;
		}
		bounce = memalign(32, biggest);
		for (i = 0; bounce && i < h->texture_count; i++)
		{
			const struct dcl_texture *t = &L.textures[i];

			if (n64_file_seek(file, (long)(h->texdata_offset + t->offset)) < 0 ||
				n64_file_read(file, bounce, (long)t->size) != (long)t->size)
				continue;
			L.vram[i] = pvr_mem_malloc(t->size);
			if (!L.vram[i])
			{
				n64_log("dcl: no video memory for a texture");
				continue;
			}
			pvr_txr_load(bounce, L.vram[i], t->size);
			L.vram_bytes += t->size;
		}
		free(bounce);
	}
	n64_file_close(file);
	L.size = h->texdata_offset;
	L.materials = (const struct dcl_material *)(L.data + h->materials_offset);
	L.groups = (const struct dcl_group *)(L.data + h->groups_offset);
	L.owners = (const uint16_t *)(L.data + h->owners_offset);
	L.runs = (const struct dcl_run *)(L.data + h->runs_offset);
	L.positions = (const struct dcl_position *)(L.data + h->positions_offset);
	L.attributes = (struct dcl_attribute *)(L.data + h->attributes_offset);
	L.indices = (const uint16_t *)(L.data + h->indices_offset);
	for (i = 0; i < h->run_count; i++)
	{
		const struct dcl_run *run = &L.runs[i];

		if (run->vertex_count > DC_RUN_VERTICES || run->material >= h->material_count ||
			run->first_vertex + run->vertex_count > h->vertex_count || run->first_index + run->index_count > h->index_count)
		{
			n64_log("dcl: a run out of bounds");
			n64_render_unload_level();
			return 0;
		}
	}
	/* (a material with no texture: its colour on the vertices) */
	for (i = 0; i < h->run_count; i++)
	{
		const struct dcl_run *run = &L.runs[i];
		const struct dcl_material *material = &L.materials[run->material];
		uint32_t flat = material->flat_argb | 0xFF000000u;
		/* (water keeps its colour's alpha: blended, level_dc.py) */
		uint32_t alpha = (material->flags & MATERIAL_WATER) ? (material->flat_argb & 0xFF000000u) : 0;
		unsigned long k;

		if (material->texture >= 0 && L.vram[material->texture])
			continue;
		for (k = 0; k < run->vertex_count; k++)
		{
			struct dcl_attribute *a = &L.attributes[run->first_vertex + k];

			a->argb = dc_modulate(a->argb, flat);
			if (alpha)
				a->argb = (a->argb & 0x00FFFFFFu) | alpha;
		}
	}
	dc_level_compile_headers();
	snprintf(line, sizeof(line), "dcl: %s, %u groups, %u runs, %lu vertices, %lu indices, %lu KB; %u textures, %lu KB",
		map_name, h->group_count, h->run_count, (unsigned long)h->vertex_count, (unsigned long)h->index_count,
		L.size / 1024, h->texture_count, L.vram_bytes / 1024);
	n64_log(line);
	return 1;
}

/* a run wholly beyond the near plane: each vertex to (sx, sy, 1/depth) */
static void dc_run_fast(const struct dcl_run *run)
{
	const struct dcl_position *p = L.positions + run->first_vertex;
	const struct dcl_attribute *a = L.attributes + run->first_vertex;
	const uint16_t *ix = L.indices + run->first_index;
	float sx[DC_RUN_VERTICES], sy[DC_RUN_VERTICES], sz[DC_RUN_VERTICES];
	uint32_t i;

	for (i = 0; i < run->vertex_count; i++)
	{
		float x = p[i].x, y = p[i].y, z = p[i].z;

		mat_trans_single(x, y, z);
		sx[i] = x;
		sy[i] = y;
		sz[i] = z;
	}
	for (i = 0; i < run->index_count; i++)
	{
		unsigned k = ix[i] & 0x7FFF;
		pvr_vertex_t *d = pvr_dr_target(dc_dr);

		d->flags = (ix[i] & 0x8000) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
		d->x = sx[k];
		d->y = sy[k];
		d->z = sz[k];
		d->u = a[k].u;
		d->v = a[k].v;
		d->argb = a[k].argb;
		d->oargb = 0;
		pvr_dr_commit(d);
	}
	dc_stat.vertices += run->vertex_count;
	dc_stat.strip_vertices += run->index_count;
}

/* a run crossing the near plane: transformed undivided, clipped by strip */
static void dc_run_clip(const struct dcl_run *run)
{
	const struct dcl_position *p = L.positions + run->first_vertex;
	const struct dcl_attribute *a = L.attributes + run->first_vertex;
	const uint16_t *ix = L.indices + run->first_index;
	const dc_cv *strip[DC_STRIP_MAX];
	int n = 0;
	uint32_t i;

	for (i = 0; i < run->vertex_count; i++)
	{
		float x = p[i].x, y = p[i].y, z = p[i].z, w = 1.0f;

		mat_trans_nodiv(x, y, z, w);
		dc_cvs[i].x = x;
		dc_cvs[i].y = y;
		dc_cvs[i].w = w;
		dc_cvs[i].u = a[i].u;
		dc_cvs[i].v = a[i].v;
		dc_cvs[i].argb = a[i].argb;
	}
	for (i = 0; i < run->index_count; i++)
	{
		if (n < DC_STRIP_MAX)
			strip[n++] = &dc_cvs[ix[i] & 0x7FFF];
		if (ix[i] & 0x8000)
		{
			dc_clip_strip(strip, n);
			n = 0;
		}
	}
	dc_stat.vertices += run->vertex_count;
	dc_stat.strip_vertices += run->index_count;
	dc_stat.runs_clipped++;
}

/* the detail pass over a run (dc_level_draw): its vertices again, the
detail's coordinates (the base's times the scale), white, on the
translucent list (through the clip: it stages) */
static void dc_run_detail(const struct dcl_run *run, float scale)
{
	const struct dcl_position *p = L.positions + run->first_vertex;
	const struct dcl_attribute *a = L.attributes + run->first_vertex;
	const uint16_t *ix = L.indices + run->first_index;
	const dc_cv *strip[DC_STRIP_MAX];
	int n = 0;
	uint32_t i;

	for (i = 0; i < run->vertex_count; i++)
	{
		float x = p[i].x, y = p[i].y, z = p[i].z, w = 1.0f;

		mat_trans_nodiv(x, y, z, w);
		dc_cvs[i].x = x;
		dc_cvs[i].y = y;
		dc_cvs[i].w = w;
		dc_cvs[i].u = a[i].u * scale;
		dc_cvs[i].v = a[i].v * scale;
		dc_cvs[i].argb = 0xFFFFFFFFu;
	}
	for (i = 0; i < run->index_count; i++)
	{
		if (n < DC_STRIP_MAX)
			strip[n++] = &dc_cvs[ix[i] & 0x7FFF];
		if (ix[i] & 0x8000)
		{
			dc_clip_strip(strip, n);
			n = 0;
		}
	}
	dc_stat.detail_vertices += run->index_count;
}

/* a box's nearest point's distance from the eye, squared */
static float dc_box_distance_squared(const float *eye, const float *minimum, const float *maximum)
{
	float d = 0.0f;
	int k;

	for (k = 0; k < 3; k++)
	{
		float e = eye[k] < minimum[k] ? minimum[k] - eye[k] : eye[k] > maximum[k] ? eye[k] - maximum[k] : 0.0f;

		d += e * e;
	}
	return d;
}

void dc_level_draw(const dc_camera_t *camera, const struct n64_view *view)
{
	int last_material = -1;
	int g, i;

	if (!L.data)
		return;
	L.detail_run_count = 0;
	L.cutout_run_count = 0;
	L.water_run_count = 0;
	for (g = 0; g < L.h->group_count; g++)
	{
		const struct dcl_group *group = &L.groups[g];
		int group_test, r;

		if (!group->run_count)
			continue;
		/* Halo's potentially visible set (n64_render.c): drawn if any
		owner cluster is visible; an owner past the BSP's clusters counts
		as visible */
		if (view->visible_clusters && group->owner_count)
		{
			int seen = 0, k;

			for (k = group->first_owner; k < group->first_owner + group->owner_count && !seen; k++)
			{
				int o = L.owners[k];

				seen = o >= view->cluster_count || (view->visible_clusters[o >> 5] & (1UL << (o & 31)));
			}
			if (!seen)
			{
				dc_stat.groups_pvs++;
				continue;
			}
		}
		group_test = dc_box_test(camera, group->minimum, group->maximum);
		if (group_test == DC_OUT)
			continue;
		dc_stat.groups++;
		for (r = group->first_run; r < group->first_run + group->run_count; r++)
		{
			const struct dcl_run *run = &L.runs[r];
			const struct dcl_material *material = &L.materials[run->material];
			int run_test = (group_test & DC_PARTIAL) ? dc_box_test(camera, run->minimum, run->maximum) : group_test;

			if (run_test == DC_OUT)
				continue;
			/* (the frame's room in the tile accelerator: dc_render.c) */
			if (!dc_stage_room((int)run->index_count * ((run_test & DC_NEAR) ? 4 : 1) + 1))
				continue;
			/* (R2: the translucent list) */
			if (material->flags & MATERIAL_WATER)
			{
				if (L.water_run_count < DC_WATER_RUNS)
					L.water_runs[L.water_run_count++] = (uint16_t)r;
				continue;
			}
			/* (other transparent shaders: not drawn yet) */
			if (material->flags & MATERIAL_TRANSPARENT)
				continue;
			/* (alpha-tested: after the loop, on the punch-through list) */
			if ((material->flags & MATERIAL_ALPHA_TEST) && material->texture >= 0 && L.vram[material->texture])
			{
				if (L.cutout_run_count < DC_CUTOUT_RUNS)
					L.cutout_runs[L.cutout_run_count++] = (uint16_t)r;
				continue;
			}
			if (run->material != last_material)
			{
				dc_send_header(&L.headers[run->material]);
				last_material = run->material;
			}
			/* (the matrix register is the camera's for every run: the
			game's code between runs does not touch it, but cheap) */
			mat_load(&camera->m);
			if (run_test & DC_NEAR)
				dc_run_clip(run);
			else
				dc_run_fast(run);
			dc_stat.runs++;
			if (L.has_detail[run->material] && L.detail_run_count < DC_DETAIL_RUNS && !dc_no_detail)
			{
				float scale, reach;

				memcpy(&scale, &material->reserved[1], sizeof(scale));
				reach = scale < 0.0f ? DC_NEAR_DETAIL_DISTANCE : DC_DETAIL_DISTANCE;
				if (dc_box_distance_squared(view->position, run->minimum, run->maximum) < reach * reach)
					L.detail_runs[L.detail_run_count++] = (uint16_t)r;
			}
		}
	}
	/* the alpha-tested runs, staged on the punch-through list (through the
	clip, which stages: dc_run_fast writes to the opaque list directly) */
	if (L.cutout_run_count)
	{
		dc_use_list(PVR_LIST_PT_POLY);
		last_material = -1;
		for (i = 0; i < L.cutout_run_count; i++)
		{
			const struct dcl_run *run = &L.runs[L.cutout_runs[i]];

			if (!dc_stage_room((int)run->index_count * 4 + 1))
				break;
			if (run->material != last_material)
			{
				dc_send_header(&L.headers[run->material]);
				last_material = run->material;
			}
			mat_load(&camera->m);
			dc_run_clip(run);
			dc_stat.runs++;
		}
		dc_use_list(PVR_LIST_OP_POLY);
		last_material = -1;
	}
	/* the water, staged on the translucent list (the PowerVR sorts it with
	the rest of the frame's translucent polygons) */
	if (L.water_run_count)
	{
		dc_use_list(PVR_LIST_TR_POLY);
		last_material = -1;
		for (i = 0; i < L.water_run_count; i++)
		{
			const struct dcl_run *run = &L.runs[L.water_runs[i]];

			if (!dc_stage_room((int)run->index_count * 4 + 1))
				break;
			if (run->material != last_material)
			{
				dc_send_header(&L.headers[run->material]);
				last_material = run->material;
			}
			mat_load(&camera->m);
			dc_run_clip(run);
			dc_stat.runs++;
		}
		dc_use_list(PVR_LIST_OP_POLY);
		last_material = -1;
	}
	/* the detail pass (the runs near the eye: dc_level_compile_headers) */
	if (L.detail_run_count)
	{
		dc_use_list(PVR_LIST_TR_POLY);
		last_material = -1;
		for (i = 0; i < L.detail_run_count; i++)
		{
			const struct dcl_run *run = &L.runs[L.detail_runs[i]];
			const struct dcl_material *material = &L.materials[run->material];
			float scale;

			if (!dc_stage_room((int)run->index_count * 4 + 1))
				break;
			if (run->material != last_material)
			{
				dc_send_header(&L.detail_headers[run->material]);
				last_material = run->material;
			}
			memcpy(&scale, &material->reserved[1], sizeof(scale));
			mat_load(&camera->m);
			dc_run_detail(run, scale < 0.0f ? -scale : scale);
		}
		dc_use_list(PVR_LIST_OP_POLY);
	}
}

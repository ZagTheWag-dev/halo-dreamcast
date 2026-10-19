/*
DC_MODEL.C

The objects' models (levels/<map>.dcm, tools/n64/models_dc.py), drawn from
the game's struct n64_object (n64_render_game.c: the model tag, its node
matrices, region permutations, tint): the N64's render_object
(port/n64/src/n64_render.c) on the PowerVR.

A model's vertices are in its nodes' spaces, sorted by node within a batch:
each node's matrix (the view's camera times the game's node matrix, built
once an object) transforms its range of vertices, each once, divided and
tinted, into a scratch of tile-accelerator vertices; a strip index is then a
copy of one. An object whose sphere reaches the near plane, and every
first-person model, has each batch's nearest vertex checked, and in a batch
that crosses the plane each strip: only a strip that crosses it goes
through the near-plane clip (dc_render.c), and there only its crossing
triangles. (The first-person weapon went through the clip whole, a divide
a strip index, and the clip took a crossing strip triangle by triangle:
its 1500 triangles took 1.75 ms of Flycast's time a frame, now 1.55.)
A batch's list is its material's: opaque
straight to the tile accelerator, cut-outs punched through, glows and
glass translucent (staged). The textures go to video memory at load.
*/

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "dc_render.h"

struct dcm_header
{
	char magic[4];
	uint32_t version;
	uint16_t texture_count, model_count, region_count, permutation_count, geometry_count, batch_count, range_count, pad;
	uint32_t textures_offset, models_offset, regions_offset, permutations_offset, geometries_offset, batches_offset,
		ranges_offset, data_offset, texdata_offset, texdata_size;
};

struct dcm_texture
{
	uint32_t offset, size;
	uint16_t width, height;
	uint32_t format, flags, mean_argb, reserved[2];
};

struct dcm_model
{
	uint16_t tag_index;
	uint8_t node_count, region_count;
	uint16_t first_region, flags, first_batch, batch_count, far_offset, far_count;
	uint32_t data_offset, vertex_count, index_count;
};

struct dcm_region
{
	uint16_t permutation_count, first_permutation;
};

struct dcm_geometry
{
	uint16_t first_batch, batch_count;
};

struct dcm_batch
{
	int16_t texture;
	uint16_t flags, first_range, range_count;
	uint32_t first_vertex;
	uint16_t vertex_count, triangle_count;
	uint32_t first_index, index_count;
};

struct dcm_range
{
	uint8_t node, pad;
	uint16_t count;
};

struct dcm_vertex
{
	float x, y, z, u, v;
	uint32_t argb, pad[2];
};

typedef char dcm_header_size_assert[sizeof(struct dcm_header) == 64 ? 1 : -1];
typedef char dcm_model_size_assert[sizeof(struct dcm_model) == 28 ? 1 : -1];
typedef char dcm_batch_size_assert[sizeof(struct dcm_batch) == 24 ? 1 : -1];
typedef char dcm_vertex_size_assert[sizeof(struct dcm_vertex) == 32 ? 1 : -1];

#define DCM_TEXTURES 512
#define DCM_NODES 64
/* (a model smaller than this many pixels across is not drawn; smaller
than FAR_PIXELS, its far version: n64_render.c, at 480 lines) */
#define DCM_MINIMUM_PIXELS 1.5f
#define DCM_FAR_PIXELS 48.0f
#define DCM_STRIP_MAX 256

/* dcl_texture.format (tools/n64/level_dc.py) */
static const int dcm_texture_formats[] = {PVR_TXRFMT_RGB565, PVR_TXRFMT_ARGB1555, PVR_TXRFMT_ARGB4444};
#define DCM_FORMAT_COUNT ((int)(sizeof(dcm_texture_formats) / sizeof(dcm_texture_formats[0])))
/* (with the format: VQ compressed, tools/n64/level_dc.py FORMAT_VQ) */
#define DCM_FORMAT_VQ 0x100
/* a weapon's display (tools/n64/models_dc.py display_texture): the batch's
texture the first of its frames, one picked by the object's function value;
the first frame's reserved[1]: DCM_DISPLAY_VALID | the function << 16 | the
counter's limit << 8 | the frame count */
#define MATERIAL_DISPLAY 0x4000
#define MATERIAL_DISPLAY_TENS 0x2000
#define DCM_DISPLAY_VALID 0x80000000u
/* a batch whose shader glows (tools/n64/models_dc.py self_illumination): its
texture's reserved[1] DCM_GLOW_VALID | the glow texture's index, added over
the batch (its depth equal), unlit */
#define MATERIAL_SELF_ILLUM 0x0080
#define DCM_GLOW_VALID 0x40000000u
/* the object being drawn's function values (n64_render_object) */
static const float *dcm_functions;

/* a texture's PowerVR format word */
static int dcm_format(uint32_t format)
{
	return dcm_texture_formats[format & 0xFF] | ((format & DCM_FORMAT_VQ) ? PVR_TXRFMT_VQ_ENABLE : 0);
}

static struct
{
	uint8_t *data;
	unsigned long size;
	const struct dcm_header *h;
	const struct dcm_texture *textures;
	const struct dcm_model *models;
	const struct dcm_region *regions;
	const int16_t *permutations;
	const struct dcm_geometry *geometries;
	const struct dcm_batch *batches;
	const struct dcm_range *ranges;
	pvr_ptr_t vram[DCM_TEXTURES];
	unsigned long vram_bytes;
	/* each batch's header and list */
	pvr_poly_hdr_t *headers;
	uint8_t *lists;
	/* each change-colour batch's overlay header (dc_model_compile_headers),
	and whether it has one */
	pvr_poly_hdr_t *overlay_headers;
	uint8_t *overlays;
	/* each glowing batch's glow header, and whether it has one */
	pvr_poly_hdr_t *glow_headers;
	uint8_t *glows;
	/* model by tag index */
	int16_t *by_tag;
	int tag_count;
	/* the largest batch's vertices: the transform's scratch */
	int scratch_size;
	pvr_vertex_t *packed;
	dc_cv *cvs;
} M;

static matrix_t dcm_nodes[DCM_NODES] __attribute__((aligned(32)));

void dc_model_unload(void)
{
	int i;

	if (M.data)
		pvr_wait_render_done();
	for (i = 0; i < DCM_TEXTURES; i++)
	{
		if (M.vram[i])
			pvr_mem_free(M.vram[i]);
	}
	free(M.data);
	free(M.headers);
	free(M.overlay_headers);
	free(M.lists);
	free(M.overlays);
	free(M.glow_headers);
	free(M.glows);
	free(M.by_tag);
	free(M.packed);
	free(M.cvs);
	memset(&M, 0, sizeof(M));
}

void dc_model_compile_headers(void)
{
	int b;

	if (!M.data)
		return;
	for (b = 0; b < M.h->batch_count; b++)
	{
		const struct dcm_batch *batch = &M.batches[b];
		int list = dc_material_list(batch->flags);
		int t = batch->texture;

		M.lists[b] = (uint8_t)list;
		M.overlays[b] = 0;
		M.glows[b] = 0;
		if (t >= 0 && t < M.h->texture_count && M.vram[t])
		{
			int overlay = (int)M.textures[t].reserved[0] - 1;

			dc_header_compile_list(&M.headers[b], list, M.vram[t], dcm_format(M.textures[t].format),
				M.textures[t].width, batch->flags);
			/* a change-colour batch's overlay: the texture with the colour mask
			as its alpha, blended over the batch's own surface (its depth
			equal), tinted by the player's colour */
			if ((batch->flags & MATERIAL_CHANGE_COLOR) && overlay >= 0 && overlay < M.h->texture_count &&
				M.vram[overlay])
			{
				dc_header_compile_list(&M.overlay_headers[b], PVR_LIST_TR_POLY, M.vram[overlay],
					dcm_format(M.textures[overlay].format), M.textures[overlay].width,
					(batch->flags & MATERIAL_TWO_SIDED) | DC_HEADER_DEPTH_EQUAL);
				M.overlays[b] = 1;
			}
			/* a glowing batch's glow: added (MATERIAL_GLOW: ONE, ONE, no
			fog) over the batch's own surface */
			if ((batch->flags & MATERIAL_SELF_ILLUM) && !(batch->flags & MATERIAL_DISPLAY) &&
				(M.textures[t].reserved[1] & (DCM_DISPLAY_VALID | DCM_GLOW_VALID)) == DCM_GLOW_VALID)
			{
				int glow = (int)(M.textures[t].reserved[1] & 0xFFFF);

				if (glow < M.h->texture_count && M.vram[glow])
				{
					dc_header_compile_list(&M.glow_headers[b], PVR_LIST_TR_POLY, M.vram[glow],
						dcm_format(M.textures[glow].format), M.textures[glow].width,
						(batch->flags & MATERIAL_TWO_SIDED) | DC_HEADER_DEPTH_EQUAL | MATERIAL_GLOW);
					M.glows[b] = 1;
				}
			}
		}
		else
			dc_header_compile_list(&M.headers[b], list, NULL, 0, 0, batch->flags);
	}
}

static int dcm_section(unsigned long offset, unsigned long count, unsigned long size, unsigned long align)
{
	return offset % align == 0 && offset <= M.size && count * size <= M.size - offset;
}

static void dcm_fail(const char *why)
{
	n64_log(why);
	dc_model_unload();
}

int dc_model_load(const char *map_name)
{
	char path[64], line[160];
	const struct dcm_header *h;
	long file, size;
	unsigned long i;

	dc_model_unload();
	snprintf(path, sizeof(path), "rom:/levels/%s.dcm", map_name);
	file = n64_file_open(path);
	if (file < 0)
	{
		n64_log("dcm: no model file");
		return 0;
	}
	/* (the file but its texture data, its last section, into RAM: the
	textures go from the disc to video memory, a texture at a time -- the
	campaign's models' are 1.7 MB) */
	size = n64_file_size(file);
	{
		struct dcm_header head;
		long kept = size;

		if (n64_file_read(file, &head, sizeof(head)) == (long)sizeof(head) && head.texdata_offset <= (uint32_t)size &&
			head.texdata_offset >= sizeof(head))
			kept = (long)head.texdata_offset;
		M.data = memalign(32, kept);
		if (!M.data || n64_file_seek(file, 0) < 0 || n64_file_read(file, M.data, kept) != kept)
		{
			n64_file_close(file);
			dcm_fail("dcm: could not read the models");
			return 0;
		}
	}
	/* (the sections checked against the whole file) */
	M.size = (unsigned long)size;
	h = M.h = (const struct dcm_header *)M.data;
	if (memcmp(h->magic, "DCM1", 4) || h->version != 1 || h->texture_count > DCM_TEXTURES ||
		!dcm_section(h->textures_offset, h->texture_count, sizeof(struct dcm_texture), 4) ||
		!dcm_section(h->models_offset, h->model_count, sizeof(struct dcm_model), 4) ||
		!dcm_section(h->regions_offset, h->region_count, sizeof(struct dcm_region), 4) ||
		!dcm_section(h->permutations_offset, h->permutation_count, 2, 2) ||
		!dcm_section(h->geometries_offset, h->geometry_count, sizeof(struct dcm_geometry), 4) ||
		!dcm_section(h->batches_offset, h->batch_count, sizeof(struct dcm_batch), 4) ||
		!dcm_section(h->ranges_offset, h->range_count, sizeof(struct dcm_range), 4) ||
		!dcm_section(h->texdata_offset, h->texdata_size, 1, 32))
	{
		n64_file_close(file);
		dcm_fail("dcm: the model file is not this build's");
		return 0;
	}
	/* the textures from the disc to video memory, through a buffer the
	largest one's size */
	M.textures = (const struct dcm_texture *)(M.data + h->textures_offset);
	{
		unsigned long biggest = 32;
		void *bounce;

		for (i = 0; i < h->texture_count; i++)
		{
			const struct dcm_texture *t = &M.textures[i];

			if ((int)(t->format & 0xFF) >= DCM_FORMAT_COUNT || t->width != t->height ||
				t->offset + t->size > h->texdata_size)
			{
				n64_file_close(file);
				dcm_fail("dcm: a texture out of bounds");
				return 0;
			}
			if (t->size > biggest)
				biggest = t->size;
		}
		bounce = memalign(32, biggest);
		for (i = 0; bounce && i < h->texture_count; i++)
		{
			const struct dcm_texture *t = &M.textures[i];

			if (n64_file_seek(file, (long)(h->texdata_offset + t->offset)) < 0 ||
				n64_file_read(file, bounce, (long)t->size) != (long)t->size)
				continue;
			M.vram[i] = pvr_mem_malloc(t->size);
			if (!M.vram[i])
				continue;
			pvr_txr_load(bounce, M.vram[i], t->size);
			M.vram_bytes += t->size;
		}
		free(bounce);
	}
	n64_file_close(file);
	M.size = h->texdata_offset <= M.size && h->texdata_offset >= sizeof(*h) ? h->texdata_offset : M.size;
	M.textures = (const struct dcm_texture *)(M.data + h->textures_offset);
	M.models = (const struct dcm_model *)(M.data + h->models_offset);
	M.regions = (const struct dcm_region *)(M.data + h->regions_offset);
	M.permutations = (const int16_t *)(M.data + h->permutations_offset);
	M.geometries = (const struct dcm_geometry *)(M.data + h->geometries_offset);
	M.batches = (const struct dcm_batch *)(M.data + h->batches_offset);
	M.ranges = (const struct dcm_range *)(M.data + h->ranges_offset);
	/* every model's batches, ranges and data in bounds; the tag table; the
	largest batch */
	M.tag_count = 0;
	for (i = 0; i < h->model_count; i++)
	{
		const struct dcm_model *model = &M.models[i];
		unsigned long b;

		if (model->tag_index >= M.tag_count)
			M.tag_count = model->tag_index + 1;
		if (model->first_batch + model->batch_count > h->batch_count ||
			model->first_batch + model->far_offset + model->far_count > h->batch_count ||
			model->first_region + model->region_count > h->region_count ||
			!dcm_section(model->data_offset, model->vertex_count, sizeof(struct dcm_vertex), 32) ||
			!dcm_section(model->data_offset + model->vertex_count * sizeof(struct dcm_vertex), model->index_count, 2, 2))
		{
			dcm_fail("dcm: a model out of bounds");
			return 0;
		}
		for (b = model->first_batch; b < (unsigned long)model->first_batch + model->far_offset + model->far_count; b++)
		{
			const struct dcm_batch *batch = &M.batches[b];

			if (batch->first_vertex + batch->vertex_count > model->vertex_count ||
				batch->first_index + batch->index_count > model->index_count ||
				batch->first_range + batch->range_count > h->range_count)
			{
				dcm_fail("dcm: a batch out of bounds");
				return 0;
			}
			if (batch->vertex_count > M.scratch_size)
				M.scratch_size = batch->vertex_count;
		}
	}
	for (i = 0; i < h->region_count; i++)
	{
		if (M.regions[i].first_permutation + M.regions[i].permutation_count > h->permutation_count)
		{
			dcm_fail("dcm: a region out of bounds");
			return 0;
		}
	}
	for (i = 0; i < h->permutation_count; i++)
	{
		if (M.permutations[i] >= (int)h->geometry_count)
		{
			dcm_fail("dcm: a permutation out of bounds");
			return 0;
		}
	}
	for (i = 0; i < h->geometry_count; i++)
	{
		if (M.geometries[i].first_batch + M.geometries[i].batch_count > h->batch_count)
		{
			dcm_fail("dcm: a geometry out of bounds");
			return 0;
		}
	}
	M.by_tag = malloc(M.tag_count * sizeof(int16_t));
	M.headers = memalign(32, h->batch_count * sizeof(pvr_poly_hdr_t));
	M.overlay_headers = memalign(32, h->batch_count * sizeof(pvr_poly_hdr_t));
	M.lists = malloc(h->batch_count);
	M.overlays = malloc(h->batch_count);
	M.glow_headers = memalign(32, h->batch_count * sizeof(pvr_poly_hdr_t));
	M.glows = malloc(h->batch_count);
	M.packed = memalign(32, M.scratch_size * sizeof(pvr_vertex_t));
	/* (a strip's, up to DCM_STRIP_MAX) */
	M.cvs = malloc(DCM_STRIP_MAX * sizeof(dc_cv));
	if (!M.by_tag || !M.headers || !M.overlay_headers || !M.lists || !M.overlays || !M.glow_headers || !M.glows ||
		!M.packed || !M.cvs)
	{
		dcm_fail("dcm: no memory for the models' tables");
		return 0;
	}
	for (i = 0; i < (unsigned long)M.tag_count; i++)
		M.by_tag[i] = -1;
	for (i = 0; i < h->model_count; i++)
		M.by_tag[M.models[i].tag_index] = (int16_t)i;
	dc_model_compile_headers();
	{
		int glowing = 0;

		for (i = 0; i < h->batch_count; i++)
			glowing += M.glows[i];
		snprintf(line, sizeof(line), "dcm: %s, %u models, %u batches (%d glowing), %lu KB; %u textures, %lu KB",
			map_name, h->model_count, h->batch_count, glowing, M.size / 1024, h->texture_count, M.vram_bytes / 1024);
	}
	n64_log(line);
	return 1;
}

/* the game's real_matrix4x3 (scale, forward, left, up, position) as a
column matrix, times the camera */
static void dcm_node_matrix(matrix_t *out, const float *m)
{
	matrix_t node __attribute__((aligned(32)));
	float s = m[0];

	node[0][0] = m[1] * s;
	node[0][1] = m[2] * s;
	node[0][2] = m[3] * s;
	node[0][3] = 0.0f;
	node[1][0] = m[4] * s;
	node[1][1] = m[5] * s;
	node[1][2] = m[6] * s;
	node[1][3] = 0.0f;
	node[2][0] = m[7] * s;
	node[2][1] = m[8] * s;
	node[2][2] = m[9] * s;
	node[2][3] = 0.0f;
	node[3][0] = m[10];
	node[3][1] = m[11];
	node[3][2] = m[12];
	node[3][3] = 1.0f;
	mat_load(&dc_cam.m);
	mat_apply(&node);
	mat_store(out);
}

static inline uint32_t dcm_tinted(uint32_t argb, const unsigned char *tint)
{
	/* (n64_div255: exact, no 64-bit multiply; a vertex a strip index) */
	uint32_t r = n64_div255(((argb >> 16) & 255) * tint[0]);
	uint32_t g = n64_div255(((argb >> 8) & 255) * tint[1]);
	uint32_t b = n64_div255((argb & 255) * tint[2]);

	return (argb & 0xFF000000u) | r << 16 | g << 8 | b;
}

/* the scratch vertex's depth (w), near objects only: kept where its offset
colour goes (sent as 0) */
#define DCM_W_MINIMUM 1e-4f

static inline float dcm_w(const pvr_vertex_t *p)
{
	union { uint32_t u; float f; } w;

	w.u = p->oargb;
	return w.f;
}

static inline void dcm_set_w(pvr_vertex_t *p, float f)
{
	union { uint32_t u; float f; } w;

	w.f = f;
	p->oargb = w.u;
}

/* one batch's ranges by their nodes' matrices into the packed scratch:
each vertex once, divided, tinted, as the tile accelerator takes it, so
that a strip index is a 32-byte copy (a vertex is in two or three strips'
triangles: the divide and the tint were done again for each, and the
first-person weapon, always "near", went through the clip's per-index
divide). Returns the nearest depth (only worked out when near: an object
wholly beyond the near plane needs no test) */
static float dcm_transform(const struct dcm_batch *batch, const struct dcm_vertex *v, int node_count, int near,
	const unsigned char *tint)
{
	pvr_vertex_t *p = M.packed;
	float nearest = 1e30f;
	int k = 0, r;

	for (r = 0; r < batch->range_count; r++)
	{
		const struct dcm_range *range = &M.ranges[batch->first_range + r];
		int node = range->node < node_count ? range->node : 0;
		int end = k + range->count;

		if (end > batch->vertex_count)
			end = batch->vertex_count;
		mat_load(&dcm_nodes[node]);
		if (near)
		{
			for (; k < end; k++)
			{
				float x = v[k].x, y = v[k].y, z = v[k].z, w = 1.0f, iw;

				mat_trans_nodiv(x, y, z, w);
				if (w < nearest)
					nearest = w;
				/* (behind the eye: the strip goes through the clip instead) */
				iw = 1.0f / (w > DCM_W_MINIMUM ? w : DCM_W_MINIMUM);
				p[k].x = x * iw;
				p[k].y = y * iw;
				p[k].z = iw * dc_zscale;
				p[k].u = v[k].u;
				p[k].v = v[k].v;
				p[k].argb = tint ? dcm_tinted(v[k].argb, tint) : v[k].argb;
				dcm_set_w(&p[k], w);
			}
		}
		else
		{
			for (; k < end; k++)
			{
				float x = v[k].x, y = v[k].y, z = v[k].z;

				mat_trans_single(x, y, z);
				p[k].x = x;
				p[k].y = y;
				p[k].z = z * dc_zscale;
				p[k].u = v[k].u;
				p[k].v = v[k].v;
				p[k].argb = tint ? dcm_tinted(v[k].argb, tint) : v[k].argb;
			}
		}
	}
	return nearest;
}

/* a strip index's vertex from the scratch, as the tile accelerator takes it */
static inline void dcm_emit(const pvr_vertex_t *p, int last)
{
	const uint32_t *s = (const uint32_t *)p;
	uint32_t *d = (uint32_t *)dc_slot();

	d[0] = last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
	d[1] = s[1];
	d[2] = s[2];
	d[3] = s[3];
	d[4] = s[4];
	d[5] = s[5];
	d[6] = s[6];
	d[7] = 0;
	dc_commit((pvr_vertex_t *)d);
}

/* a batch that crosses the near plane: each strip wholly in front sent
from the scratch as it is, the others through the clip (dc_render.c),
their vertices undivided again from the scratch's x/w, y/w and w */
static void dcm_draw_clipped(const struct dcm_batch *batch, const uint16_t *ix)
{
	const pvr_vertex_t *p = M.packed;
	uint32_t i, first = 0;

	for (i = 0; i < batch->index_count; i++)
	{
		uint32_t k;
		int n = 0, behind = 0;

		if (!(ix[i] & 0x8000))
			continue;
		/* the strip first..i */
		for (k = first; k <= i; k++)
		{
			unsigned j = ix[k] & 0x7FFF;

			if (j < (unsigned)batch->vertex_count && dcm_w(&p[j]) < dc_cam.near)
				behind = 1;
		}
		if (!behind)
		{
			for (k = first; k <= i; k++)
			{
				unsigned j = ix[k] & 0x7FFF;

				dcm_emit(&p[j < (unsigned)batch->vertex_count ? j : 0], k == i);
			}
		}
		else
		{
			const dc_cv *strip[DCM_STRIP_MAX];

			for (k = first; k <= i && n < DCM_STRIP_MAX; k++)
			{
				unsigned j = ix[k] & 0x7FFF;
				float w, wc;
				dc_cv *c = &M.cvs[n];

				if (j >= (unsigned)batch->vertex_count)
					continue;
				w = dcm_w(&p[j]);
				wc = w > DCM_W_MINIMUM ? w : DCM_W_MINIMUM;
				c->x = p[j].x * wc;
				c->y = p[j].y * wc;
				c->w = w;
				c->u = p[j].u;
				c->v = p[j].v;
				c->argb = p[j].argb;
				strip[n++] = c;
			}
			if (n >= 3)
				dc_clip_strip(strip, n);
		}
		first = i + 1;
	}
}

/* one batch of a model: its vertices transformed, its strips sent (through
the near-plane clip when near and the batch reaches past it) */
static void dcm_draw_batch(const struct dcm_model *model, int b, int node_count, int near,
	const unsigned char *tint)
{
	const struct dcm_batch *batch = &M.batches[b];
	const struct dcm_vertex *v = (const struct dcm_vertex *)(M.data + model->data_offset) + batch->first_vertex;
	const uint16_t *ix = (const uint16_t *)(M.data + model->data_offset + model->vertex_count * sizeof(struct dcm_vertex)) +
		batch->first_index;
	const unsigned char *batch_tint = (batch->flags & MATERIAL_CHANGE_COLOR) ? tint : NULL;
	/* (with an overlay, the batch itself untinted: the overlay is) */
	int overlay = batch_tint && M.overlays[b];
	uint32_t i;
	int clipped;

	if (overlay)
		batch_tint = NULL;
	dc_use_list(M.lists[b]);
	/* (a staged list's room: a strip index is one vertex, but through the
	clip a crossing triangle can become four, and a run of whole ones gets
	its lead vertex and last two: four an index at most) */
	clipped = near && dcm_transform(batch, v, node_count, 1, batch_tint) < dc_cam.near;
	if (!dc_stage_room(batch->index_count * (clipped ? 4 : 1) + 1))
		return;
	if ((batch->flags & MATERIAL_DISPLAY) && dcm_functions)
	{
		/* the frame the weapon's state shows (rasterizer_xbox_transparent_
		geometry.c's numeric shaders: round(limit x value), its digit in
		the place; a meter's fill, the same with one digit) */
		static pvr_poly_hdr_t display_header __attribute__((aligned(32)));
		uint32_t info = M.textures[batch->texture].reserved[1];
		int frames = (int)(info & 255), limit = (int)((info >> 8) & 255), function = (int)((info >> 16) & 3);
		float value = dcm_functions[function];
		int counter = (int)(limit * (value < 0.f ? 0.f : value > 1.f ? 1.f : value) + 0.5f), t;

		if (!(info & DCM_DISPLAY_VALID) || frames < 1)
			return;
		if (batch->flags & MATERIAL_DISPLAY_TENS)
			counter /= frames;
		t = batch->texture + counter % frames;
		if (t >= M.h->texture_count || !M.vram[t])
			return;
		dc_header_compile_list(&display_header, M.lists[b], M.vram[t], dcm_format(M.textures[t].format),
			M.textures[t].width, batch->flags);
		dc_send_header(&display_header);
	}
	else
		dc_send_header(&M.headers[b]);
	if (clipped)
		dcm_draw_clipped(batch, ix);
	else
	{
		if (!near)
			dcm_transform(batch, v, node_count, 0, batch_tint);
		for (i = 0; i < batch->index_count; i++)
		{
			unsigned j = ix[i] & 0x7FFF;

			dcm_emit(&M.packed[j < (unsigned)batch->vertex_count ? j : 0], ix[i] & 0x8000);
		}
	}
	if (overlay)
	{
		/* the same vertices again, tinted, on the translucent list */
		const struct dcm_vertex *source = v;
		int k;

		for (k = 0; k < batch->vertex_count; k++)
			M.packed[k].argb = dcm_tinted(source[k].argb, tint) | 0xFF000000u;
		dc_use_list(PVR_LIST_TR_POLY);
		if (dc_stage_room(batch->index_count * (clipped ? 4 : 1) + 1))
		{
			dc_send_header(&M.overlay_headers[b]);
			if (clipped)
				dcm_draw_clipped(batch, ix);
			else
			{
				for (i = 0; i < batch->index_count; i++)
				{
					unsigned j = ix[i] & 0x7FFF;

					dcm_emit(&M.packed[j < (unsigned)batch->vertex_count ? j : 0], ix[i] & 0x8000);
				}
			}
		}
	}
	if (M.glows[b])
	{
		/* the same vertices again, white (the glow unlit), added on the
		translucent list */
		int k;

		for (k = 0; k < batch->vertex_count; k++)
			M.packed[k].argb = 0xFFFFFFFFu;
		dc_use_list(PVR_LIST_TR_POLY);
		if (dc_stage_room(batch->index_count * (clipped ? 4 : 1) + 1))
		{
			dc_send_header(&M.glow_headers[b]);
			if (clipped)
				dcm_draw_clipped(batch, ix);
			else
			{
				for (i = 0; i < batch->index_count; i++)
				{
					unsigned j = ix[i] & 0x7FFF;

					dcm_emit(&M.packed[j < (unsigned)batch->vertex_count ? j : 0], ix[i] & 0x8000);
				}
			}
		}
	}
	dc_stat.model_triangles += batch->triangle_count;
	if (dc_first_person)
		dc_stat.first_person_triangles += batch->triangle_count;
}

int n64_render_object(const struct n64_object *object)
{
	const struct dcm_model *model;
	int index, node_count, test, far = 0, near, r, n;
	uint64_t start;

	if (!M.data || object->model_tag_index < 0 || object->model_tag_index >= M.tag_count ||
		(index = M.by_tag[object->model_tag_index]) < 0)
		return 0;
	model = &M.models[index];
	dcm_functions = object->functions;
	if (!dc_first_person)
	{
		/* testing setting "dc_no_objects": the world's objects not drawn */
		static int no_objects = -1;

		if (no_objects < 0)
			no_objects = n64_debug_flag("dc_no_objects");
		if (no_objects)
			return 0;
		/* past the fog, smaller than a pixel and a half, or small enough for
		its far version (n64_render.c), by its size in pixels: 2 r k / d */
		float dx = object->center[0] - dc_cam.eye[0], dy = object->center[1] - dc_cam.eye[1];
		float dz = object->center[2] - dc_cam.eye[2];
		float distance_squared = dx * dx + dy * dy + dz * dz;
		float radius = object->radius;

		if (distance_squared > (dc_cam.far + radius) * (dc_cam.far + radius))
		{
			dc_stat.objects_culled++;
			return 0;
		}
		if (distance_squared > radius * radius)
		{
			float size = radius * 2.0f * dc_cam.k;

			size *= size;
			if (size < DCM_MINIMUM_PIXELS * DCM_MINIMUM_PIXELS * distance_squared)
			{
				dc_stat.objects_culled++;
				return 0;
			}
			far = model->far_count && size < DCM_FAR_PIXELS * DCM_FAR_PIXELS * distance_squared;
		}
	}
	test = dc_sphere_test(&dc_cam, object->center, object->radius + 0.125f);
	if (test == DC_OUT)
	{
		dc_stat.objects_culled++;
		return 0;
	}
	node_count = object->node_count < model->node_count ? object->node_count : model->node_count;
	if (node_count <= 0)
		return 1;
	if (node_count > DCM_NODES)
		node_count = DCM_NODES;
	start = timer_us_gettime64();
	for (n = 0; n < node_count; n++)
		dcm_node_matrix(&dcm_nodes[n], object->node_matrices + n * 13);
	near = dc_first_person || (test & DC_NEAR);
	{
		/* (the tint: a unit's colour, for its change-colour batches; white
		draws as it is) */
		const unsigned char *tint = object->tint;
		int white = tint[0] == 255 && tint[1] == 255 && tint[2] == 255;

		if (white)
			tint = NULL;
		if (far)
		{
			int b;

			for (b = model->first_batch + model->far_offset; b < model->first_batch + model->far_offset + model->far_count;
				b++)
				dcm_draw_batch(model, b, node_count, near, tint);
			dc_stat.objects_far++;
		}
		else
		{
			for (r = 0; r < model->region_count; r++)
			{
				const struct dcm_region *region = &M.regions[model->first_region + r];
				int permutation = r < object->region_count && object->region_permutations ? object->region_permutations[r] : 0;
				int geometry, b;

				if (!region->permutation_count)
					continue;
				if (permutation < 0 || permutation >= region->permutation_count)
					permutation = 0;
				geometry = M.permutations[region->first_permutation + permutation];
				if (geometry < 0)
					continue;
				for (b = M.geometries[geometry].first_batch;
					b < M.geometries[geometry].first_batch + M.geometries[geometry].batch_count; b++)
					dcm_draw_batch(model, b, node_count, near, tint);
			}
		}
	}
	dc_use_list(PVR_LIST_OP_POLY);
	dc_stat.objects++;
	{
		unsigned long us = (unsigned long)(timer_us_gettime64() - start);

		dc_stat.model_us += us;
		if (dc_first_person)
			dc_stat.first_person_us += us;
	}
	return 1;
}

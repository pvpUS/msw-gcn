#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <gccore.h>
#include <ogc/tpl.h>

#include "world.h"
#include "lz.h"
#include "block_shapes.h"
#include "atlas_tpl.h"        /* generated: atlas_tpl[], atlas_tpl_size */
#include "block_faces_gen.h"  /* generated: g_topTile[], g_bottomTile[] */
#include "atlas_gen.h"        /* generated: ATLAS_TEX_W/H, ATLAS_CELL, ... */
#include "block_shapes_gen.h" /* generated: g_blockShape[], g_blockParam[] */

/* Texcoords are GX_U16 with 10 fractional bits, so a stored value V maps to
 * V/1024 of the texture's width/height. Each tile sits inside an ATLAS_PAD
 * px clamped border (see tools/build_atlas.py's pad_tile()); UV_LO/UV_HI
 * land exactly on the first/last *texel*'s center (px+0.5 .. px+TILE-0.5),
 * inset from the padded cell, so mip-mapped/bilinear sampling never reaches
 * past a tile's own padding into a real neighbour tile. This runs once per
 * face while building the display list, not per frame, so plain floats are
 * fine here. */
static inline u16 uv_raw(float atlasPixel, float atlasDim) {
	return (u16)(atlasPixel * 1024.0f / atlasDim + 0.5f);
}
#define UV_LO(px, dim) uv_raw((px) + 0.5f, (dim))
#define UV_HI(px, dim) uv_raw((px) + ATLAS_TILE - 0.5f, (dim))

#define BATCH_QUADS 16000  /* keep GX_Begin vertex counts under 65535 */

static TPLFile  atlasTPL;
static GXTexObj atlasTex;

/* Unit-cube corners per face and their tile UV corners.
 * Face order: 0:-X 1:+X 2:-Y 3:+Y(top) 4:-Z 5:+Z */
static const s16 faceVerts[6][4][3] = {
	{ {0,0,0},{0,0,1},{0,1,1},{0,1,0} }, /* -X */
	{ {1,0,1},{1,0,0},{1,1,0},{1,1,1} }, /* +X */
	{ {0,0,0},{1,0,0},{1,0,1},{0,0,1} }, /* -Y */
	{ {0,1,1},{1,1,1},{1,1,0},{0,1,0} }, /* +Y */
	{ {1,0,0},{0,0,0},{0,1,0},{1,1,0} }, /* -Z */
	{ {0,0,1},{1,0,1},{1,1,1},{0,1,1} }, /* +Z */
};
/* UV corner (u,v) in {0,1} for each vertex above; v=0 is the tile's top. */
static const u8 faceUV[6][4][2] = {
	{ {0,1},{1,1},{1,0},{0,0} }, /* -X */
	{ {0,1},{1,1},{1,0},{0,0} }, /* +X */
	{ {0,0},{1,0},{1,1},{0,1} }, /* -Y */
	{ {0,0},{1,0},{1,1},{0,1} }, /* +Y */
	{ {0,1},{1,1},{1,0},{0,0} }, /* -Z */
	{ {0,1},{1,1},{1,0},{0,0} }, /* +Z */
};
static const int faceNormal[6][3] = {
	{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},
};
/* Minecraft-style directional ambient shade per face. */
static const u8 faceShade[6] = { 153, 153, 128, 255, 204, 204 };

/* ---- little-endian? no: blobs are big-endian ------------------------- */
static inline u16 rd_u16(const u8 *p) { return ((u16)p[0] << 8) | p[1]; }
static inline s16 rd_s16(const u8 *p) { return (s16)rd_u16(p); }
static inline u32 rd_u32(const u8 *p) {
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static inline u32 rd_uvarint(const u8 *s, u32 *pp) {
	u32 p = *pp, r = 0, sh = 0, b;
	do { b = s[p++]; r |= (u32)(b & 0x7F) << sh; sh += 7; } while (b & 0x80);
	*pp = p;
	return r;
}

/* ---- occupancy grid --------------------------------------------------- */
typedef struct {
	u8 *occ;
	int dimx, dimy, dimz;
} Occ;

static inline u32 occ_index(Occ *o, int x, int y, int z) {
	return ((u32)x * o->dimz + z) * o->dimy + y;
}
static inline void occ_set(Occ *o, int x, int y, int z) {
	u32 i = occ_index(o, x, y, z);
	o->occ[i >> 3] |= (u8)(1 << (i & 7));
}
static inline int occ_solid(Occ *o, int x, int y, int z) {
	if (x < 0 || x >= o->dimx || y < 0 || y >= o->dimy || z < 0 || z >= o->dimz)
		return 0;
	u32 i = occ_index(o, x, y, z);
	return (o->occ[i >> 3] >> (i & 7)) & 1;
}
/* Inverse of occ_index(): recovers grid coords from a linear index. */
static inline void occ_decode(Occ *o, u32 idx, int *x, int *y, int *z) {
	*y = (int)(idx % (u32)o->dimy);
	u32 rest = idx / (u32)o->dimy;
	*z = (int)(rest % (u32)o->dimz);
	*x = (int)(rest / (u32)o->dimz);
}

/* ---- voxel stream walk ------------------------------------------------ */
typedef void (*VoxFn)(void *ctx, int x, int y, int z, int li);

static void WalkVoxels(const u8 *S, int dimz, u32 ncol, int idbytes,
                       VoxFn fn, void *ctx) {
	u32 p = 0;
	u32 c = 0;
	u32 i;
	for (i = 0; i < ncol; i++) {
		c += rd_uvarint(S, &p);
		int x = (int)(c / (u32)dimz);
		int z = (int)(c % (u32)dimz);
		u32 nseg = rd_uvarint(S, &p);
		int y = 0;
		u32 s;
		for (s = 0; s < nseg; s++) {
			u32 gap = rd_uvarint(S, &p);
			u32 run = rd_uvarint(S, &p) + 1;
			int li;
			if (idbytes == 1) { li = S[p]; p += 1; }
			else { li = ((int)S[p] << 8) | S[p + 1]; p += 2; }
			y += (int)gap;
			u32 k;
			for (k = 0; k < run; k++) fn(ctx, x, y + (int)k, z, li);
			y += (int)run;
		}
	}
}

static void cb_occ(void *ctx, int x, int y, int z, int li) {
	(void)li;
	occ_set((Occ *)ctx, x, y, z);
}

/* ---- sparse non-cube shape table --------------------------------------
 * Collision (World_BlockBoxes) and, from milestone 2 on, custom mesh
 * emission both need "what shape/param does the block at (x,y,z) have"
 * outside the palette-driven WalkVoxels passes. Only the (typically small)
 * subset of occupied voxels whose shape isn't SHAPE_CUBE is recorded here,
 * keyed by the same linear index scheme as occ[] (occ_index above).
 * WalkVoxels visits voxels in strictly ascending occ_index order (columns
 * in ascending (x*dimz+z), y ascending within a column), so entries are
 * appended already sorted -- no separate sort pass needed. */
typedef struct {
	u32 *idx;
	u8  *shape;
	u8  *param;
	u8  *connect;  /* neighbor connect mask, FENCE/WALL/PANE only; 0 else */
	u32  count;
	u32  cap;
} ShapeGrid;

static void shapegrid_push(ShapeGrid *sg, u32 idx, u8 shape, u8 param) {
	if (sg->count == sg->cap) {
		sg->cap    = sg->cap ? sg->cap * 2 : 64;
		sg->idx    = realloc(sg->idx,    sg->cap * sizeof(u32));
		sg->shape  = realloc(sg->shape,  sg->cap * sizeof(u8));
		sg->param  = realloc(sg->param,  sg->cap * sizeof(u8));
		sg->connect= realloc(sg->connect,sg->cap * sizeof(u8));
	}
	sg->idx[sg->count]    = idx;
	sg->shape[sg->count]  = shape;
	sg->param[sg->count]  = param;
	sg->connect[sg->count]= 0;
	sg->count++;
}

/* Binary search for a linear voxel index; returns its ShapeGrid slot or -1
 * if that voxel is absent (i.e. a plain full cube, or unoccupied). */
static int shapegrid_find(const ShapeGrid *sg, u32 idx) {
	int lo = 0, hi = (int)sg->count - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (sg->idx[mid] == idx) return mid;
		if (sg->idx[mid] < idx) lo = mid + 1; else hi = mid - 1;
	}
	return -1;
}

typedef struct {
	Occ *o;
	const u8 *palette;
	ShapeGrid *sg;
} ShapeCtx;

static void cb_shape(void *ctx, int x, int y, int z, int li) {
	ShapeCtx *sc = (ShapeCtx *)ctx;
	int g = rd_u16(sc->palette + li * 2);
	u8 shape = g_blockShape[g];
	if (shape == SHAPE_CUBE) return;
	shapegrid_push(sc->sg, occ_index(sc->o, x, y, z), shape, g_blockParam[g]);
}

/* Fills sg->connect[] for FENCE/WALL/PANE entries: bit0=-X,1=+X,2=-Z,3=+Z,
 * set when that neighbor is occupied and is either a plain full cube or
 * another entry of the *same* shape (matches vanilla's "connects to solid
 * blocks and same-family posts" rule; deliberately not species-specific,
 * e.g. all fence species connect to each other, mirroring BlockFence). Must
 * run after the ShapeGrid is fully built (needs random-access neighbor
 * lookups WalkVoxels' single forward pass can't provide). */
static void shapegrid_link(ShapeGrid *sg, Occ *o) {
	static const int dx[4] = {-1, 1, 0, 0};
	static const int dz[4] = {0, 0, -1, 1};
	u32 i;
	for (i = 0; i < sg->count; i++) {
		u8 shape = sg->shape[i];
		if (shape != SHAPE_FENCE && shape != SHAPE_WALL && shape != SHAPE_PANE)
			continue;
		int x, y, z;
		occ_decode(o, sg->idx[i], &x, &y, &z);
		u8 mask = 0;
		int d;
		for (d = 0; d < 4; d++) {
			int nx = x + dx[d], nz = z + dz[d];
			if (!occ_solid(o, nx, y, nz)) continue;
			int ni = shapegrid_find(sg, occ_index(o, nx, y, nz));
			if (ni < 0 || sg->shape[ni] == shape) mask |= (u8)(1 << d);
		}
		sg->connect[i] = mask;
	}
}

typedef struct {
	Occ *o;
	const u8 *palette;   /* points into blob: big-endian u16 per local id */
	ShapeGrid *sg;        /* for FENCE/WALL/PANE connect-mask lookup       */
	u32 faceCount;       /* total (known before emit)                     */
	u32 faceIdx;         /* running index while emitting                  */
	int emit;            /* 0 = count, 1 = emit                           */
} FaceCtx;

/* Shared quad emitter for both the full-cube path and BlockShape_Mesh's
 * custom shapes: `x0,y0,z0`..`x1,y1,z1` are a bounding box in sixteenths of a
 * block, local to voxel (vx,vy,vz); `face` picks which 4 of its 8 corners
 * make up that face (same faceVerts/faceUV/faceShade convention as before).
 * Handles the count-vs-emit split and BATCH_QUADS GX_Begin/End bookkeeping,
 * so callers (cb_face's cube loop, shape_quad_sink) don't need to. */
static void emit_quad(FaceCtx *fc, int vx, int vy, int vz, int face,
                      s16 x0, s16 y0, s16 z0, s16 x1, s16 y1, s16 z1, int tile) {
	if (!fc->emit) { fc->faceCount++; return; }

	if (fc->faceIdx % BATCH_QUADS == 0) {
		u32 rem = fc->faceCount - fc->faceIdx;
		u32 n = rem < BATCH_QUADS ? rem : BATCH_QUADS;
		GX_Begin(GX_QUADS, GX_VTXFMT0, n * 4);
	}

	int col = tile % ATLAS_COLS;
	int row = tile / ATLAS_COLS;
	int px0 = col * ATLAS_CELL + ATLAS_PAD;
	int py0 = row * ATLAS_CELL + ATLAS_PAD;
	u16 u0 = UV_LO(px0, ATLAS_TEX_W), u1 = UV_HI(px0, ATLAS_TEX_W);
	u16 v0 = UV_LO(py0, ATLAS_TEX_H), v1 = UV_HI(py0, ATLAS_TEX_H);
	u8 sh = faceShade[face];
	s16 bx[2] = {x0, x1}, by[2] = {y0, y1}, bz[2] = {z0, z1};

	int v;
	for (v = 0; v < 4; v++) {
		GX_Position3s16((s16)(vx * 16 + bx[faceVerts[face][v][0]]),
		                (s16)(vy * 16 + by[faceVerts[face][v][1]]),
		                (s16)(vz * 16 + bz[faceVerts[face][v][2]]));
		GX_Color4u8(sh, sh, sh, 255);
		GX_TexCoord2u16(faceUV[face][v][0] ? u1 : u0,
		                faceUV[face][v][1] ? v1 : v0);
	}

	fc->faceIdx++;
	if (fc->faceIdx % BATCH_QUADS == 0) GX_End();
}

typedef struct { FaceCtx *fc; int x, y, z; } ShapeSink;

static void shape_quad_sink(void *ctx, int face, s16 x0, s16 y0, s16 z0,
                            s16 x1, s16 y1, s16 z1, int tile) {
	ShapeSink *sk = (ShapeSink *)ctx;
	emit_quad(sk->fc, sk->x, sk->y, sk->z, face, x0, y0, z0, x1, y1, z1, tile);
}

static void cb_face(void *ctx, int x, int y, int z, int li) {
	FaceCtx *fc = (FaceCtx *)ctx;
	int g = rd_u16(fc->palette + li * 2);
	u8 shape = g_blockShape[g];

	if (shape != SHAPE_CUBE) {
		u8 param = g_blockParam[g];
		u8 connect = 0;
		if (shape == SHAPE_FENCE || shape == SHAPE_WALL || shape == SHAPE_PANE) {
			int si = shapegrid_find(fc->sg, occ_index(fc->o, x, y, z));
			connect = (si >= 0) ? fc->sg->connect[si] : 0;
		}
		ShapeSink sk = { fc, x, y, z };
		/* No neighbor face-culling for custom shapes -- unlike full cubes,
		 * their faces don't line up with the voxel boundary, so "is the
		 * neighbor solid" doesn't reliably mean "is this face hidden"
		 * (e.g. a bottom slab's top face is exposed regardless of what's in
		 * the voxel above it). Always emitting the shape's own faces trades
		 * a little overdraw on a minority of blocks for never leaving a
		 * gap; GX_CULL_NONE means there's no correctness downside. */
		BlockShape_Mesh(shape, param, connect, g, shape_quad_sink, &sk);
		return;
	}

	int f;
	for (f = 0; f < 6; f++) {
		if (occ_solid(fc->o, x + faceNormal[f][0], y + faceNormal[f][1],
		              z + faceNormal[f][2]))
			continue;
		/* Face order: 2:-Y(bottom) 3:+Y(top), others use the side tile. */
		int tile = (f == 3) ? g_topTile[g] : (f == 2) ? g_bottomTile[g] : g;
		emit_quad(fc, x, y, z, f, 0, 0, 0, 16, 16, 16, tile);
	}
}

/* ---- collision-box wireframe overlay (MODEL_TEST_MODE, see main.c) ---
 * Reuses GX_VTXFMT0 (same POS+CLR0+TEX0 layout as the real mesh) so no
 * separate vertex format/desc is needed -- World_Draw disables texturing via
 * a TEV state flip instead, so the filler texcoord below is simply unused
 * while that flat-color stage is active. */
static inline s16 wire_coord(int block, float frac) {
	return (s16)(block * 16 + (int)(frac * 16.0f + 0.5f));
}

static void emit_wire_box(int bx, int by, int bz, const BlockAABB *box) {
	s16 X[2] = { wire_coord(bx, box->x0), wire_coord(bx, box->x1) };
	s16 Y[2] = { wire_coord(by, box->y0), wire_coord(by, box->y1) };
	s16 Z[2] = { wire_coord(bz, box->z0), wire_coord(bz, box->z1) };
	/* 12 edges of a box, each as a pair of {x,y,z} corner selectors (0/1
	 * indexing into X/Y/Z above). */
	static const u8 edges[12][2][3] = {
		{{0,0,0},{1,0,0}}, {{1,0,0},{1,0,1}}, {{1,0,1},{0,0,1}}, {{0,0,1},{0,0,0}},
		{{0,1,0},{1,1,0}}, {{1,1,0},{1,1,1}}, {{1,1,1},{0,1,1}}, {{0,1,1},{0,1,0}},
		{{0,0,0},{0,1,0}}, {{1,0,0},{1,1,0}}, {{1,0,1},{1,1,1}}, {{0,0,1},{0,1,1}},
	};
	int e, v;
	GX_Begin(GX_LINES, GX_VTXFMT0, 24);
	for (e = 0; e < 12; e++) {
		for (v = 0; v < 2; v++) {
			const u8 *c = edges[e][v];
			GX_Position3s16(X[c[0]], Y[c[1]], Z[c[2]]);
			GX_Color4u8(255, 0, 255, 255); /* bright magenta: never a real block color */
			GX_TexCoord2u16(0, 0);
		}
	}
	GX_End();
}

/* ---- public API ------------------------------------------------------- */
void World_InitGX(void) {
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);

	/* 4 fractional bits: positions are s16 in sixteenths of a block, matching
	 * Minecraft's own 16-unit block-model grid so vanilla model coordinates
	 * (slab half-heights, stair corners, fence-post insets, ...) map to
	 * integers directly. Full-cube corners are always whole blocks (0 or 16
	 * sixteenths), so scaling cb_face's positions by 16 (below) is a no-op
	 * for existing worlds -- same geometry, just finer-grained encoding. */
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 4);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_U16, 10);

	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);

	/* solid rendering with cutout support (leaves, glass, flowers) */
	GX_SetAlphaCompare(GX_GEQUAL, 128, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetZCompLoc(GX_FALSE);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetCullMode(GX_CULL_NONE);

	TPL_OpenTPLFromMemory(&atlasTPL, (void *)atlas_tpl, atlas_tpl_size);
	TPL_GetTexture(&atlasTPL, 0, &atlasTex);
	/* GX_NEAR_MIP_LIN when minified (viewed at any distance): point-sampled
	 * within a level (keeps texels crisp/blocky) but blended between the
	 * ATLAS_MAXLOD mip levels baked into the TPL, which are pre-averaged so
	 * a noisy 16x16 texture doesn't alias into a moire pattern at distance.
	 * Safe against atlas-neighbour bleeding because every tile is packed
	 * with an ATLAS_PAD clamped border (tools/build_atlas.py's pad_tile())
	 * wide enough to absorb the box filter's reach at ATLAS_MAXLOD.
	 * GX_NEAR when magnified keeps the crisp blocky look up close. */
	GX_InitTexObjLOD(&atlasTex, GX_NEAR_MIP_LIN, GX_NEAR,
	                  0.0f, (float)ATLAS_MAXLOD, 0.0f, GX_ENABLE, GX_ENABLE, GX_ANISO_1);
	GX_InitTexObjWrapMode(&atlasTex, GX_CLAMP, GX_CLAMP);
}

int World_Load(World *w, const u8 *blob, u32 blobLen) {
	(void)blobLen;
	memset(w, 0, sizeof(*w));

	if (blob[0] != 'M' || blob[1] != 'W' || blob[2] != 'L' || blob[3] != '1')
		return 0;

	int idbytes = blob[5];
	u16 palcount = rd_u16(blob + 6);
	w->minx = rd_s16(blob + 8);
	w->miny = rd_s16(blob + 10);
	w->minz = rd_s16(blob + 12);
	w->dimx = rd_u16(blob + 14);
	w->dimy = rd_u16(blob + 16);
	w->dimz = rd_u16(blob + 18);
	w->spawnx = rd_s16(blob + 20);
	w->spawny = rd_s16(blob + 22);
	w->spawnz = rd_s16(blob + 24);
	w->blocks = rd_u32(blob + 26);
	u32 ncol = rd_u32(blob + 30);
	u32 rawS = rd_u32(blob + 34);

	const u8 *palette = blob + 38;
	const u8 *comp = palette + (u32)palcount * 2;

	/* 1. decompress the structural stream */
	u8 *S = memalign(32, rawS);
	if (!S) return 0;
	LZ_Decompress(comp, S, rawS);

	/* 2. build occupancy grid */
	Occ o;
	o.dimx = w->dimx; o.dimy = w->dimy; o.dimz = w->dimz;
	u32 nbits = (u32)w->dimx * w->dimy * w->dimz;
	u32 occBytes = (nbits + 7) / 8;
	o.occ = calloc(occBytes, 1);
	if (!o.occ) { free(S); return 0; }
	WalkVoxels(S, w->dimz, ncol, idbytes, cb_occ, &o);

	/* Retain the occupancy grid for runtime collision queries; freed in
	 * World_Free. Its layout matches occ_index() / World_BlockSolid(). */
	w->occ = o.occ;

	/* 2.5. build the sparse non-cube shape table (collision + custom mesh
	 * dispatch, see the ShapeGrid comment above). Must run before meshing
	 * (step 3/4) so FENCE/WALL/PANE connectivity is known at emission time,
	 * and is retained into w-> regardless of whether the mesh ends up empty. */
	ShapeGrid sg; memset(&sg, 0, sizeof(sg));
	ShapeCtx sc; sc.o = &o; sc.palette = palette; sc.sg = &sg;
	WalkVoxels(S, w->dimz, ncol, idbytes, cb_shape, &sc);
	shapegrid_link(&sg, &o);
	w->shapeIdx = sg.idx; w->shapeShape = sg.shape;
	w->shapeParam = sg.param; w->shapeConnect = sg.connect;
	w->shapeCount = sg.count;

	/* 3. count exposed faces */
	FaceCtx fc;
	fc.o = &o; fc.palette = palette; fc.sg = &sg;
	fc.faceCount = 0; fc.faceIdx = 0; fc.emit = 0;
	WalkVoxels(S, w->dimz, ncol, idbytes, cb_face, &fc);
	w->faces = fc.faceCount;

	if (fc.faceCount == 0) { free(S); return 1; }

	/* 4. allocate and record the display list */
	/* Upper bound: 4 verts * 14 bytes per face, plus a GX_Begin header
	 * (3 bytes) per batch. Over-allocate so the buffer is strictly larger
	 * than the padded list (GX_BeginDispList misbehaves at the exact size). */
	u32 nbatches = (fc.faceCount + BATCH_QUADS - 1) / BATCH_QUADS;
	u32 dlSize = fc.faceCount * (4 * 14) + nbatches * 4 + 256;
	dlSize = (dlSize + 31) & ~31u;
	w->dl = memalign(32, dlSize);
	if (!w->dl) { free(o.occ); free(S); return 0; }

	GX_BeginDispList(w->dl, dlSize);
	fc.faceIdx = 0; fc.emit = 1;
	WalkVoxels(S, w->dimz, ncol, idbytes, cb_face, &fc);
	if (fc.faceIdx % BATCH_QUADS != 0) GX_End();
	w->dlLen = GX_EndDispList();

	/* 5. collision-box wireframe overlay -- see emit_wire_box above. Built
	 * from `sg` (still in scope) regardless of shape count; skipped entirely
	 * when there are no non-cube blocks (the overwhelming majority of maps). */
	if (sg.count > 0) {
		u32 maxBoxes = sg.count * 2;
		u32 dbgSize = maxBoxes * (24 * 14 + 4) + 256;
		dbgSize = (dbgSize + 31) & ~31u;
		w->debugDl = memalign(32, dbgSize);
		if (w->debugDl) {
			GX_BeginDispList(w->debugDl, dbgSize);
			u32 i;
			for (i = 0; i < sg.count; i++) {
				int x, y, z;
				occ_decode(&o, sg.idx[i], &x, &y, &z);
				BlockAABB boxes[2];
				int n = BlockShape_Boxes(sg.shape[i], sg.param[i], sg.connect[i], boxes);
				int b;
				for (b = 0; b < n; b++) emit_wire_box(x, y, z, &boxes[b]);
			}
			w->debugDlLen = GX_EndDispList();
		}
	}

	free(S);
	return 1;
}

int World_BlockSolid(const World *w, int bx, int by, int bz) {
	if (!w->occ) return 0;
	/* Block coord B maps to occupancy grid index B - min (see World_Draw:
	 * a grid cell g renders at world-block coordinate g + min). */
	int gx = bx - w->minx;
	int gy = by - w->miny;
	int gz = bz - w->minz;
	if (gx < 0 || gx >= w->dimx || gy < 0 || gy >= w->dimy ||
	    gz < 0 || gz >= w->dimz)
		return 0;
	u32 i = ((u32)gx * w->dimz + gz) * w->dimy + gy;
	return (w->occ[i >> 3] >> (i & 7)) & 1;
}

int World_BlockBoxes(const World *w, int bx, int by, int bz, BlockAABB out[2]) {
	if (!w->occ) return 0;
	int gx = bx - w->minx, gy = by - w->miny, gz = bz - w->minz;
	if (gx < 0 || gx >= w->dimx || gy < 0 || gy >= w->dimy ||
	    gz < 0 || gz >= w->dimz)
		return 0;
	u32 i = ((u32)gx * w->dimz + gz) * w->dimy + gy;
	if (!((w->occ[i >> 3] >> (i & 7)) & 1)) return 0;

	/* Occupied but absent from the sparse shape table: a plain full cube,
	 * the common case -- same box World_BlockSolid's caller used to assume. */
	int lo = 0, hi = (int)w->shapeCount - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (w->shapeIdx[mid] == i) {
			return BlockShape_Boxes(w->shapeShape[mid], w->shapeParam[mid],
			                         w->shapeConnect[mid], out);
		}
		if (w->shapeIdx[mid] < i) lo = mid + 1; else hi = mid - 1;
	}
	out[0] = (BlockAABB){0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
	return 1;
}

void World_Draw(World *w, Mtx view, int showDebugBoxes) {
	if (!w->dl || w->dlLen == 0) return;

	Mtx model, mv;
	guMtxScale(model, WORLD_BLOCK_SIZE, WORLD_BLOCK_SIZE, WORLD_BLOCK_SIZE);
	guMtxTransApply(model, model,
	                w->minx * WORLD_BLOCK_SIZE,
	                w->miny * WORLD_BLOCK_SIZE,
	                w->minz * WORLD_BLOCK_SIZE);
	guMtxConcat(view, model, mv);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);

	GX_LoadTexObj(&atlasTex, GX_TEXMAP0);
	GX_CallDispList(w->dl, w->dlLen);

	if (showDebugBoxes && w->debugDl && w->debugDlLen) {
		/* Same model/view matrix (already loaded above) applies to the debug
		 * list too -- it uses the same fixed-point convention as dl. Flip to
		 * a flat vertex-color TEV stage (no texture sampling) for the
		 * wireframe pass, then restore World_InitGX's textured setup so the
		 * rest of this frame (and the next one) draws normally. */
		GX_SetNumTexGens(0);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

		GX_CallDispList(w->debugDl, w->debugDlLen);

		GX_SetNumTexGens(1);
		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
		GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	}
}

void World_SpawnCamera(World *w, guVector *pos, float *yaw, float *pitch) {
	/* Spawn at the true spawn point (the scan origin, world 0,0,0). */
	pos->x = w->spawnx * WORLD_BLOCK_SIZE;
	pos->y = w->spawny * WORLD_BLOCK_SIZE;
	pos->z = w->spawnz * WORLD_BLOCK_SIZE;
	*yaw = 0.0f;
	*pitch = 0.0f;
}

void World_Free(World *w) {
	if (w->dl) free(w->dl);
	w->dl = NULL;
	w->dlLen = 0;
	if (w->debugDl) free(w->debugDl);
	w->debugDl = NULL;
	w->debugDlLen = 0;
	if (w->occ) free(w->occ);
	w->occ = NULL;
	if (w->shapeIdx) free(w->shapeIdx);
	if (w->shapeShape) free(w->shapeShape);
	if (w->shapeParam) free(w->shapeParam);
	if (w->shapeConnect) free(w->shapeConnect);
	w->shapeIdx = NULL; w->shapeShape = NULL; w->shapeParam = NULL; w->shapeConnect = NULL;
	w->shapeCount = 0;
}

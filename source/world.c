#include <stdlib.h>
#include <string.h>
#include <math.h>
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
#include "block_book_gen.h"   /* generated: DESTROY_STAGE_TILE */
#include "block_props_gen.h"  /* generated: g_blockProps[] (material, for liquids) */
#include "blockmap_gen.h"     /* generated: BLOCKMAP_SENTINEL_ID */

/* Every per-id table this file indexes -- g_blockShape[], g_blockParam[],
 * g_topTile[], g_bottomTile[], g_blockOpaque[], g_blockProps[] -- is exactly
 * NUM_BLOCK_IDS long and is read on the mesh/collision hot paths with no
 * bounds test of its own. So an id is validated once, here, at the only two
 * places one can enter the world: the .mworld decoder and World_SetBlock (the
 * network path). Out of range becomes the palette sentinel, which renders as
 * a magenta/black checker -- visible and inert, rather than whatever lies past
 * the end of those arrays. */
static inline u16 clamp_block_id(int id) {
	return (id >= 0 && id < NUM_BLOCK_IDS) ? (u16)id : (u16)BLOCKMAP_SENTINEL_ID;
}

/* Texcoords are GX_U16 with 10 fractional bits, so a stored value V maps to
 * V/1024 of the texture's width/height. `atlasPixel` is an exact texel edge in
 * the atlas (a tile's interior spans px0 .. px0+ATLAS_TILE); we map edge-to-
 * edge, not inset to texel centers, so a full face shows all 16x16 texels
 * (an earlier half-texel inset here made every tile read as 15x15). Sampling
 * exactly on a tile's outer edge is safe because each tile carries an
 * ATLAS_PAD clamped border (tools/build_atlas.py's pad_tile()) wide enough to
 * absorb the mip box-filter's reach, so it never pulls in a neighbour tile.
 * This runs once per face while building a display list, not per frame, so
 * plain floats are fine here. */
static inline u16 uv_raw(float atlasPixel, float atlasDim) {
	return (u16)(atlasPixel * 1024.0f / atlasDim + 0.5f);
}

#define BATCH_QUADS 16000  /* keep GX_Begin vertex counts under 65535 */

/* Padded scratch grid a chunk is meshed from: the chunk's own columns plus a
 * one-block border on each side, so every face-culling / fence-connectivity
 * neighbour lookup is a plain array read instead of a column binary search. */
#define PADW (WORLD_CHUNK_XZ + 2)
#define PAD_AIR 0xFFFF

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
/* Which local axis (0=x,1=y,2=z) drives the tile's U and V on each face, and
 * whether the tile's low edge (U=0 left / V=0 top) sits at that axis's HIGH
 * end. Derived directly from the faceVerts/faceUV pairing above so a partial
 * box maps to the matching crop of its tile instead of the whole tile squashed
 * onto it (see emit_quad). v=0 is the tile top, so every vertical (Y) mapping
 * is flipped; the horizontal ones follow each face's winding. */
static const u8 faceUAxis[6] = { 2, 2, 0, 0, 0, 0 };
static const u8 faceUFlip[6] = { 0, 1, 0, 0, 1, 0 };
static const u8 faceVAxis[6] = { 1, 1, 2, 2, 1, 1 };
static const u8 faceVFlip[6] = { 1, 1, 0, 1, 1, 1 };
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

/* ---- block storage ----------------------------------------------------- */

static inline u32 col_index(const World *w, int gx, int gz) {
	return (u32)gx * w->dimz + (u32)gz;
}
/* Linear voxel index, the key the edit overlay is sorted by. Columns are
 * contiguous in y, which is what lets a column's edits be found with one
 * lower-bound search (see pad_fill). */
static inline u32 vox_index(const World *w, int gx, int gy, int gz) {
	return col_index(w, gx, gz) * w->dimy + (u32)gy;
}

/* Grid coords of a block coordinate; returns 0 if it falls outside the grid. */
static inline int to_grid(const World *w, int bx, int by, int bz,
                          int *gx, int *gy, int *gz) {
	*gx = bx - w->minx; *gy = by - w->miny; *gz = bz - w->minz;
	return *gx >= 0 && *gx < w->dimx && *gy >= 0 && *gy < w->dimy &&
	       *gz >= 0 && *gz < w->dimz;
}

/* First edit whose index is >= key (binary lower bound over the sorted list). */
static u32 edit_lower(const World *w, u32 key) {
	u32 lo = 0, hi = w->editCount;
	while (lo < hi) {
		u32 mid = (lo + hi) >> 1;
		if (w->editIdx[mid] < key) lo = mid + 1; else hi = mid;
	}
	return lo;
}

/* Loaded (pre-edit) block id in a column run, or -1. */
static int base_block(const World *w, int gx, int gy, int gz) {
	u32 c = col_index(w, gx, gz);
	u32 lo = w->colStart[c], hi = w->colStart[c + 1];
	while (lo < hi) {
		u32 mid = (lo + hi) >> 1;
		if (w->voxY[mid] < (u8)gy) lo = mid + 1;
		else if (w->voxY[mid] > (u8)gy) hi = mid;
		else return (int)w->voxId[mid];
	}
	return -1;
}

int World_InBounds(const World *w, int bx, int by, int bz) {
	int gx, gy, gz;
	return to_grid(w, bx, by, bz, &gx, &gy, &gz);
}

int World_GetBlock(const World *w, int bx, int by, int bz) {
	int gx, gy, gz;
	if (!w->colStart || !to_grid(w, bx, by, bz, &gx, &gy, &gz)) return -1;
	if (w->editCount) {
		u32 key = vox_index(w, gx, gy, gz);
		u32 i = edit_lower(w, key);
		if (i < w->editCount && w->editIdx[i] == key) return w->editId[i];
	}
	return base_block(w, gx, gy, gz);
}

/* Insert/replace an edit-overlay entry, keeping editIdx[] sorted. Returns 0 if
 * the entry could not be recorded, so the caller can report the edit as having
 * failed rather than leaving the world and its mesh disagreeing. */
static int edit_put(World *w, u32 key, int id) {
	u32 i = edit_lower(w, key);
	if (i < w->editCount && w->editIdx[i] == key) { w->editId[i] = (s16)id; return 1; }
	if (w->editCount == w->editCap) {
		u32 cap = w->editCap ? w->editCap * 2 : 256;
		u32 *ni = realloc(w->editIdx, cap * sizeof(u32));
		if (!ni) return 0;
		w->editIdx = ni;
		s16 *nd = realloc(w->editId, cap * sizeof(s16));
		if (!nd) return 0;   /* editIdx is simply over-allocated; still valid */
		w->editId = nd;
		w->editCap = cap;
	}
	memmove(&w->editIdx[i + 1], &w->editIdx[i], (w->editCount - i) * sizeof(u32));
	memmove(&w->editId[i + 1],  &w->editId[i],  (w->editCount - i) * sizeof(s16));
	w->editIdx[i] = key;
	w->editId[i] = (s16)id;
	w->editCount++;
	return 1;
}

/* ---- shape / collision ------------------------------------------------- */

/* Neighbour connect mask for FENCE/WALL/PANE (bit0=-X,1=+X,2=-Z,3=+Z): set
 * when that neighbour is occupied and is either a plain full cube or another
 * block of the *same* shape -- vanilla's "connects to solid blocks and
 * same-family posts" rule (BlockFence.canConnectTo), deliberately not
 * species-specific so all fence species connect to each other. */
static u8 connect_mask(const World *w, int bx, int by, int bz, u8 shape) {
	static const int dx[4] = {-1, 1, 0, 0};
	static const int dz[4] = { 0, 0, -1, 1};
	u8 mask = 0;
	int d;
	for (d = 0; d < 4; d++) {
		int id = World_GetBlock(w, bx + dx[d], by, bz + dz[d]);
		if (id < 0) continue;
		u8 s = g_blockShape[id];
		if (s == SHAPE_CUBE || s == shape) mask |= (u8)(1 << d);
	}
	return mask;
}

int World_BlockBoxes(const World *w, int bx, int by, int bz, BlockAABB out[2]) {
	int id = World_GetBlock(w, bx, by, bz);
	if (id < 0) return 0;
	u8 shape = g_blockShape[id];
	if (shape == SHAPE_CUBE) {
		out[0] = (BlockAABB){0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
		return 1;
	}
	u8 connect = 0;
	if (shape == SHAPE_FENCE || shape == SHAPE_WALL || shape == SHAPE_PANE)
		connect = connect_mask(w, bx, by, bz, shape);
	return BlockShape_Boxes(shape, g_blockParam[id], connect, out);
}

int World_BlockBoxesFor(const World *w, int id, int bx, int by, int bz,
                        BlockAABB out[2]) {
	if (id < 0 || id >= NUM_BLOCK_IDS) return 0;
	u8 shape = g_blockShape[id];
	if (shape == SHAPE_CUBE) {
		out[0] = (BlockAABB){0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
		return 1;
	}
	u8 connect = 0;
	if (shape == SHAPE_FENCE || shape == SHAPE_WALL || shape == SHAPE_PANE)
		connect = connect_mask(w, bx, by, bz, shape);
	return BlockShape_Boxes(shape, g_blockParam[id], connect, out);
}

int World_BlockSolid(const World *w, int bx, int by, int bz) {
	int id = World_GetBlock(w, bx, by, bz);
	return id >= 0 && g_blockShape[id] == SHAPE_CUBE;
}

/* ---- indexed-vertex dedup --------------------------------------------------
 * The world mesh draws with POS direct but CLR0/TEX0 *indexed*, so each vertex
 * in a display list is 6 B position + 1 B colour index + 2 B texcoord index
 * (9 B) instead of 6+4+4 (14 B) -- a ~1.55x smaller mesh, which is what lets
 * the largest maps fit the MEM1 heap. Colours are a handful of face shades
 * (linear scan); texcoords are a few thousand distinct (tile x corner) pairs,
 * deduped through the hash below -- far under the 65535 a 16-bit index can
 * address, so POS can stay direct and no per-chunk vertex splitting is needed.
 * Both arrays are append-only and shared by every chunk, so re-meshing one
 * chunk can add entries without invalidating indices already baked into
 * another chunk's list. */
static u8 dedup_clr(World *w, u8 r, u8 g, u8 b, u8 a) {
	u32 i;
	for (i = 0; i < w->clrCount; i++) {
		const u8 *c = &w->clrArr[i * 4];
		if (c[0] == r && c[1] == g && c[2] == b && c[3] == a) return (u8)i;
	}
	if (w->clrCount == w->clrCap) {
		u32 cap = w->clrCap ? w->clrCap * 2 : 16;
		u8 *n = memalign(32, (cap * 4 + 31) & ~31u);
		if (!n) return 0;
		if (w->clrArr) memcpy(n, w->clrArr, w->clrCount * 4);
		free(w->clrArr);
		w->clrArr = n; w->clrCap = cap;
	}
	u8 *c = &w->clrArr[w->clrCount * 4];
	c[0] = r; c[1] = g; c[2] = b; c[3] = a;
	return (u8)w->clrCount++;
}

static void tex_hash_grow(World *w) {
	u32 newmask = w->texMask ? (w->texMask * 2 + 1) : 2047;
	u32 *nk = malloc((newmask + 1) * sizeof(u32));
	u16 *nv = malloc((newmask + 1) * sizeof(u16));
	u32 i;
	if (!nk || !nv) { free(nk); free(nv); return; }
	for (i = 0; i <= newmask; i++) nk[i] = 0xFFFFFFFFu;
	if (w->texKey) {
		for (i = 0; i <= w->texMask; i++) {
			u32 key = w->texKey[i];
			if (key == 0xFFFFFFFFu) continue;
			u32 h = (key * 2654435761u) & newmask;
			while (nk[h] != 0xFFFFFFFFu) h = (h + 1) & newmask;
			nk[h] = key; nv[h] = w->texVal[i];
		}
		free(w->texKey); free(w->texVal);
	}
	w->texKey = nk; w->texVal = nv; w->texMask = newmask;
}

/* Texcoords never hit 0xFFFFFFFF (s,t stay well under 1024 in the 1024px
 * atlas), so that value is a safe empty-slot sentinel. */
static u16 dedup_tex(World *w, u16 s, u16 t) {
	u32 key = ((u32)s << 16) | t;
	if ((w->texCount + 1) * 4 >= (w->texMask + 1) * 3) tex_hash_grow(w);
	u32 h = (key * 2654435761u) & w->texMask;
	while (w->texKey[h] != 0xFFFFFFFFu) {
		if (w->texKey[h] == key) return w->texVal[h];
		h = (h + 1) & w->texMask;
	}
	if (w->texCount == w->texCap) {
		u32 cap = w->texCap ? w->texCap * 2 : 1024;
		u16 *n = memalign(32, (cap * 4 + 31) & ~31u);
		if (!n) return 0;
		if (w->texArr) memcpy(n, w->texArr, w->texCount * 4);
		free(w->texArr);
		w->texArr = n; w->texCap = cap;
	}
	u16 idx = (u16)w->texCount;
	w->texArr[w->texCount * 2] = s; w->texArr[w->texCount * 2 + 1] = t;
	w->texCount++;
	w->texKey[h] = key; w->texVal[h] = idx;
	return idx;
}

/* ---- mesh emission ----------------------------------------------------- */

typedef struct {
	World *w;
	u32 faceCount;   /* total for this chunk (known after the count pass) */
	u32 faceIdx;     /* running index while emitting                      */
	int emit;        /* 0 = count, 1 = emit                               */
} FaceCtx;

/* Shared quad emitter for both the full-cube path and BlockShape_Mesh's
 * custom shapes: `x0,y0,z0`..`x1,y1,z1` are a bounding box in sixteenths of a
 * block, local to voxel (vx,vy,vz); `face` picks which 4 of its 8 corners
 * make up that face (same faceVerts/faceUV/faceShade convention as before).
 * Handles the count-vs-emit split and BATCH_QUADS GX_Begin/End bookkeeping,
 * so callers (mesh_voxel's cube loop, shape_quad_sink) don't need to. */
static void emit_quad(FaceCtx *fc, int vx, int vy, int vz, int face,
                      s16 x0, s16 y0, s16 z0, s16 x1, s16 y1, s16 z1,
                      int tile, u8 whole) {
	int col = tile % ATLAS_COLS;
	int row = tile / ATLAS_COLS;
	int px0 = col * ATLAS_CELL + ATLAS_PAD;   /* tile's top-left interior texel */
	int py0 = row * ATLAS_CELL + ATLAS_PAD;

	/* Texture crop in tile texels (0..ATLAS_TILE), from the box's projection
	 * onto this face: block-model coords are sixteenths of a block and tiles
	 * are ATLAS_TILE(=16) texels, so a box edge in sixteenths is a texel edge
	 * 1:1. A face shorter/narrower than a full block therefore shows only the
	 * matching slice of its tile (slab side = lower half, thin trapdoor edge =
	 * thin strip) rather than the whole tile stretched to fit -- matching
	 * Minecraft's default model UVs. `whole` overrides this to the full tile
	 * for boxes with a purpose-made crop (enchant-table book); full cubes take
	 * the projected path but span 0..16, so it resolves to the whole tile too.
	 * uLo/uHi and vLo/vHi are the texel offsets at the face's faceUV==0 / ==1
	 * corners, honoring each axis's flip. */
	s16 lo[3] = { x0, y0, z0 }, hi[3] = { x1, y1, z1 };
	int ua = faceUAxis[face], va = faceVAxis[face];
	int uLo, uHi, vLo, vHi;
	if (whole) {
		uLo = 0; uHi = ATLAS_TILE; vLo = 0; vHi = ATLAS_TILE;
	} else {
		uLo = faceUFlip[face] ? (ATLAS_TILE - hi[ua]) : lo[ua];
		uHi = faceUFlip[face] ? (ATLAS_TILE - lo[ua]) : hi[ua];
		vLo = faceVFlip[face] ? (ATLAS_TILE - hi[va]) : lo[va];
		vHi = faceVFlip[face] ? (ATLAS_TILE - lo[va]) : hi[va];
	}
	u16 u0 = uv_raw(px0 + uLo, ATLAS_TEX_W), u1 = uv_raw(px0 + uHi, ATLAS_TEX_W);
	u16 v0 = uv_raw(py0 + vLo, ATLAS_TEX_H), v1 = uv_raw(py0 + vHi, ATLAS_TEX_H);
	u8 sh = faceShade[face];
	u16 tu[4], tv[4];
	int v;
	for (v = 0; v < 4; v++) {
		tu[v] = faceUV[face][v][0] ? u1 : u0;
		tv[v] = faceUV[face][v][1] ? v1 : v0;
	}

	if (!fc->emit) {                       /* count pass: build the dedup arrays */
		dedup_clr(fc->w, sh, sh, sh, 255);
		for (v = 0; v < 4; v++) dedup_tex(fc->w, tu[v], tv[v]);
		fc->faceCount++;
		return;
	}

	if (fc->faceIdx % BATCH_QUADS == 0) {
		u32 rem = fc->faceCount - fc->faceIdx;
		u32 n = rem < BATCH_QUADS ? rem : BATCH_QUADS;
		GX_Begin(GX_QUADS, GX_VTXFMT0, n * 4);
	}

	u8 ci = dedup_clr(fc->w, sh, sh, sh, 255);
	s16 bx[2] = {x0, x1}, by[2] = {y0, y1}, bz[2] = {z0, z1};
	for (v = 0; v < 4; v++) {
		GX_Position3s16((s16)(vx * 16 + bx[faceVerts[face][v][0]]),
		                (s16)(vy * 16 + by[faceVerts[face][v][1]]),
		                (s16)(vz * 16 + bz[faceVerts[face][v][2]]));
		GX_Color1x8(ci);
		GX_TexCoord1x16(dedup_tex(fc->w, tu[v], tv[v]));
	}

	fc->faceIdx++;
	if (fc->faceIdx % BATCH_QUADS == 0) GX_End();
}

/* Emit a single free quad from 4 explicit corners (sixteenths, local to voxel
 * vx,vy,vz), mapping the whole tile across it -- the counterpart to emit_quad
 * for shapes whose planes aren't axis-aligned box faces (SHAPE_CROSS). Shares
 * emit_quad's count-vs-emit split and BATCH_QUADS bookkeeping (via the same fc
 * counters), so quads from both paths interleave correctly in one display list.
 * Corners map to tile UV (u,v) = (0,1),(1,1),(1,0),(0,0), i.e. bottom-left,
 * bottom-right, top-right, top-left, edge-to-edge like the cube path (the
 * ATLAS_PAD border absorbs mip bleed). Plant crosses are unshaded in vanilla,
 * so a fixed full-bright shade is used. */
static void emit_free_quad(FaceCtx *fc, int vx, int vy, int vz,
                           const s16 c[4][3], int tile) {
	int col = tile % ATLAS_COLS, row = tile / ATLAS_COLS;
	int px0 = col * ATLAS_CELL + ATLAS_PAD, py0 = row * ATLAS_CELL + ATLAS_PAD;
	u16 u0 = uv_raw(px0, ATLAS_TEX_W), u1 = uv_raw(px0 + ATLAS_TILE, ATLAS_TEX_W);
	u16 v0 = uv_raw(py0, ATLAS_TEX_H), v1 = uv_raw(py0 + ATLAS_TILE, ATLAS_TEX_H);
	static const u8 cornerU[4] = {0, 1, 1, 0};  /* 0 -> u0, 1 -> u1 */
	static const u8 cornerV[4] = {1, 1, 0, 0};  /* 0 -> v0, 1 -> v1 */
	u16 tu[4], tv[4];
	int v;
	for (v = 0; v < 4; v++) { tu[v] = cornerU[v] ? u1 : u0; tv[v] = cornerV[v] ? v1 : v0; }

	if (!fc->emit) {                       /* count pass: build the dedup arrays */
		dedup_clr(fc->w, 255, 255, 255, 255);
		for (v = 0; v < 4; v++) dedup_tex(fc->w, tu[v], tv[v]);
		fc->faceCount++;
		return;
	}

	if (fc->faceIdx % BATCH_QUADS == 0) {
		u32 rem = fc->faceCount - fc->faceIdx;
		u32 n = rem < BATCH_QUADS ? rem : BATCH_QUADS;
		GX_Begin(GX_QUADS, GX_VTXFMT0, n * 4);
	}

	u8 ci = dedup_clr(fc->w, 255, 255, 255, 255);
	for (v = 0; v < 4; v++) {
		GX_Position3s16((s16)(vx * 16 + c[v][0]),
		                (s16)(vy * 16 + c[v][1]),
		                (s16)(vz * 16 + c[v][2]));
		GX_Color1x8(ci);
		GX_TexCoord1x16(dedup_tex(fc->w, tu[v], tv[v]));
	}

	fc->faceIdx++;
	if (fc->faceIdx % BATCH_QUADS == 0) GX_End();
}

typedef struct { FaceCtx *fc; int x, y, z; } ShapeSink;

static void shape_quad_sink(void *ctx, int face, s16 x0, s16 y0, s16 z0,
                            s16 x1, s16 y1, s16 z1, int tile, u8 whole) {
	ShapeSink *sk = (ShapeSink *)ctx;
	emit_quad(sk->fc, sk->x, sk->y, sk->z, face, x0, y0, z0, x1, y1, z1, tile, whole);
}

static void shape_free_quad_sink(void *ctx,
                                 s16 x0, s16 y0, s16 z0, s16 x1, s16 y1, s16 z1,
                                 s16 x2, s16 y2, s16 z2, s16 x3, s16 y3, s16 z3,
                                 int tile) {
	ShapeSink *sk = (ShapeSink *)ctx;
	const s16 c[4][3] = { {x0,y0,z0}, {x1,y1,z1}, {x2,y2,z2}, {x3,y3,z3} };
	emit_free_quad(sk->fc, sk->x, sk->y, sk->z, c, tile);
}

/* ---- padded chunk scratch grid ----------------------------------------- */

typedef struct {
	u16 *ids;        /* [PADW * PADW * dimy], PAD_AIR where empty        */
	u8  *loY, *hiY;  /* [PADW * PADW] occupied y range per column, hi < lo
	                  * for an empty one -- lets the mesh pass skip the
	                  * (usually vast) empty part of a full-height column */
	int  ox, oz;     /* grid coords of local (0,0) = chunk origin - 1     */
	int  dimy;
} PadGrid;

static inline u16 pad_get(const PadGrid *p, int lx, int ly, int lz) {
	if (lx < 0 || lx >= PADW || lz < 0 || lz >= PADW ||
	    ly < 0 || ly >= p->dimy) return PAD_AIR;
	return p->ids[((u32)lx * PADW + lz) * p->dimy + ly];
}

/* Fill the scratch grid from the column runs plus the edit overlay. */
static void pad_fill(PadGrid *p, const World *w) {
	memset(p->ids, 0xFF, (u32)PADW * PADW * p->dimy * sizeof(u16));
	int lx, lz;
	for (lx = 0; lx < PADW; lx++) {
		int gx = p->ox + lx;
		for (lz = 0; lz < PADW; lz++) {
			u32 lc = (u32)lx * PADW + lz;
			p->loY[lc] = 1; p->hiY[lc] = 0;      /* empty range */
			int gz = p->oz + lz;
			if (gx < 0 || gx >= w->dimx || gz < 0 || gz >= w->dimz) continue;
			u16 *dst = &p->ids[lc * p->dimy];
			u32 c = col_index(w, gx, gz);
			u32 i;
			int lo = p->dimy, hi = -1;
			for (i = w->colStart[c]; i < w->colStart[c + 1]; i++) {
				dst[w->voxY[i]] = w->voxId[i];
				if (w->voxY[i] < lo) lo = w->voxY[i];
				if (w->voxY[i] > hi) hi = w->voxY[i];
			}
			/* overlay this column's runtime edits (contiguous key range) */
			if (w->editCount) {
				u32 base = c * (u32)w->dimy;
				u32 e = edit_lower(w, base);
				for (; e < w->editCount && w->editIdx[e] < base + (u32)w->dimy; e++) {
					int ey = (int)(w->editIdx[e] - base);
					if (w->editId[e] < 0) { dst[ey] = PAD_AIR; continue; }
					dst[ey] = (u16)w->editId[e];
					if (ey < lo) lo = ey;
					if (ey > hi) hi = ey;
				}
			}
			if (hi >= lo) { p->loY[lc] = (u8)lo; p->hiY[lc] = (u8)hi; }
		}
	}
}

/* Connect mask from the scratch grid (same rule as connect_mask()). */
static u8 pad_connect(const PadGrid *p, int lx, int ly, int lz, u8 shape) {
	static const int dx[4] = {-1, 1, 0, 0};
	static const int dz[4] = { 0, 0, -1, 1};
	u8 mask = 0;
	int d;
	for (d = 0; d < 4; d++) {
		u16 id = pad_get(p, lx + dx[d], ly, lz + dz[d]);
		if (id == PAD_AIR) continue;
		u8 s = g_blockShape[id];
		if (s == SHAPE_CUBE || s == shape) mask |= (u8)(1 << d);
	}
	return mask;
}

/* One voxel's geometry. `lx,ly,lz` are scratch-grid local; `vx,vy,vz` are the
 * grid coords the vertices are emitted at. */
static void mesh_voxel(FaceCtx *fc, const PadGrid *p, int lx, int ly, int lz,
                       int vx, int vy, int vz, int g) {
	u8 shape = g_blockShape[g];

	if (shape != SHAPE_CUBE) {
		u8 connect = 0;
		if (shape == SHAPE_FENCE || shape == SHAPE_WALL || shape == SHAPE_PANE)
			connect = pad_connect(p, lx, ly, lz, shape);
		ShapeSink sk = { fc, vx, vy, vz };
		/* No neighbor face-culling for custom shapes -- unlike full cubes,
		 * their faces don't line up with the voxel boundary, so "is the
		 * neighbor solid" doesn't reliably mean "is this face hidden"
		 * (e.g. a bottom slab's top face is exposed regardless of what's in
		 * the voxel above it). Always emitting the shape's own faces trades
		 * a little overdraw on a minority of blocks for never leaving a
		 * gap; GX_CULL_NONE means there's no correctness downside. */
		BlockShape_Mesh(shape, g_blockParam[g], connect, g, shape_quad_sink,
		                shape_free_quad_sink, &sk);
		return;
	}

	int f;
	for (f = 0; f < 6; f++) {
		/* Cull only against an opaque full-cube neighbour -- a non-cube one may
		 * not cover this face, and a see-through cube (glass/leaves) would show
		 * the culled face through its gaps, either way leaving a hole you look
		 * through into the void. */
		u16 nb = pad_get(p, lx + faceNormal[f][0], ly + faceNormal[f][1],
		                 lz + faceNormal[f][2]);
		if (nb != PAD_AIR && g_blockOpaque[nb]) continue;
		/* Face order: 2:-Y(bottom) 3:+Y(top), others use the side tile. */
		int tile = (f == 3) ? g_topTile[g] : (f == 2) ? g_bottomTile[g] : g;
		emit_quad(fc, vx, vy, vz, f, 0, 0, 0, 16, 16, 16, tile, 0);
	}
}

/* Count or emit every voxel of one chunk out of the (already filled) scratch
 * grid. Local x/z 1..WORLD_CHUNK_XZ are the chunk's own columns; 0 and
 * PADW-1 are the borrowed border used only for culling/connectivity. */
static void mesh_chunk_pass(FaceCtx *fc, const PadGrid *p) {
	int lx, lz, ly;
	for (lx = 1; lx <= WORLD_CHUNK_XZ; lx++) {
		for (lz = 1; lz <= WORLD_CHUNK_XZ; lz++) {
			u32 lc = (u32)lx * PADW + lz;
			const u16 *col = &p->ids[lc * p->dimy];
			for (ly = p->loY[lc]; ly <= p->hiY[lc]; ly++) {
				if (col[ly] == PAD_AIR) continue;
				mesh_voxel(fc, p, lx, ly, lz,
				           p->ox + lx, ly, p->oz + lz, col[ly]);
			}
		}
	}
}

/* ---- vertex descriptors ------------------------------------------------ */

/* The world mesh's vertex descriptor: position inline, colour and texcoord as
 * indices into the World's small deduped CLR0/TEX0 arrays (see dedup_clr/
 * dedup_tex). The targeted-block outline and crack overlay instead stream all
 * three attributes inline, so they need the all-direct descriptor below. */
static void mesh_vtxdesc(void) {
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX8);
	GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX16);
}
static void wire_vtxdesc(void) {
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
}

/* ---- chunk (re)meshing ------------------------------------------------- */

/* Bytes of scratch a (re)mesh of this world needs: the padded id grid plus its
 * per-column occupied-y range. Allocated once into World.meshPad. */
static u32 pad_bytes(const World *w) {
	return (u32)PADW * PADW * w->dimy * sizeof(u16) + 2 * PADW * PADW;
}

/* Point a PadGrid at the world's retained scratch buffer. */
static void pad_bind(const World *w, PadGrid *p) {
	p->ids  = w->meshPad;
	p->loY  = (u8 *)(w->meshPad + (u32)PADW * PADW * w->dimy);
	p->hiY  = p->loY + PADW * PADW;
	p->dimy = w->dimy;
}

/* Record chunk (cx,cz)'s display list from the current block data, replacing
 * whatever was there. */
static void remesh_chunk(World *w, int cx, int cz, PadGrid *p) {
	u32 ci = (u32)cx * w->czCount + (u32)cz;
	p->ox = cx * WORLD_CHUNK_XZ - 1;
	p->oz = cz * WORLD_CHUNK_XZ - 1;
	pad_fill(p, w);

	FaceCtx fc;
	fc.w = w; fc.faceCount = 0; fc.faceIdx = 0; fc.emit = 0;
	mesh_chunk_pass(&fc, p);

	if (fc.faceCount == 0) {
		free(w->chunkDl[ci]);
		w->chunkDl[ci] = NULL;
		w->chunkDlLen[ci] = 0;
		w->chunkDlCap[ci] = 0;
		w->faces -= w->chunkFaces[ci];
		w->chunkFaces[ci] = 0;
		return;
	}

	/* Upper bound: 4 verts * 9 bytes per face (POS s16x3 inline + 1-byte CLR0
	 * index + 2-byte TEX0 index), plus a GX_Begin header per batch and slack
	 * for the vertex-descriptor registers libogc flushes into the first list
	 * it records. Over-allocate so the buffer is strictly larger than the
	 * padded list (GX_BeginDispList misbehaves at the exact size). */
	u32 nbatches = (fc.faceCount + BATCH_QUADS - 1) / BATCH_QUADS;
	u32 need = fc.faceCount * (4 * 9) + nbatches * 4 + 512;
	need = (need + 31) & ~31u;
	if (need > w->chunkDlCap[ci]) {
		/* Allocate before releasing the old list: if the heap can't take the
		 * bigger one, keep drawing the previous (stale but complete) geometry
		 * rather than blanking a whole 16x16 column of the map. */
		void *nb = memalign(32, need);
		if (!nb) return;
		free(w->chunkDl[ci]);
		w->chunkDl[ci] = nb;
		w->chunkDlCap[ci] = need;
	}

	w->faces -= w->chunkFaces[ci];
	w->chunkFaces[ci] = fc.faceCount;
	w->faces += fc.faceCount;

	mesh_vtxdesc();   /* the display list is recorded against this descriptor */
	GX_BeginDispList(w->chunkDl[ci], w->chunkDlCap[ci]);
	fc.faceIdx = 0; fc.emit = 1;
	mesh_chunk_pass(&fc, p);
	if (fc.faceIdx % BATCH_QUADS != 0) GX_End();
	w->chunkDlLen[ci] = GX_EndDispList();
}

/* The GX arrays the chunk lists index into must be visible to the GP. */
static void flush_vertex_arrays(const World *w) {
	if (w->clrArr) DCFlushRange(w->clrArr, (w->clrCount * 4 + 31) & ~31u);
	if (w->texArr) DCFlushRange(w->texArr, (w->texCount * 4 + 31) & ~31u);
}

int World_SetBlock(World *w, int bx, int by, int bz, int id) {
	int gx, gy, gz;
	if (!w->colStart || !w->meshPad) return 0;
	if (!to_grid(w, bx, by, bz, &gx, &gy, &gz)) return 0;
	if (id >= 0) id = clamp_block_id(id);   /* -1 stays air */
	if (World_GetBlock(w, bx, by, bz) == id) return 0;

	if (!edit_put(w, vox_index(w, gx, gy, gz), id)) return 0;

	/* Re-mesh the chunk the block lives in, plus the horizontal neighbour
	 * across a chunk seam (its culled faces / fence connections may change).
	 * Chunks span the full Y range, so a vertical neighbour is never in a
	 * different chunk. */
	int cx = gx / WORLD_CHUNK_XZ, cz = gz / WORLD_CHUNK_XZ;
	int lx = gx % WORLD_CHUNK_XZ, lz = gz % WORLD_CHUNK_XZ;
	int cs[3][2];
	int n = 0;
	cs[n][0] = cx; cs[n][1] = cz; n++;
	if (lx == 0 && cx > 0)                        { cs[n][0] = cx - 1; cs[n][1] = cz; n++; }
	else if (lx == WORLD_CHUNK_XZ - 1 && cx + 1 < w->cxCount) { cs[n][0] = cx + 1; cs[n][1] = cz; n++; }
	if (lz == 0 && cz > 0)                        { cs[n][0] = cx; cs[n][1] = cz - 1; n++; }
	else if (lz == WORLD_CHUNK_XZ - 1 && cz + 1 < w->czCount) { cs[n][0] = cx; cs[n][1] = cz + 1; n++; }

	PadGrid p;
	pad_bind(w, &p);
	int i;
	for (i = 0; i < n; i++) remesh_chunk(w, cs[i][0], cs[i][1], &p);
	flush_vertex_arrays(w);
	return 1;
}

/* ---- wireframe box (the targeted-block outline) ---------------------------
 * Reuses GX_VTXFMT0 (same POS+CLR0+TEX0 layout as the real mesh) so no
 * separate vertex format is needed -- the caller disables texturing via a TEV
 * state flip instead, so the filler texcoord below is simply unused while that
 * flat-color stage is active. */
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
			GX_Color4u8(0, 0, 0, 255);
			GX_TexCoord2u16(0, 0);
		}
	}
	GX_End();
}

/* ---- public API ------------------------------------------------------- */
void World_SetupRenderState(void) {
	mesh_vtxdesc();

	/* 4 fractional bits: positions are s16 in sixteenths of a block, matching
	 * Minecraft's own 16-unit block-model grid so vanilla model coordinates
	 * (slab half-heights, stair corners, fence-post insets, ...) map to
	 * integers directly. */
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
}

void World_InitGX(void) {
	World_SetupRenderState();

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

void World_BindAtlas(void) {
	GX_LoadTexObj(&atlasTex, GX_TEXMAP0);
}

/* ---- loading ----------------------------------------------------------- */

typedef struct {
	World *w;
	const u8 *palette;
	u32 *count;         /* pass 1: per-column block count                   */
	u32  lastCol, k;    /* pass 2: current column + index within it         */
} LoadCtx;

static void cb_count(void *ctx, int x, int y, int z, int li) {
	LoadCtx *lc = (LoadCtx *)ctx;
	(void)y; (void)li;
	lc->count[col_index(lc->w, x + WORLD_MARGIN_XZ, z + WORLD_MARGIN_XZ)]++;
}

static void cb_store(void *ctx, int x, int y, int z, int li) {
	LoadCtx *lc = (LoadCtx *)ctx;
	World *w = lc->w;
	u32 c = col_index(w, x + WORLD_MARGIN_XZ, z + WORLD_MARGIN_XZ);
	/* WalkVoxels visits each column exactly once, contiguously, with y
	 * ascending -- so a running index within the current column is enough to
	 * land every voxel at its sorted slot without a second cursor array. */
	if (c != lc->lastCol) { lc->lastCol = c; lc->k = 0; }
	u32 i = w->colStart[c] + lc->k++;
	w->voxY[i] = (u8)(y + WORLD_MARGIN_Y);
	w->voxId[i] = clamp_block_id((int)rd_u16(lc->palette + li * 2));
}

int World_Load(World *w, const u8 *blob, u32 blobLen) {
	(void)blobLen;
	memset(w, 0, sizeof(*w));

	if (blob[0] != 'M' || blob[1] != 'W' || blob[2] != 'L' || blob[3] != '1')
		return 0;

	int idbytes = blob[5];
	u16 palcount = rd_u16(blob + 6);
	s16 sminx = rd_s16(blob + 8);
	s16 sminy = rd_s16(blob + 10);
	s16 sminz = rd_s16(blob + 12);
	u16 sdimx = rd_u16(blob + 14);
	u16 sdimy = rd_u16(blob + 16);
	u16 sdimz = rd_u16(blob + 18);
	w->spawnx = rd_s16(blob + 20);
	w->spawny = rd_s16(blob + 22);
	w->spawnz = rd_s16(blob + 24);
	w->blocks = rd_u32(blob + 26);
	u32 ncol = rd_u32(blob + 30);
	u32 rawS = rd_u32(blob + 34);

	/* Inflate the grid by the build margin so blocks can be placed past the
	 * scanned bounds. voxY is a u8, so the inflated height has to stay under
	 * 256 -- every map is far below that, but refuse rather than corrupt. */
	w->minx = (s16)(sminx - WORLD_MARGIN_XZ);
	w->miny = (s16)(sminy - WORLD_MARGIN_Y);
	w->minz = (s16)(sminz - WORLD_MARGIN_XZ);
	w->dimx = (u16)(sdimx + 2 * WORLD_MARGIN_XZ);
	w->dimy = (u16)(sdimy + 2 * WORLD_MARGIN_Y);
	w->dimz = (u16)(sdimz + 2 * WORLD_MARGIN_XZ);
	if (w->dimy > 255) return 0;

	const u8 *palette = blob + 38;
	const u8 *comp = palette + (u32)palcount * 2;

	/* 1. decompress the structural stream */
	u8 *S = malloc(rawS);
	if (!S) return 0;
	LZ_Decompress(comp, S, rawS);

	/* 2. per-column block counts -> prefix sums (colStart) */
	u32 ncols = (u32)w->dimx * w->dimz;
	w->colStart = calloc(ncols + 1, sizeof(u32));
	if (!w->colStart) { free(S); return 0; }

	LoadCtx lc;
	lc.w = w; lc.palette = palette; lc.count = w->colStart + 1;
	lc.lastCol = 0xFFFFFFFFu; lc.k = 0;
	WalkVoxels(S, sdimz, ncol, idbytes, cb_count, &lc);

	u32 c;
	for (c = 0; c < ncols; c++) w->colStart[c + 1] += w->colStart[c];
	u32 total = w->colStart[ncols];
	w->blocks = total;

	/* 3. the column runs themselves */
	if (total) {
		w->voxY  = malloc(total);
		w->voxId = malloc(total * sizeof(u16));
		if (!w->voxY || !w->voxId) { free(S); World_Free(w); return 0; }
		WalkVoxels(S, sdimz, ncol, idbytes, cb_store, &lc);
	}
	free(S);

	/* 4. chunk grid + per-chunk display lists */
	w->cxCount = (u16)((w->dimx + WORLD_CHUNK_XZ - 1) / WORLD_CHUNK_XZ);
	w->czCount = (u16)((w->dimz + WORLD_CHUNK_XZ - 1) / WORLD_CHUNK_XZ);
	u32 nchunks = (u32)w->cxCount * w->czCount;
	w->chunkDl     = calloc(nchunks, sizeof(void *));
	w->chunkDlLen  = calloc(nchunks, sizeof(u32));
	w->chunkDlCap  = calloc(nchunks, sizeof(u32));
	w->chunkFaces  = calloc(nchunks, sizeof(u32));
	if (!w->chunkDl || !w->chunkDlLen || !w->chunkDlCap || !w->chunkFaces) {
		World_Free(w); return 0;
	}

	/* The mesh scratch is retained for the world's lifetime so that editing a
	 * block never has to allocate (see World.meshPad). */
	w->meshPad = malloc(pad_bytes(w));
	if (!w->meshPad) { World_Free(w); return 0; }

	PadGrid p;
	pad_bind(w, &p);
	int cx, cz;
	for (cx = 0; cx < w->cxCount; cx++)
		for (cz = 0; cz < w->czCount; cz++)
			remesh_chunk(w, cx, cz, &p);
	flush_vertex_arrays(w);
	return 1;
}

/* ---- drawing ----------------------------------------------------------- */

/* Model-view for the block grid: the mesh is in sixteenths of a block with
 * the grid origin at (0,0,0), so scale up and translate to w->min*. */
static void world_mv(const World *w, Mtx view, Mtx mv) {
	Mtx model;
	guMtxScale(model, WORLD_BLOCK_SIZE, WORLD_BLOCK_SIZE, WORLD_BLOCK_SIZE);
	guMtxTransApply(model, model,
	                w->minx * WORLD_BLOCK_SIZE,
	                w->miny * WORLD_BLOCK_SIZE,
	                w->minz * WORLD_BLOCK_SIZE);
	guMtxConcat(view, model, mv);
}

void World_Draw(World *w, Mtx view) {
	if (!w->chunkDl) return;

	Mtx mv;
	world_mv(w, view, mv);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);

	GX_LoadTexObj(&atlasTex, GX_TEXMAP0);
	/* The mesh draws indexed: point CLR0/TEX0 at this world's deduped arrays
	 * and select the indexed descriptor (the HUD pass and the block-outline
	 * pass both leave a different descriptor loaded, so re-assert it every
	 * frame). */
	mesh_vtxdesc();
	GX_SetArray(GX_VA_CLR0, w->clrArr, 4);
	GX_SetArray(GX_VA_TEX0, w->texArr, 4);

	u32 nchunks = (u32)w->cxCount * w->czCount, i;
	for (i = 0; i < nchunks; i++)
		if (w->chunkDl[i] && w->chunkDlLen[i])
			GX_CallDispList(w->chunkDl[i], w->chunkDlLen[i]);
}

void World_DrawBlockOutline(World *w, Mtx view, int bx, int by, int bz) {
	BlockAABB boxes[2];
	int n = World_BlockBoxes(w, bx, by, bz, boxes);
	if (n == 0) {
		/* passable block (flower, torch, ...): outline its selection shape */
		int id = World_GetBlock(w, bx, by, bz);
		if (id < 0) return;
		n = BlockShape_SelectBoxes(g_blockShape[id], g_blockParam[id], 0, boxes);
		if (n == 0) return;
	}

	Mtx mv;
	world_mv(w, view, mv);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);

	/* Flat vertex colour, no texture -- and drawn with the depth test relaxed
	 * a touch (GX_LEQUAL against geometry it exactly coincides with) so the
	 * outline isn't z-fought away by the block's own faces. */
	GX_SetNumTexGens(0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_FALSE);
	wire_vtxdesc();

	int gx = bx - w->minx, gy = by - w->miny, gz = bz - w->minz;
	int b;
	for (b = 0; b < n; b++) {
		/* Grown a hair past the block so the lines sit just outside its
		 * surface (vanilla expands the selection box by 0.002). */
		BlockAABB e = boxes[b];
		e.x0 -= 0.002f; e.y0 -= 0.002f; e.z0 -= 0.002f;
		e.x1 += 0.002f; e.y1 += 0.002f; e.z1 += 0.002f;
		emit_wire_box(gx, gy, gz, &e);
	}

	World_SetupRenderState();
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
}

void World_DrawBreakOverlay(World *w, Mtx view, int bx, int by, int bz, int stage) {
	if (stage < 0) return;
	if (stage >= DESTROY_STAGE_COUNT) stage = DESTROY_STAGE_COUNT - 1;
	int id = World_GetBlock(w, bx, by, bz);
	if (id < 0) return;

	BlockAABB boxes[2];
	int n = World_BlockBoxes(w, bx, by, bz, boxes);
	if (n == 0) n = BlockShape_SelectBoxes(g_blockShape[id], g_blockParam[id], 0, boxes);
	if (n == 0) return;

	Mtx mv;
	world_mv(w, view, mv);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);

	int tile = DESTROY_STAGE_TILE + stage;
	int col = tile % ATLAS_COLS, row = tile / ATLAS_COLS;
	float px0 = (float)(col * ATLAS_CELL + ATLAS_PAD);
	float py0 = (float)(row * ATLAS_CELL + ATLAS_PAD);
	u16 u0 = uv_raw(px0, ATLAS_TEX_W), u1 = uv_raw(px0 + ATLAS_TILE, ATLAS_TEX_W);
	u16 v0 = uv_raw(py0, ATLAS_TEX_H), v1 = uv_raw(py0 + ATLAS_TILE, ATLAS_TEX_H);

	/* Alpha-blended crack texture, depth-tested equal-ish against the block's
	 * own faces and offset outward slightly so it never z-fights them away. */
	World_BindAtlas();
	wire_vtxdesc();
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);

	int gx = bx - w->minx, gy = by - w->miny, gz = bz - w->minz;
	int b, f, v;
	for (b = 0; b < n; b++) {
		const BlockAABB *box = &boxes[b];
		/* Exactly coincident with the block's own faces: positions quantize to
		 * sixteenths of a block, so there is no room to nudge the decal outward
		 * -- instead it goes through the same matrix and vertex format as the
		 * mesh, producing identical depth values that GX_LEQUAL accepts. */
		float lo[3] = { box->x0, box->y0, box->z0 };
		float hi[3] = { box->x1, box->y1, box->z1 };
		for (f = 0; f < 6; f++) {
			GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
			for (v = 0; v < 4; v++) {
				const s16 *sel = faceVerts[f][v];
				float fx = sel[0] ? hi[0] : lo[0];
				float fy = sel[1] ? hi[1] : lo[1];
				float fz = sel[2] ? hi[2] : lo[2];
				GX_Position3s16((s16)((gx + fx) * 16.0f),
				                (s16)((gy + fy) * 16.0f),
				                (s16)((gz + fz) * 16.0f));
				GX_Color4u8(255, 255, 255, 255);
				GX_TexCoord2u16(faceUV[f][v][0] ? u1 : u0,
				                faceUV[f][v][1] ? v1 : v0);
			}
			GX_End();
		}
	}

	World_SetupRenderState();
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
}

void World_SpawnCamera(World *w, guVector *pos, float *yaw, float *pitch) {
	/* Spawn at the true spawn point (the scan origin, world 0,0,0). */
	pos->x = w->spawnx * WORLD_BLOCK_SIZE;
	pos->y = w->spawny * WORLD_BLOCK_SIZE;
	pos->z = w->spawnz * WORLD_BLOCK_SIZE;
	*yaw = 0.0f;
	*pitch = 0.0f;
}

/* ---- ray tracing ------------------------------------------------------- */

/* Slab-method ray/AABB intersection. `o` is the ray origin relative to the
 * box's block, `d` the full ray delta. Returns 1 and fills *t (0..1 along the
 * ray) and *face (the entry face index) on a hit. */
static int ray_box(const double o[3], const double d[3], const BlockAABB *b,
                   double *t, int *face) {
	double lo[3] = { b->x0, b->y0, b->z0 };
	double hi[3] = { b->x1, b->y1, b->z1 };
	double tmin = 0.0, tmax = 1.0;
	int enterAxis = -1, enterNeg = 0;
	int a;
	for (a = 0; a < 3; a++) {
		if (fabs(d[a]) < 1e-12) {
			if (o[a] < lo[a] || o[a] > hi[a]) return 0;
			continue;
		}
		double inv = 1.0 / d[a];
		double t0 = (lo[a] - o[a]) * inv;
		double t1 = (hi[a] - o[a]) * inv;
		int neg = 0;
		if (t0 > t1) { double s = t0; t0 = t1; t1 = s; neg = 1; }
		if (t0 > tmin) { tmin = t0; enterAxis = a; enterNeg = neg; }
		if (t1 < tmax) tmax = t1;
		if (tmin > tmax) return 0;
	}
	if (enterAxis < 0) return 0;    /* started inside the box */
	*t = tmin;
	/* face index: axis*2 + (0 for the low side, 1 for the high side) */
	*face = enterAxis * 2 + (enterNeg ? 1 : 0);
	return 1;
}

/* Blocks a non-liquid ray passes straight through: BlockLiquid overrides
 * canCollideCheck to return false unless the trace asked for liquids, which
 * a normal look-at trace never does. Air is already handled by id < 0. */
static inline int ray_ignores(int id) {
	return g_blockProps[id].material == MAT_WATER;
}

/* Test one block; returns 1 on a hit closer than *bestT. */
static int ray_block(const World *w, int bx, int by, int bz,
                     const double from[3], const double d[3],
                     double *bestT, BlockHit *out) {
	int id = World_GetBlock(w, bx, by, bz);
	if (id < 0 || ray_ignores(id)) return 0;

	BlockAABB boxes[2];
	int n = World_BlockBoxes(w, bx, by, bz, boxes);
	if (n == 0)
		n = BlockShape_SelectBoxes(g_blockShape[id], g_blockParam[id], 0, boxes);
	if (n == 0) return 0;

	double o[3] = { from[0] - bx, from[1] - by, from[2] - bz };
	int found = 0, i;
	for (i = 0; i < n; i++) {
		double t; int face;
		if (!ray_box(o, d, &boxes[i], &t, &face)) continue;
		if (t >= *bestT) continue;
		*bestT = t;
		out->hit = 1;
		out->bx = bx; out->by = by; out->bz = bz;
		out->face = face;
		out->hx = from[0] + d[0] * t;
		out->hy = from[1] + d[1] * t;
		out->hz = from[2] + d[2] * t;
		found = 1;
	}
	return found;
}

static inline int ifloord(double v) { return (int)floor(v); }

int World_RayTrace(const World *w, double x0, double y0, double z0,
                   double x1, double y1, double z1, BlockHit *out) {
	memset(out, 0, sizeof(*out));

	double from[3] = { x0, y0, z0 };
	double d[3] = { x1 - x0, y1 - y0, z1 - z0 };
	double cur[3] = { x0, y0, z0 };
	int i = ifloord(x1), j = ifloord(y1), k = ifloord(z1);
	int l = ifloord(x0), i1 = ifloord(y0), j1 = ifloord(z0);

	double bestT = 1.0;
	if (ray_block(w, l, i1, j1, from, d, &bestT, out)) return 1;

	/* Vanilla's grid walk (World.rayTraceBlocks): step to the nearest voxel
	 * boundary along whichever axis is crossed first, and test the block just
	 * entered. Capped at 200 steps exactly like the original. */
	int steps = 200;
	while (steps-- >= 0) {
		if (l == i && i1 == j && j1 == k) return 0;

		int stepX = 1, stepY = 1, stepZ = 1;
		double d0 = 999.0, d1 = 999.0, d2 = 999.0;
		if (i > l)      d0 = (double)l + 1.0;
		else if (i < l) d0 = (double)l;
		else            stepX = 0;
		if (j > i1)      d1 = (double)i1 + 1.0;
		else if (j < i1) d1 = (double)i1;
		else             stepY = 0;
		if (k > j1)      d2 = (double)j1 + 1.0;
		else if (k < j1) d2 = (double)j1;
		else             stepZ = 0;

		double d3 = 999.0, d4 = 999.0, d5 = 999.0;
		double d6 = x1 - cur[0], d7 = y1 - cur[1], d8 = z1 - cur[2];
		if (stepX) d3 = (d0 - cur[0]) / d6;
		if (stepY) d4 = (d1 - cur[1]) / d7;
		if (stepZ) d5 = (d2 - cur[2]) / d8;
		/* vanilla's -0.0 guard: a zero-length step would never advance */
		if (d3 == 0.0) d3 = -1.0E-4;
		if (d4 == 0.0) d4 = -1.0E-4;
		if (d5 == 0.0) d5 = -1.0E-4;

		int face;
		if (d3 < d4 && d3 < d5) {
			face = (i > l) ? 0 : 1;            /* entered through -X or +X */
			cur[0] = d0; cur[1] += d7 * d3; cur[2] += d8 * d3;
		} else if (d4 < d5) {
			face = (j > i1) ? 2 : 3;           /* -Y or +Y */
			cur[0] += d6 * d4; cur[1] = d1; cur[2] += d8 * d4;
		} else {
			face = (k > j1) ? 4 : 5;           /* -Z or +Z */
			cur[0] += d6 * d5; cur[1] += d7 * d5; cur[2] = d2;
		}

		l  = ifloord(cur[0]) - (face == 1 ? 1 : 0);
		i1 = ifloord(cur[1]) - (face == 3 ? 1 : 0);
		j1 = ifloord(cur[2]) - (face == 5 ? 1 : 0);

		if (ray_block(w, l, i1, j1, from, d, &bestT, out)) return 1;
	}
	return 0;
}

void World_Free(World *w) {
	if (w->chunkDl) {
		u32 n = (u32)w->cxCount * w->czCount, i;
		for (i = 0; i < n; i++) free(w->chunkDl[i]);
		free(w->chunkDl);
	}
	free(w->chunkDlLen);
	free(w->chunkDlCap);
	free(w->chunkFaces);
	w->chunkDl = NULL; w->chunkDlLen = NULL;
	w->chunkDlCap = NULL; w->chunkFaces = NULL;

	free(w->clrArr); free(w->texArr);
	free(w->texKey); free(w->texVal);
	w->clrArr = NULL; w->texArr = NULL; w->texKey = NULL; w->texVal = NULL;
	w->clrCount = w->clrCap = w->texCount = w->texCap = w->texMask = 0;

	free(w->meshPad);
	w->meshPad = NULL;

	free(w->colStart); free(w->voxY); free(w->voxId);
	w->colStart = NULL; w->voxY = NULL; w->voxId = NULL;

	free(w->editIdx); free(w->editId);
	w->editIdx = NULL; w->editId = NULL;
	w->editCount = w->editCap = 0;
}

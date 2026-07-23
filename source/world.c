#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <gccore.h>
#include <ogc/tpl.h>

#include "world.h"
#include "lz.h"
#include "atlas_tpl.h"     /* generated: atlas_tpl[], atlas_tpl_size */

/* ---- atlas layout (must match tools/build_atlas.py) -------------------- */
#define ATLAS_COLS  32
#define ATLAS_TILE  16
/* Texcoords are GX_U16 with 10 fractional bits, so a stored value V maps to
 * V/1024. A normalised coordinate pixel/512 is therefore V = 2*pixel. Tiles
 * are inset half a pixel (+/-1) to stop atlas neighbours bleeding. */
#define UV_LO(px)  (u16)(2 * (px) + 1)
#define UV_HI(px)  (u16)(2 * (px) + 2 * ATLAS_TILE - 1)

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

typedef struct {
	Occ *o;
	const u8 *palette;   /* points into blob: big-endian u16 per local id */
	u32 faceCount;       /* total (known before emit)                     */
	u32 faceIdx;         /* running index while emitting                  */
	int emit;            /* 0 = count, 1 = emit                           */
} FaceCtx;

static void cb_face(void *ctx, int x, int y, int z, int li) {
	FaceCtx *fc = (FaceCtx *)ctx;
	int f;
	for (f = 0; f < 6; f++) {
		if (occ_solid(fc->o, x + faceNormal[f][0], y + faceNormal[f][1],
		              z + faceNormal[f][2]))
			continue;

		if (!fc->emit) { fc->faceCount++; continue; }

		if (fc->faceIdx % BATCH_QUADS == 0) {
			u32 rem = fc->faceCount - fc->faceIdx;
			u32 n = rem < BATCH_QUADS ? rem : BATCH_QUADS;
			GX_Begin(GX_QUADS, GX_VTXFMT0, n * 4);
		}

		int g = rd_u16(fc->palette + li * 2);
		int col = g & (ATLAS_COLS - 1);
		int row = g / ATLAS_COLS;
		int px0 = col * ATLAS_TILE;
		int py0 = row * ATLAS_TILE;
		u16 u0 = UV_LO(px0), u1 = UV_HI(px0);
		u16 v0 = UV_LO(py0), v1 = UV_HI(py0);
		u8 sh = faceShade[f];

		int v;
		for (v = 0; v < 4; v++) {
			GX_Position3s16((s16)(x + faceVerts[f][v][0]),
			                (s16)(y + faceVerts[f][v][1]),
			                (s16)(z + faceVerts[f][v][2]));
			GX_Color4u8(sh, sh, sh, 255);
			GX_TexCoord2u16(faceUV[f][v][0] ? u1 : u0,
			                faceUV[f][v][1] ? v1 : v0);
		}

		fc->faceIdx++;
		if (fc->faceIdx % BATCH_QUADS == 0) GX_End();
	}
}

/* ---- public API ------------------------------------------------------- */
void World_InitGX(void) {
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);

	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 0);
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
	GX_InitTexObjFilterMode(&atlasTex, GX_NEAR, GX_NEAR);
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

	/* 3. count exposed faces */
	FaceCtx fc;
	fc.o = &o; fc.palette = palette; fc.faceCount = 0; fc.faceIdx = 0; fc.emit = 0;
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

void World_Draw(World *w, Mtx view) {
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
	if (w->occ) free(w->occ);
	w->occ = NULL;
}

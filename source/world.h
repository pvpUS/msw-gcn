#ifndef MSW_WORLD_H
#define MSW_WORLD_H

#include <gccore.h>

/* World units per block edge. Block grid coordinates are stored as s16 and
 * scaled up by the model matrix at draw time. */
#define WORLD_BLOCK_SIZE 4.0f

/* Blocks of empty grid kept around the scanned bounds so the player can build
 * out past the map's own extent (bridging up/out is the whole point of being
 * able to place blocks). Storage is per *occupied voxel*, not per grid cell
 * (see World.voxY/voxId), so the only cost of a margin is the per-column
 * index -- a few tens of KB even on the largest map. */
#define WORLD_MARGIN_XZ 8
#define WORLD_MARGIN_Y  16

/* The mesh is split into display lists covering WORLD_CHUNK_XZ x WORLD_CHUNK_XZ
 * full-height columns, so placing/breaking a block only has to re-mesh the one
 * chunk it lives in (plus the neighbour across a chunk seam) instead of the
 * whole map. Chunks span the full Y range, so only the 4 horizontal neighbours
 * can ever share a culled face with an edit. */
#define WORLD_CHUNK_XZ 16

/* Block edits held unsorted before being merged into the edit overlay in one
 * pass. See World.pendIdx and edit_put in world.c for why this exists and how
 * the value is chosen; it is a fixed part of the World, not an allocation. */
#define WORLD_EDIT_PEND_MAX 128

typedef struct {
	s16 minx, miny, minz;      /* grid origin in block coords (margin included)*/
	u16 dimx, dimy, dimz;      /* grid dimensions (margin included)            */
	s16 spawnx, spawny, spawnz;/* spawn reference point (scan origin)          */
	u32 blocks;                /* solid block count as loaded                  */
	u32 faces;                 /* exposed faces baked into the display lists   */

	/* ---- block storage -------------------------------------------------
	 * The map is far too sparse for a dense per-voxel id array (the largest
	 * map is 9.2M grid cells but only 393k blocks), so blocks are stored as
	 * one sorted run per vertical column: colStart[c] .. colStart[c+1] index
	 * voxY[]/voxId[], ascending in y. Lookup is a short binary search inside
	 * one column; meshing walks a column's run directly. Runtime edits are
	 * NOT written back here -- see the edit overlay below -- so this stays
	 * exactly the world that was loaded. */
	u32 *colStart;             /* [dimx*dimz + 1] first entry of each column   */
	u8  *voxY;                 /* y within the column (dimy must be <= 255)    */
	u16 *voxId;                /* global block id                              */

	/* ---- runtime edit overlay ------------------------------------------
	 * Every block placed or broken since load, as a sorted (linear voxel
	 * index -> id) list searched ahead of the loaded data; id -1 means the
	 * voxel was broken and now reads as air. Kept separate so a single edit
	 * costs an insert into a small array rather than a reshuffle of the
	 * column runs above. */
	u32 *editIdx;
	s16 *editId;
	u32  editCount, editCap;

	/* Edits staged but not yet merged into the sorted list above. Keeping the
	 * list sorted costs an O(editCount) memmove per insert, which is nothing
	 * for one mined block and ruinous for a network batch of thousands; this
	 * makes the common case an append and pays for the ordering once per
	 * WORLD_EDIT_PEND_MAX entries. Reads consult it ahead of the sorted list.
	 * Always empty outside a batch -- anything that needs the sorted view
	 * (the mesher) drains it first. */
	u32  pendIdx[WORLD_EDIT_PEND_MAX];
	s16  pendId [WORLD_EDIT_PEND_MAX];
	u32  pendCount;

	/* ---- render data ---------------------------------------------------- */
	u16   cxCount, czCount;    /* chunk grid dimensions                        */
	void **chunkDl;            /* [cxCount*czCount] display lists, NULL = empty*/
	u32   *chunkDlLen;
	u32   *chunkDlCap;         /* bytes allocated for each (re-mesh in place)  */
	u32   *chunkFaces;         /* quads in each, so World.faces stays right    */

	/* Chunks whose block data has changed but whose display list has not been
	 * rebuilt yet -- one byte per chunk, plus the count so the flush can bail
	 * in O(1) on the overwhelmingly common "nothing changed" frame. Deferring
	 * is what lets a network block batch (hundreds of edits arriving in one
	 * GCLink message) cost one re-mesh per chunk instead of one per block; see
	 * World_SetBlockDeferred / World_FlushRemesh. Block *reads* are never
	 * deferred -- the edit overlay is updated immediately, so collision and
	 * ray-tracing are correct the instant the edit lands, and only the geometry
	 * lags by a frame or two. */
	u8    *chunkDirty;
	u32    dirtyCount;

	/* Indexed CLR0/TEX0 vertex arrays the display lists reference (POS is
	 * inline). 32-byte aligned + DC-flushed; GX_SetArray'd in World_Draw.
	 * Storing colour/texcoord as 1/2-byte indices instead of 4+4 bytes inline
	 * is what shrinks the mesh enough for the largest maps to fit MEM1. They
	 * are append-only and shared by every chunk, so indices already baked
	 * into a display list stay valid when a re-mesh adds new entries. */
	u8  *clrArr;   u32 clrCount, clrCap;   /* RGBA8 face-shade palette         */
	u16 *texArr;   u32 texCount, texCap;   /* deduped (s,t) u16 pairs          */
	u32 *texKey;   u16 *texVal; u32 texMask;  /* open-addressing texcoord hash */

	/* Scratch grid a chunk (re)mesh works out of, allocated once at load and
	 * kept for the world's lifetime. Editing a block must never have to
	 * allocate: on the densest maps the heap is nearly full after loading, and
	 * a failed scratch malloc would silently skip the re-mesh -- leaving the
	 * broken block still drawn while collision and drops said it was gone. */
	u16 *meshPad;

	/* ---- accounting (World_GetStats) ------------------------------------ */
	u32   chunksDrawn;         /* display lists the last World_Draw submitted */
	float remeshMs, remeshMsMax;        /* per re-mesh *call*                 */
	float remeshChunkMsMax;             /* worst single chunk, the T24 budget */
} World;

/* Local block-relative (0..1) axis-aligned box. */
typedef struct { float x0, y0, z0, x1, y1, z1; } BlockAABB;

/* One-time GX pipeline setup for world rendering (vtx formats, atlas TEV). */
void World_InitGX(void);

/* (Re)apply the world's GX render state -- vertex descriptor/format 0, TEV,
 * texgen, alpha compare, blend and cull modes. World_InitGX calls this once;
 * the HUD pass calls it again on exit to undo its own GX state changes so the
 * next World_Draw sees the pipeline it expects. Does NOT touch the projection
 * matrix or reopen the texture. */
void World_SetupRenderState(void);

/* Bind the block-texture atlas to GX_TEXMAP0 (for drawing block icons in the
 * HUD; World_Draw already does this itself for the world mesh). */
void World_BindAtlas(void);

/* Load and mesh a .mworld blob. Returns 1 on success, 0 on failure.
 * All scratch (decompressed stream, palette) is freed before returning. */
int  World_Load(World *w, const u8 *blob, u32 blobLen);

/* Draw the world. `view` is the camera view matrix. */
void World_Draw(World *w, Mtx view);

/* Suggested starting camera position/target for a freshly loaded world. */
void World_SpawnCamera(World *w, guVector *pos, float *yaw, float *pitch);

/* ---- block access (Minecraft block coordinates, 1 block = 1 unit) -------- */

/* Global block id at (bx,by,bz), or -1 for air / outside the grid. Consults
 * the runtime edit overlay first, then the loaded column runs. */
int  World_GetBlock(const World *w, int bx, int by, int bz);

/* 1 if (bx,by,bz) is inside the (margin-inflated) grid. */
int  World_InBounds(const World *w, int bx, int by, int bz);

/* Set the block at (bx,by,bz) to `id` (-1 = air), recording it in the edit
 * overlay and re-meshing the affected chunk(s). Returns 1 if anything changed.
 * This is the only mutation path -- World.colStart/voxY/voxId stay as loaded.
 *
 * Synchronous: the geometry is correct by the time this returns, which is what
 * single-player mining and placing want (one edit at a time, and the block must
 * visibly go away on the frame the player broke it). */
int  World_SetBlock(World *w, int bx, int by, int bz, int id);

/* ---- deferred, batched re-meshing (T24) ---------------------------------
 * The same edit without the re-mesh: record it and mark the owning chunk (plus
 * the neighbour across a chunk seam) dirty, leaving the display list stale
 * until World_FlushRemesh gets to it. Same return contract as World_SetBlock.
 *
 * This is the path for edits that arrive in bulk -- the proxy's join-time chunk
 * diff is hundreds of blocks in a single GCLink batch, and re-meshing inside
 * each call would be a multi-second freeze re-recording the same few chunks
 * over and over. */
int  World_SetBlockDeferred(World *w, int bx, int by, int bz, int id);

/* Re-mesh at most `maxChunks` dirty chunks, nearest-to-(px,pz) first (block
 * coordinates, normally the player's). Returns the number still dirty, so a
 * caller can tell whether the world has converged.
 *
 * Call once per rendered frame. Nearest-first is what makes a join look right:
 * the chunks under and around the player resolve in the first frames, and the
 * far side of the map catches up over the next few. Costs one early-out when
 * nothing is dirty, which is every frame of offline play.
 *
 * `maxChunks` is a ceiling, not a quota -- the call also stops once it has
 * spent its own wall-clock budget, so a dense map self-limits rather than
 * overrunning the frame. See FLUSH_BUDGET_US in world.c. */
int  World_FlushRemesh(World *w, int maxChunks, double px, double pz);

/* Returns 1 if the block at (bx,by,bz) is a solid full cube, 0 for air/void
 * (outside the loaded region reads as air). */
int  World_BlockSolid(const World *w, int bx, int by, int bz);

/* Local block-relative (0..1) collision box(es) for the block at (bx,by,bz);
 * caller adds (bx,by,bz) for world-absolute bounds. Returns the number of
 * boxes written to out[0..1] (0 = passable/air). Full-cube blocks -- the
 * overwhelming majority -- return exactly 1 box {0,0,0,1,1,1}. */
int  World_BlockBoxes(const World *w, int bx, int by, int bz, BlockAABB out[2]);

/* Same, but for a hypothetical block of global id `id` at (bx,by,bz) -- what
 * World.canBlockBePlaced needs to know before actually placing it (vanilla's
 * checkNoEntityCollision on blockIn.getCollisionBoundingBox). */
int  World_BlockBoxesFor(const World *w, int id, int bx, int by, int bz,
                         BlockAABB out[2]);

/* ---- ray tracing (net.minecraft.world.World.rayTraceBlocks) -------------- */

typedef struct {
	int    hit;              /* 0 = nothing within the ray                    */
	int    bx, by, bz;       /* block that was hit                            */
	int    face;             /* entry face: 0:-X 1:+X 2:-Y 3:+Y 4:-Z 5:+Z     */
	double hx, hy, hz;       /* exact hit point in block units                */
} BlockHit;

/* Walks the voxel grid from (x0,y0,z0) to (x1,y1,z1) exactly like vanilla's
 * World.rayTraceBlocks, testing each visited block against its selection
 * box(es). Returns 1 (and fills `out`) on a hit. Liquids and air are skipped,
 * matching Block.canCollideCheck for a non-liquid trace. */
int  World_RayTrace(const World *w, double x0, double y0, double z0,
                    double x1, double y1, double z1, BlockHit *out);

/* ---- targeted-block overlays (drawn right after World_Draw) -------------- */

/* Vanilla's black selection wireframe around the block the player is looking
 * at (RenderGlobal.drawSelectionBox), sized to the block's own shape. */
void World_DrawBlockOutline(World *w, Mtx view, int bx, int by, int bz);

/* Vanilla's destroy_stage_N crack overlay on the block being mined; `stage` is
 * 0..DESTROY_STAGE_COUNT-1. Blends over the block's own faces. */
void World_DrawBreakOverlay(World *w, Mtx view, int bx, int by, int bz, int stage);

/* ---- accounting (the perf overlay, T27) ---------------------------------
 * Memory is the binding constraint on this target -- 24 MB MEM1, a ~15 MB
 * heap, and the densest map already at ~13 MB -- so every budget in the BBA
 * plan is stated against one of these numbers, and the overlay is how they
 * are read rather than a boot-time printf. */
typedef struct {
	u32 chunks;              /* meshed chunks (cxCount * czCount)            */
	u32 chunksDrawn;         /* display lists submitted by the last draw     */
	u32 faces;               /* quads baked into those lists                 */
	u32 dlBytes;             /* bytes those display lists hold               */
	u32 dlUsed;              /* bytes actually recorded into them            */
	u32 clrCount, texCount;  /* indexed vertex arrays, vs the 65535 ceiling  */
	u32 edits;               /* runtime block edits in the overlay           */
	u32 dirtyChunks;         /* meshes still owed after the last flush       */
	float remeshMs, remeshMsMax;  /* last / worst re-mesh call               */
	float remeshChunkMsMax;  /* worst single chunk (T24 budgets <= 4 ms)     */
} WorldStats;

void World_GetStats(const World *w, WorldStats *out);

/* Reset the rolling maxima (remeshMsMax, remeshChunkMsMax), so a spike can be
 * attributed to what just happened rather than to the whole session. */
void World_ResetStatsMax(World *w);

void World_Free(World *w);

#endif

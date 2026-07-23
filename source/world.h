#ifndef MSW_WORLD_H
#define MSW_WORLD_H

#include <gccore.h>

/* World units per block edge. Block grid coordinates are stored as s16 and
 * scaled up by the model matrix at draw time. */
#define WORLD_BLOCK_SIZE 4.0f

typedef struct {
	s16 minx, miny, minz;      /* bounding-box origin (spawn-relative coords) */
	u16 dimx, dimy, dimz;      /* grid dimensions                            */
	s16 spawnx, spawny, spawnz;/* spawn reference point (scan origin)        */
	u32 blocks;                /* solid block count                          */
	u32 faces;                 /* exposed faces baked into the display list  */

	void *dl;                  /* GX display list (32-byte aligned)          */
	u32   dlLen;               /* display list length in bytes               */

	u8   *occ;                 /* retained occupancy bitset for collision     */

	/* Sparse per-voxel shape/collision data for the (typically small) subset
	 * of occupied voxels that are not plain full cubes -- see block_shapes.h.
	 * Sorted by the same linear index scheme as occ[] (occ_index in world.c),
	 * so it can be located either by walking with occ[] or via binary search
	 * at collision-query time (World_BlockBoxes). Voxels absent from this
	 * list but present in occ[] are full cubes. */
	u32  *shapeIdx;             /* sorted occ-index of each entry              */
	u8   *shapeShape;           /* shape id (see block_shapes.h)               */
	u8   *shapeParam;           /* packed per-shape params (facing/open/etc.)  */
	u8   *shapeConnect;         /* neighbor connect mask, FENCE/WALL/PANE only */
	u32   shapeCount;
} World;

/* One-time GX pipeline setup for world rendering (vtx formats, atlas TEV). */
void World_InitGX(void);

/* Load and mesh a .mworld blob. Returns 1 on success, 0 on failure.
 * All scratch (decompressed stream, occupancy grid, palette) is freed before
 * returning; only the display list is retained. */
int  World_Load(World *w, const u8 *blob, u32 blobLen);

/* Draw the world. `view` is the camera view matrix. */
void World_Draw(World *w, Mtx view);

/* Suggested starting camera position/target for a freshly loaded world. */
void World_SpawnCamera(World *w, guVector *pos, float *yaw, float *pitch);

/* Collision query in Minecraft block coordinates (1 block = 1 unit).
 * Returns 1 if the block at (bx,by,bz) is a solid full cube, 0 for air/void
 * (outside the loaded region reads as air). */
int  World_BlockSolid(const World *w, int bx, int by, int bz);

/* Local block-relative (0..1) axis-aligned collision box(es) for the block at
 * (bx,by,bz); caller adds (bx,by,bz) for world-absolute bounds. Returns the
 * number of boxes written to out[0..1] (0 = passable/air). Full-cube blocks
 * -- the overwhelming majority -- return exactly 1 box {0,0,0,1,1,1}. */
typedef struct { float x0, y0, z0, x1, y1, z1; } BlockAABB;
int  World_BlockBoxes(const World *w, int bx, int by, int bz, BlockAABB out[2]);

void World_Free(World *w);

#endif

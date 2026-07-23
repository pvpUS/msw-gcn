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

void World_Free(World *w);

#endif

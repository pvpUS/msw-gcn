#ifndef MSW_BLOCK_SHAPES_H
#define MSW_BLOCK_SHAPES_H

#include <gccore.h>
#include "world.h"

/* Shape ids for non-full-cube blocks. Numeric values MUST match
 * tools/build_atlas.py's SHAPE_IDS table exactly -- both sides index the
 * generated per-global-id tables in block_shapes_gen.h by these values. */
enum {
	SHAPE_CUBE = 0,   /* default: ordinary full cube, the existing fast path */
	SHAPE_SLAB,
	SHAPE_STAIR,
	SHAPE_FENCE,
	SHAPE_FENCE_GATE,
	SHAPE_WALL,
	SHAPE_PANE,       /* shared by iron bars and glass panes */
	SHAPE_ANVIL,
	SHAPE_ENCHANT_TABLE,
	SHAPE_TRAPDOOR,
	SHAPE_DOOR,
};

/* Local block-relative (0..1) collision boxes for a block of the given shape/
 * param. `connectMask` (bit0=-X,1=+X,2=-Z,3=+Z) is only consulted by
 * FENCE/WALL/PANE; pass 0 for everything else. Writes up to 2 boxes to out[]
 * and returns the count (0 = fully passable). */
int BlockShape_Boxes(u8 shape, u8 param, u8 connectMask, BlockAABB out[2]);

/* Mesh emission for non-cube shapes. world.c owns the atlas/UV/shading
 * tables and the GX_Begin/End batching bookkeeping (it needs those for the
 * full-cube path anyway), so block_shapes.c only produces geometry: for each
 * quad, it calls back with a face index (0:-X 1:+X 2:-Y 3:+Y(top) 4:-Z 5:+Z,
 * same convention as world.c's own faceNormal/faceUV/faceShade), the quad's
 * axis-aligned bounds in sixteenths-of-a-block *local to the voxel*, and an
 * atlas tile index. world.c's callback derives the 4 corners from the bounds
 * exactly like its existing faceVerts table, just parameterized instead of
 * hardcoded to 0/16. */
typedef void (*BlockQuadFn)(void *ctx, int face,
                             s16 x0, s16 y0, s16 z0,
                             s16 x1, s16 y1, s16 z1, int tile);

/* Emits (or, if `emit` is a counting stub, just tallies) the quads for a
 * block of the given shape/param/connectMask at global id `g` (used to
 * resolve texture tiles via g_topTile[]/g_bottomTile[], same as the full-cube
 * path). Returns the number of quads emitted. */
u32 BlockShape_Mesh(u8 shape, u8 param, u8 connectMask, int g,
                     BlockQuadFn emit, void *ctx);

#endif

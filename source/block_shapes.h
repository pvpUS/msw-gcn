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
	SHAPE_CROSS,      /* crossed diagonal planes: flowers, tall grass, saplings, ... */
	SHAPE_TORCH,      /* crossed billboards forming a torch (floor/wall variants)    */
	SHAPE_LADDER,     /* single plane flush against one wall                         */
	SHAPE_VINE,       /* per-set-bit planes on each attached wall                    */
	SHAPE_PLATE,      /* thin floor slab (pressure plate)                            */
	SHAPE_CHEST,      /* inset solid box (chest)                                     */
	SHAPE_SKULL,      /* small head box, floor or wall mounted (steve head)          */
};

/* Local block-relative (0..1) collision boxes for a block of the given shape/
 * param. `connectMask` (bit0=-X,1=+X,2=-Z,3=+Z) is only consulted by
 * FENCE/WALL/PANE; pass 0 for everything else. Writes up to 2 boxes to out[]
 * and returns the count (0 = fully passable). */
int BlockShape_Boxes(u8 shape, u8 param, u8 connectMask, BlockAABB out[2]);

/* Same, but for what the player can *aim at* rather than walk into. Vanilla
 * keeps these separate (getSelectedBoundingBox vs getCollisionBoundingBox):
 * flowers, torches, ladders, vines and pressure plates are all walk-through
 * yet still targetable for breaking, so BlockShape_Boxes returns 0 boxes for
 * them while this returns their (small) visual bounds. Everything with real
 * collision just forwards to BlockShape_Boxes. */
int BlockShape_SelectBoxes(u8 shape, u8 param, u8 connectMask, BlockAABB out[2]);

/* Mesh emission for non-cube shapes. world.c owns the atlas/UV/shading
 * tables and the GX_Begin/End batching bookkeeping (it needs those for the
 * full-cube path anyway), so block_shapes.c only produces geometry: for each
 * quad, it calls back with a face index (0:-X 1:+X 2:-Y 3:+Y(top) 4:-Z 5:+Z,
 * same convention as world.c's own faceNormal/faceUV/faceShade), the quad's
 * axis-aligned bounds in sixteenths-of-a-block *local to the voxel*, and an
 * atlas tile index. world.c's callback derives the 4 corners from the bounds
 * exactly like its existing faceVerts table, just parameterized instead of
 * hardcoded to 0/16.
 *
 * `whole` selects how the tile maps onto the quad. 0 (the normal case) uses
 * the box's projection onto the face -- a partial box shows the matching crop
 * of its 16x16 tile (a bottom slab's side shows the tile's lower half, a thin
 * trapdoor's edge a thin strip), exactly like Minecraft's default model UVs,
 * because every shape here is aligned to the same 16-unit grid its texture is.
 * 1 stretches the whole tile across the quad regardless of size -- for a
 * decorative box carrying a purpose-made crop rather than a grid-aligned block
 * texture (the enchanting table's floating book), where a projected sub-rect
 * would show only a sliver of that crop. */
typedef void (*BlockQuadFn)(void *ctx, int face,
                             s16 x0, s16 y0, s16 z0,
                             s16 x1, s16 y1, s16 z1, int tile, u8 whole);

/* Emits one free (not necessarily axis-aligned) quad given its 4 corner
 * positions in sixteenths-of-a-block local to the voxel. Needed for the
 * diagonal crossed planes of SHAPE_CROSS (flowers/grass), which BlockQuadFn's
 * face+box model can't express. Corners are given as bottom-left, bottom-
 * right, top-right, top-left of the tile, so the whole tile maps across the
 * quad and a corner-to-corner vertical plane renders the plant upright.
 * Culling is disabled globally, so a single quad shows from both sides. */
typedef void (*BlockFreeQuadFn)(void *ctx,
                                s16 x0, s16 y0, s16 z0, s16 x1, s16 y1, s16 z1,
                                s16 x2, s16 y2, s16 z2, s16 x3, s16 y3, s16 z3,
                                int tile);

/* Emits (or, if the callbacks are counting stubs, just tallies) the quads for
 * a block of the given shape/param/connectMask at global id `g` (used to
 * resolve texture tiles via g_topTile[]/g_bottomTile[], same as the full-cube
 * path). Axis-aligned box faces go through `emit`; the diagonal cross planes go
 * through `emitFree`. Returns the number of quads emitted. */
u32 BlockShape_Mesh(u8 shape, u8 param, u8 connectMask, int g,
                     BlockQuadFn emit, BlockFreeQuadFn emitFree, void *ctx);

#endif

#include "block_shapes.h"
#include "block_faces_gen.h"  /* generated: g_topTile[], g_bottomTile[] */
#include "block_book_gen.h"   /* generated: ENCHANT_BOOK_TILE */

/* ---- collision -------------------------------------------------------- */

int BlockShape_Boxes(u8 shape, u8 param, u8 connectMask, BlockAABB out[2]) {
	switch (shape) {
	case SHAPE_SLAB: {
		/* bit3(8) = top half (ground-truthed against BlockStoneSlab.java's
		 * getStateFromMeta: (meta & 8) == 0 ? BOTTOM : TOP). */
		int top = (param & 8) != 0;
		out[0] = top ? (BlockAABB){0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 1.0f}
		             : (BlockAABB){0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f};
		return 1;
	}
	case SHAPE_STAIR: {
		/* bits0-1 = facing (0=EAST,1=WEST,2=SOUTH,3=NORTH), bit2(4) = upside
		 * down. Ground-truthed against BlockStairs.java's getStateFromMeta
		 * and setBaseCollisionBounds/func_176306_h (default, unmerged shape
		 * -- see block_shapes.c's BlockShape_Mesh comment for why we don't
		 * replicate the neighbor-dependent inner/outer corner merging). */
		int facing = param & 3;
		int upsideDown = (param & 4) != 0;
		float baseY0 = upsideDown ? 0.5f : 0.0f, baseY1 = upsideDown ? 1.0f : 0.5f;
		float riserY0 = upsideDown ? 0.0f : 0.5f, riserY1 = upsideDown ? 0.5f : 1.0f;
		out[0] = (BlockAABB){0.0f, baseY0, 0.0f, 1.0f, baseY1, 1.0f};
		switch (facing) {
		case 0: out[1] = (BlockAABB){0.5f, riserY0, 0.0f, 1.0f, riserY1, 1.0f}; break;
		case 1: out[1] = (BlockAABB){0.0f, riserY0, 0.0f, 0.5f, riserY1, 1.0f}; break;
		case 2: out[1] = (BlockAABB){0.0f, riserY0, 0.5f, 1.0f, riserY1, 1.0f}; break;
		default: out[1] = (BlockAABB){0.0f, riserY0, 0.0f, 1.0f, riserY1, 0.5f}; break;
		}
		return 2;
	}
	case SHAPE_FENCE_GATE: {
		/* bits0-1 = facing (EnumFacing.getHorizontal: 0=SOUTH,1=WEST,2=NORTH,
		 * 3=EAST), bit2(4) = open. Ground-truthed against
		 * BlockFenceGate.java's getStateFromMeta/getCollisionBoundingBox.
		 * Open gates aren't rendered swung against the post (no animation
		 * budget for this pass) so they're simply passable -- 0 boxes. */
		int facing = param & 3;
		int open = (param & 4) != 0;
		if (open) return 0;
		int axisZ = (facing % 2) == 0; /* SOUTH/NORTH: gate panel runs along X */
		out[0] = axisZ ? (BlockAABB){0.0f, 0.0f, 0.375f, 1.0f, 1.5f, 0.625f}
		               : (BlockAABB){0.375f, 0.0f, 0.0f, 0.625f, 1.5f, 1.0f};
		return 1;
	}
	case SHAPE_FENCE: {
		/* Exactly matches BlockFence.java's addCollisionBoxesToList: up to 2
		 * boxes, a north-south beam (present if connected N or S) and an
		 * east-west beam (present if connected E/W, or as the lone-post
		 * fallback when nothing connects) -- not an approximation, this *is*
		 * vanilla's real fence collision shape. connectMask: bit0=-X(west),
		 * 1=+X(east), 2=-Z(north), 3=+Z(south) (see shapegrid_link). */
		int connW = connectMask & 1, connE = (connectMask >> 1) & 1;
		int connN = (connectMask >> 2) & 1, connS = (connectMask >> 3) & 1;
		int n = 0;
		if (connN || connS) {
			float z0 = connN ? 0.0f : 0.375f, z1 = connS ? 1.0f : 0.625f;
			out[n++] = (BlockAABB){0.375f, 0.0f, z0, 0.625f, 1.5f, z1};
		}
		if (connW || connE || (!connN && !connS)) {
			float x0 = connW ? 0.0f : 0.375f, x1 = connE ? 1.0f : 0.625f;
			out[n++] = (BlockAABB){x0, 0.0f, 0.375f, x1, 1.5f, 0.625f};
		}
		return n;
	}
	case SHAPE_PANE: {
		/* Exactly matches BlockPane.java's addCollisionBoxesToList: up to 2
		 * boxes, one thin-in-Z beam along X (present unless only N/S connect)
		 * and one thin-in-X beam along Z (present unless only W/E connect);
		 * a fully isolated pane (no connections at all) renders as a full
		 * "+" cross -- both beams present at full length -- exactly like
		 * vanilla, not an approximation. Shared by iron bars and glass
		 * panes (SHAPE_PANE), same as vanilla's BlockPane base class. */
		int connW = connectMask & 1, connE = (connectMask >> 1) & 1;
		int connN = (connectMask >> 2) & 1, connS = (connectMask >> 3) & 1;
		int any = connW || connE || connN || connS;
		int n = 0;
		if ((!connW || !connE) && any) {
			if (connW)      out[n++] = (BlockAABB){0.0f, 0.0f, 0.4375f, 0.5f, 1.0f, 0.5625f};
			else if (connE) out[n++] = (BlockAABB){0.5f, 0.0f, 0.4375f, 1.0f, 1.0f, 0.5625f};
		} else {
			out[n++] = (BlockAABB){0.0f, 0.0f, 0.4375f, 1.0f, 1.0f, 0.5625f};
		}
		if ((!connN || !connS) && any) {
			if (connN)      out[n++] = (BlockAABB){0.4375f, 0.0f, 0.0f, 0.5625f, 1.0f, 0.5f};
			else if (connS) out[n++] = (BlockAABB){0.4375f, 0.0f, 0.5f, 0.5625f, 1.0f, 1.0f};
		} else {
			out[n++] = (BlockAABB){0.4375f, 0.0f, 0.0f, 0.5625f, 1.0f, 1.0f};
		}
		return n;
	}
	case SHAPE_WALL: {
		/* Matches BlockWall.java's setBlockBoundsBasedOnState (also its
		 * collision box -- BlockWall doesn't override
		 * addCollisionBoxesToList), a single box independently extended
		 * toward each connected side -- except we skip its narrowed
		 * "straight run" special case (0.8125 height / narrower cross-
		 * section when connected on exactly one axis) and just use a
		 * uniform 1-block-tall post, a minor, purely cosmetic
		 * simplification. */
		int connW = connectMask & 1, connE = (connectMask >> 1) & 1;
		int connN = (connectMask >> 2) & 1, connS = (connectMask >> 3) & 1;
		float x0 = connW ? 0.0f : 0.25f, x1 = connE ? 1.0f : 0.75f;
		float z0 = connN ? 0.0f : 0.25f, z1 = connS ? 1.0f : 0.75f;
		out[0] = (BlockAABB){x0, 0.0f, z0, x1, 1.0f, z1};
		return 1;
	}
	case SHAPE_ANVIL: {
		/* bits0-1 = facing (EnumFacing.getHorizontal: 0=SOUTH,1=WEST,2=NORTH,
		 * 3=EAST). Ground-truthed against BlockAnvil.java's
		 * setBlockBoundsBasedOnState: thin along Z when facing is on the X
		 * axis (WEST/EAST) and vice versa -- the anvil's long axis runs
		 * along its own facing. Damage (bits2+) only affects the top
		 * texture, not the shape. */
		int facing = param & 3;
		int axisX = (facing % 2) != 0;
		out[0] = axisX ? (BlockAABB){0.0f, 0.0f, 0.125f, 1.0f, 1.0f, 0.875f}
		               : (BlockAABB){0.125f, 0.0f, 0.0f, 0.875f, 1.0f, 1.0f};
		return 1;
	}
	case SHAPE_ENCHANT_TABLE:
		/* BlockEnchantmentTable.java: setBlockBounds(0,0,0, 1,0.75,1). The
		 * floating book (mesh-only, see mesh_enchant_table) has no
		 * collision, matching vanilla (it's a pure TESR visual). */
		out[0] = (BlockAABB){0.0f, 0.0f, 0.0f, 1.0f, 0.75f, 1.0f};
		return 1;
	case SHAPE_TRAPDOOR: {
		/* bits0-1 = facing (0=NORTH,1=SOUTH,2=WEST,3=EAST), bit2(4) = open,
		 * bit3(8) = top half. Ground-truthed against BlockTrapDoor.java's
		 * getStateFromMeta/setBounds: closed = thin horizontal slab (near
		 * the floor or ceiling per HALF); open = thin *vertical* slab
		 * against the edge given by FACING, full height, regardless of
		 * HALF (matches vanilla exactly -- setBounds always applies the
		 * open case after the half-dependent one). */
		int facing = param & 3;
		int open = (param & 4) != 0;
		int top = (param & 8) != 0;
		if (!open) {
			out[0] = top ? (BlockAABB){0.0f, 0.8125f, 0.0f, 1.0f, 1.0f, 1.0f}
			             : (BlockAABB){0.0f, 0.0f, 0.0f, 1.0f, 0.1875f, 1.0f};
		} else {
			switch (facing) {
			case 0: out[0] = (BlockAABB){0.0f, 0.0f, 0.8125f, 1.0f, 1.0f, 1.0f}; break;
			case 1: out[0] = (BlockAABB){0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.1875f}; break;
			case 2: out[0] = (BlockAABB){0.8125f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f}; break;
			default: out[0] = (BlockAABB){0.0f, 0.0f, 0.0f, 0.1875f, 1.0f, 1.0f}; break;
			}
		}
		return 1;
	}
	case SHAPE_DOOR: {
		/* Simplified from vanilla: BlockDoor.java derives a door voxel's
		 * *rendered* facing/hinge from "combined metadata" that mixes this
		 * voxel's own data with its vertically-paired half's (the upper
		 * half doesn't carry facing in its own data at all, only hinge) --
		 * a second, vertical connectivity axis beyond the horizontal one
		 * FENCE/WALL/PANE already use. Only 4 door voxels exist across every
		 * map in this project, all closed, so rather than add that
		 * machinery for such rare content, this always decodes facing from
		 * bits0-1 of the voxel's *own* data (matching vanilla's
		 * not-open/closed bounds, which only ever need facing, not hinge --
		 * see setBoundBasedOnMeta) -- always a plausible thin door-shaped
		 * panel on some edge, just not guaranteed to match its other half's
		 * facing bit for bit. Doors are never modeled as open (open bounds
		 * additionally need the hinge bit) for the same reason. */
		int facing = param & 3;
		switch (facing) {
		case 0: out[0] = (BlockAABB){0.0f, 0.0f, 0.8125f, 1.0f, 1.0f, 1.0f}; break;
		case 1: out[0] = (BlockAABB){0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.1875f}; break;
		case 2: out[0] = (BlockAABB){0.8125f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f}; break;
		default: out[0] = (BlockAABB){0.0f, 0.0f, 0.0f, 0.1875f, 1.0f, 1.0f}; break;
		}
		return 1;
	}
	case SHAPE_CUBE:
	default:
		/* Also the safe fallback for shapes not yet implemented: collide as
		 * a full cube rather than leaving the block silently walk-through. */
		out[0] = (BlockAABB){0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
		return 1;
	}
}

/* ---- mesh --------------------------------------------------------------
 * All of BlockShape_Mesh's boxes are expressed in sixteenths of a block
 * (0..16), matching world.c's fixed-point vertex format 1:1. */

/* skipMask: bit per face index (0:-X,1:+X,2:-Y,3:+Y,4:-Z,5:+Z) to omit --
 * used for internal seams between the boxes making up a multi-box shape
 * (stairs), where two boxes meet along a *fully coincident* rectangle (same
 * bounds on the shared plane) so the seam is entirely hidden either way. */
static u32 emit_box(s16 x0, s16 y0, s16 z0, s16 x1, s16 y1, s16 z1,
                    int tileSide, int tileTop, int tileBottom, u8 skipMask,
                    BlockQuadFn emit, void *ctx) {
	u32 n = 0;
	int f;
	for (f = 0; f < 6; f++) {
		if (skipMask & (u8)(1u << f)) continue;
		int tile = (f == 3) ? tileTop : (f == 2) ? tileBottom : tileSide;
		emit(ctx, f, x0, y0, z0, x1, y1, z1, tile);
		n++;
	}
	return n;
}

static u32 mesh_slab(u8 param, int g, BlockQuadFn emit, void *ctx) {
	int top = (param & 8) != 0;
	s16 y0 = top ? 8 : 0, y1 = top ? 16 : 8;
	return emit_box(0, y0, 0, 16, y1, 16, g, g_topTile[g], g_bottomTile[g], 0, emit, ctx);
}

/* Stairs are modeled as 3 boxes -- an "open half" box (the low/high step at
 * full height for its half) and a "closed half" pair stacked to form the
 * riser -- chosen specifically so every internal seam between boxes is a
 * *fully coincident* rectangle (never a partial overlap), letting us skip
 * exactly the touching faces with a bitmask instead of clipping geometry.
 * (A naive "base half-slab + quarter riser" 2-box version -- which IS what
 * BlockShape_Boxes uses for collision, matching vanilla's actual AABBs --
 * would z-fight where the quarter riser's bottom face sits exactly on top
 * of half of the base slab's top face.) This intentionally doesn't
 * replicate vanilla's neighbor-dependent inner/outer corner merging
 * (func_176305_g/func_176306_h in BlockStairs.java) -- adjacent perpendicular
 * stairs won't blend into a rounded corner, they'll just show the plain
 * straight shape each on its own, a bounded approximation consistent with
 * this project's existing no-rotation full-cube baseline. */
static u32 mesh_stair(u8 param, int g, BlockQuadFn emit, void *ctx) {
	int facing = param & 3;
	int upsideDown = (param & 4) != 0;
	s16 baseY0 = upsideDown ? 8 : 0, baseY1 = upsideDown ? 16 : 8;
	s16 riserY0 = upsideDown ? 0 : 8, riserY1 = upsideDown ? 8 : 16;
	s16 ox0, oz0, ox1, oz1;   /* open-half footprint (box1)   */
	s16 cx0, cz0, cx1, cz1;   /* closed-half footprint (box3, box2) */
	u8 skip1, skip3;          /* box1<->box3 split-axis seam  */

	switch (facing) {
	case 0: /* closed=x[8,16], open=x[0,8] */
		ox0 = 0; ox1 = 8; cx0 = 8; cx1 = 16; oz0 = cz0 = 0; oz1 = cz1 = 16;
		skip1 = (1 << 1); skip3 = (1 << 0);
		break;
	case 1: /* closed=x[0,8], open=x[8,16] */
		ox0 = 8; ox1 = 16; cx0 = 0; cx1 = 8; oz0 = cz0 = 0; oz1 = cz1 = 16;
		skip1 = (1 << 0); skip3 = (1 << 1);
		break;
	case 2: /* closed=z[8,16], open=z[0,8] */
		oz0 = 0; oz1 = 8; cz0 = 8; cz1 = 16; ox0 = cx0 = 0; ox1 = cx1 = 16;
		skip1 = (1 << 5); skip3 = (1 << 4);
		break;
	default: /* NORTH: closed=z[0,8], open=z[8,16] */
		oz0 = 8; oz1 = 16; cz0 = 0; cz1 = 8; ox0 = cx0 = 0; ox1 = cx1 = 16;
		skip1 = (1 << 4); skip3 = (1 << 5);
		break;
	}
	/* box3<->box2 vertical seam: box3 is the base (low for normal stairs,
	 * high for upside-down), box2 the riser sits above or below it. */
	u8 skip3v = upsideDown ? (1 << 2) : (1 << 3);
	u8 skip2v = upsideDown ? (1 << 3) : (1 << 2);

	int tileTop = g_topTile[g], tileBottom = g_bottomTile[g];
	u32 n = 0;
	n += emit_box(ox0, baseY0, oz0, ox1, baseY1, oz1, g, tileTop, tileBottom,
	              skip1, emit, ctx);
	n += emit_box(cx0, baseY0, cz0, cx1, baseY1, cz1, g, tileTop, tileBottom,
	              (u8)(skip3 | skip3v), emit, ctx);
	n += emit_box(cx0, riserY0, cz0, cx1, riserY1, cz1, g, tileTop, tileBottom,
	              skip2v, emit, ctx);
	return n;
}

/* Fence/gate/wall render as solid beams matching their collision boxes
 * exactly (see BlockShape_Boxes) rather than vanilla's separate thin
 * top/bottom rail quads -- a deliberate visual simplification (still reads
 * clearly as a fence/wall, especially at this project's texture
 * resolution) that keeps the same box math doing double duty for mesh and
 * collision. Overlapping boxes (e.g. a 4-way fence connection) cost a
 * little harmless overdraw, not a visual artifact -- see cb_face's "why no
 * face-culling for custom shapes" comment in world.c. */
static u32 mesh_fence(u8 connectMask, int g, BlockQuadFn emit, void *ctx) {
	int connW = connectMask & 1, connE = (connectMask >> 1) & 1;
	int connN = (connectMask >> 2) & 1, connS = (connectMask >> 3) & 1;
	int tileTop = g_topTile[g], tileBottom = g_bottomTile[g];
	u32 n = 0;
	if (connN || connS) {
		s16 z0 = connN ? 0 : 6, z1 = connS ? 16 : 10;
		n += emit_box(6, 0, z0, 10, 24, z1, g, tileTop, tileBottom, 0, emit, ctx);
	}
	if (connW || connE || (!connN && !connS)) {
		s16 x0 = connW ? 0 : 6, x1 = connE ? 16 : 10;
		n += emit_box(x0, 0, 6, x1, 24, 10, g, tileTop, tileBottom, 0, emit, ctx);
	}
	return n;
}

static u32 mesh_fence_gate(u8 param, int g, BlockQuadFn emit, void *ctx) {
	int facing = param & 3;
	int open = (param & 4) != 0;
	if (open) return 0; /* invisible gap when open, matching 0 collision boxes */
	int axisZ = (facing % 2) == 0;
	int tileTop = g_topTile[g], tileBottom = g_bottomTile[g];
	return axisZ ? emit_box(0, 0, 6, 16, 24, 10, g, tileTop, tileBottom, 0, emit, ctx)
	             : emit_box(6, 0, 0, 10, 24, 16, g, tileTop, tileBottom, 0, emit, ctx);
}

/* Mirrors BlockShape_Boxes' SHAPE_PANE case exactly (same up-to-2-box
 * shape, just in sixteenths for the mesh); shared by iron bars and glass
 * panes. The two boxes never overlap (unlike fence's cross case) so no
 * z-fighting concern here. */
static u32 mesh_pane(u8 connectMask, int g, BlockQuadFn emit, void *ctx) {
	int connW = connectMask & 1, connE = (connectMask >> 1) & 1;
	int connN = (connectMask >> 2) & 1, connS = (connectMask >> 3) & 1;
	int any = connW || connE || connN || connS;
	int tileTop = g_topTile[g], tileBottom = g_bottomTile[g];
	u32 n = 0;
	if ((!connW || !connE) && any) {
		if (connW)      n += emit_box(0, 0, 7, 8, 16, 9, g, tileTop, tileBottom, 0, emit, ctx);
		else if (connE) n += emit_box(8, 0, 7, 16, 16, 9, g, tileTop, tileBottom, 0, emit, ctx);
	} else {
		n += emit_box(0, 0, 7, 16, 16, 9, g, tileTop, tileBottom, 0, emit, ctx);
	}
	if ((!connN || !connS) && any) {
		if (connN)      n += emit_box(7, 0, 0, 9, 16, 8, g, tileTop, tileBottom, 0, emit, ctx);
		else if (connS) n += emit_box(7, 0, 8, 9, 16, 16, g, tileTop, tileBottom, 0, emit, ctx);
	} else {
		n += emit_box(7, 0, 0, 9, 16, 16, g, tileTop, tileBottom, 0, emit, ctx);
	}
	return n;
}

static u32 mesh_anvil(u8 param, int g, BlockQuadFn emit, void *ctx) {
	int facing = param & 3;
	int axisX = (facing % 2) != 0;
	int tileTop = g_topTile[g], tileBottom = g_bottomTile[g];
	return axisX ? emit_box(0, 0, 2, 16, 16, 14, g, tileTop, tileBottom, 0, emit, ctx)
	             : emit_box(2, 0, 0, 14, 16, 16, g, tileTop, tileBottom, 0, emit, ctx);
}

/* The table itself, plus a small static floating book box (fixed position,
 * no bob/spin/page-flip animation, per the task) using one tile
 * (ENCHANT_BOOK_TILE, a crop of the real entity-renderer book texture --
 * see tools/build_atlas.py's load_book_tile()) on all 6 faces in place of
 * vanilla's separate cover/spine/page boxes -- this engine's mesh shapes are
 * all axis-aligned, so it can't replicate the real book's tilted (80° about
 * Z) rotation either; it floats flat instead. Purely decorative: no
 * collision box of its own (see BlockShape_Boxes' SHAPE_ENCHANT_TABLE case,
 * matching vanilla where the book is a TESR visual only). */
static u32 mesh_enchant_table(int g, BlockQuadFn emit, void *ctx) {
	u32 n = emit_box(0, 0, 0, 16, 12, 16, g, g_topTile[g], g_bottomTile[g], 0, emit, ctx);
	n += emit_box(5, 13, 6, 11, 15, 10, ENCHANT_BOOK_TILE, ENCHANT_BOOK_TILE,
	              ENCHANT_BOOK_TILE, 0, emit, ctx);
	return n;
}

static u32 mesh_wall(u8 connectMask, int g, BlockQuadFn emit, void *ctx) {
	int connW = connectMask & 1, connE = (connectMask >> 1) & 1;
	int connN = (connectMask >> 2) & 1, connS = (connectMask >> 3) & 1;
	s16 x0 = connW ? 0 : 4, x1 = connE ? 16 : 12;
	s16 z0 = connN ? 0 : 4, z1 = connS ? 16 : 12;
	return emit_box(x0, 0, z0, x1, 16, z1, g, g_topTile[g], g_bottomTile[g], 0, emit, ctx);
}

static u32 mesh_trapdoor(u8 param, int g, BlockQuadFn emit, void *ctx) {
	int facing = param & 3;
	int open = (param & 4) != 0;
	int top = (param & 8) != 0;
	int tileTop = g_topTile[g], tileBottom = g_bottomTile[g];
	if (!open) {
		return top ? emit_box(0, 13, 0, 16, 16, 16, g, tileTop, tileBottom, 0, emit, ctx)
		           : emit_box(0, 0, 0, 16, 3, 16, g, tileTop, tileBottom, 0, emit, ctx);
	}
	switch (facing) {
	case 0: return emit_box(0, 0, 13, 16, 16, 16, g, tileTop, tileBottom, 0, emit, ctx);
	case 1: return emit_box(0, 0, 0, 16, 16, 3, g, tileTop, tileBottom, 0, emit, ctx);
	case 2: return emit_box(13, 0, 0, 16, 16, 16, g, tileTop, tileBottom, 0, emit, ctx);
	default: return emit_box(0, 0, 0, 3, 16, 16, g, tileTop, tileBottom, 0, emit, ctx);
	}
}

/* Mirrors BlockShape_Boxes' SHAPE_DOOR case exactly (see its comment for the
 * combined-metadata simplification). */
static u32 mesh_door(u8 param, int g, BlockQuadFn emit, void *ctx) {
	int facing = param & 3;
	int tileTop = g_topTile[g], tileBottom = g_bottomTile[g];
	switch (facing) {
	case 0: return emit_box(0, 0, 13, 16, 16, 16, g, tileTop, tileBottom, 0, emit, ctx);
	case 1: return emit_box(0, 0, 0, 16, 16, 3, g, tileTop, tileBottom, 0, emit, ctx);
	case 2: return emit_box(13, 0, 0, 16, 16, 16, g, tileTop, tileBottom, 0, emit, ctx);
	default: return emit_box(0, 0, 0, 3, 16, 16, g, tileTop, tileBottom, 0, emit, ctx);
	}
}

u32 BlockShape_Mesh(u8 shape, u8 param, u8 connectMask, int g,
                    BlockQuadFn emit, void *ctx) {
	switch (shape) {
	case SHAPE_SLAB:       return mesh_slab(param, g, emit, ctx);
	case SHAPE_STAIR:      return mesh_stair(param, g, emit, ctx);
	case SHAPE_FENCE:      return mesh_fence(connectMask, g, emit, ctx);
	case SHAPE_FENCE_GATE: return mesh_fence_gate(param, g, emit, ctx);
	case SHAPE_WALL:       return mesh_wall(connectMask, g, emit, ctx);
	case SHAPE_PANE:       return mesh_pane(connectMask, g, emit, ctx);
	case SHAPE_ANVIL:      return mesh_anvil(param, g, emit, ctx);
	case SHAPE_ENCHANT_TABLE: return mesh_enchant_table(g, emit, ctx);
	case SHAPE_TRAPDOOR:   return mesh_trapdoor(param, g, emit, ctx);
	case SHAPE_DOOR:       return mesh_door(param, g, emit, ctx);
	default:               return 0;
	}
}

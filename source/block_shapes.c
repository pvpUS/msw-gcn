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
	case SHAPE_CROSS:
	case SHAPE_TORCH:
	case SHAPE_LADDER:
	case SHAPE_VINE:
	case SHAPE_PLATE:
		/* All passable in vanilla -- flowers/grass/saplings, torches, ladders
		 * (their climb is a separate non-collision effect), vines, and pressure
		 * plates every return null from getCollisionBoundingBox, so the player
		 * walks straight through. Returning 0 boxes makes World_BlockBoxes /
		 * player.c treat them exactly as air, which is the point. */
		return 0;
	case SHAPE_CHEST:
		/* BlockChest.setBlockBounds(0.0625,0, 0.0625, 0.9375,0.875,0.9375):
		 * a full-footprint solid box, 14/16 tall, inset 1px on the sides. */
		out[0] = (BlockAABB){0.0625f, 0.0f, 0.0625f, 0.9375f, 0.875f, 0.9375f};
		return 1;
	case SHAPE_SKULL: {
		/* meta & 7 = EnumFacing.getFront: 1=UP(on floor), 2..5 = wall mounts
		 * (2=+Z 3=-Z 4=+X 5=-X wall). A floor skull is an 8x8x8 head sitting on
		 * the ground; a wall skull is that head raised to mid-height and pushed
		 * flush against its wall (BlockSkull.setBlockBoundsBasedOnState). */
		int meta = param & 7;
		switch (meta) {
		case 2:  out[0] = (BlockAABB){0.25f, 0.25f, 0.5f, 0.75f, 0.75f, 1.0f}; break;
		case 3:  out[0] = (BlockAABB){0.25f, 0.25f, 0.0f, 0.75f, 0.75f, 0.5f}; break;
		case 4:  out[0] = (BlockAABB){0.5f, 0.25f, 0.25f, 1.0f, 0.75f, 0.75f}; break;
		case 5:  out[0] = (BlockAABB){0.0f, 0.25f, 0.25f, 0.5f, 0.75f, 0.75f}; break;
		default: out[0] = (BlockAABB){0.25f, 0.0f, 0.25f, 0.75f, 0.5f, 0.75f}; break;
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

int BlockShape_SelectBoxes(u8 shape, u8 param, u8 connectMask, BlockAABB out[2]) {
	int n = BlockShape_Boxes(shape, param, connectMask, out);
	if (n) return n;

	/* The passable shapes, using vanilla's setBlockBounds (which is what
	 * getSelectedBoundingBox is built from) rather than their empty
	 * collision box. */
	switch (shape) {
	case SHAPE_CROSS:
		/* BlockBush.setBlockBounds(0.3, 0, 0.3, 0.7, 0.8, 0.7) */
		out[0] = (BlockAABB){0.3f, 0.0f, 0.3f, 0.7f, 0.8f, 0.7f};
		return 1;
	case SHAPE_TORCH: {
		/* BlockTorch.setBlockBoundsBasedOnState: meta 1=E 2=W 3=S 4=N wall
		 * mounts, anything else stands on the floor. */
		switch (param & 7) {
		case 1:  out[0] = (BlockAABB){0.0f,   0.2f, 0.35f, 0.3f,  0.8f, 0.65f}; break;
		case 2:  out[0] = (BlockAABB){0.7f,   0.2f, 0.35f, 1.0f,  0.8f, 0.65f}; break;
		case 3:  out[0] = (BlockAABB){0.35f,  0.2f, 0.0f,  0.65f, 0.8f, 0.3f};  break;
		case 4:  out[0] = (BlockAABB){0.35f,  0.2f, 0.7f,  0.65f, 0.8f, 1.0f};  break;
		default: out[0] = (BlockAABB){0.4f,   0.0f, 0.4f,  0.6f,  0.6f, 0.6f};  break;
		}
		return 1;
	}
	case SHAPE_LADDER: {
		/* BlockLadder: a 2/16 slab flush against the wall it hangs on
		 * (facing meta 2=N 3=S 4=W 5=E, same as the mesh emitter). */
		switch (param & 7) {
		case 2:  out[0] = (BlockAABB){0.0f,   0.0f, 0.875f, 1.0f,   1.0f, 1.0f};  break;
		case 3:  out[0] = (BlockAABB){0.0f,   0.0f, 0.0f,   1.0f,   1.0f, 0.125f};break;
		case 4:  out[0] = (BlockAABB){0.875f, 0.0f, 0.0f,   1.0f,   1.0f, 1.0f};  break;
		default: out[0] = (BlockAABB){0.0f,   0.0f, 0.0f,   0.125f, 1.0f, 1.0f};  break;
		}
		return 1;
	}
	case SHAPE_VINE:
	case SHAPE_PLATE:
		/* Vines hug whichever walls they're attached to and pressure plates
		 * are a thin floor pad; a single full-footprint thin box is enough to
		 * aim at either, and is what BlockVine's own bounds collapse to when
		 * it's attached on all sides. */
		out[0] = (shape == SHAPE_PLATE)
		       ? (BlockAABB){0.0625f, 0.0f, 0.0625f, 0.9375f, 0.0625f, 0.9375f}
		       : (BlockAABB){0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
		return 1;
	case SHAPE_FENCE_GATE:
		/* An open gate has no collision but is still targetable. */
		out[0] = (BlockAABB){0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
		return 1;
	default:
		return 0;
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
		emit(ctx, f, x0, y0, z0, x1, y1, z1, tile, 0);
		n++;
	}
	return n;
}

/* Emit a box, optionally mirroring its X and Z extents first. The custom
 * shapes that are authored facing one way (anvil, fence gate) are symmetric
 * front-to-back, so a plain X<->Z swap rotates the whole model 90deg for the
 * perpendicular facing without needing a real rotation matrix. */
static u32 emit_box_axis(int swapXZ, s16 x0, s16 y0, s16 z0, s16 x1, s16 y1, s16 z1,
                         int tileSide, int tileTop, int tileBottom, u8 skipMask,
                         BlockQuadFn emit, void *ctx) {
	if (swapXZ)
		return emit_box(z0, y0, x0, z1, y1, x1, tileSide, tileTop, tileBottom,
		                skipMask, emit, ctx);
	return emit_box(x0, y0, z0, x1, y1, z1, tileSide, tileTop, tileBottom,
	                skipMask, emit, ctx);
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

/* Real fence geometry -- a 4x4 centre post plus, for each connected side, a
 * pair of thin rails (a lower and an upper bar) -- ground-truthed against
 * block/fence_post.json (post [6,0,6..10,16,10]) and block/fence_n.json (bars
 * at y6..9 and y12..15, cross-section 2). An isolated fence is just the post,
 * exactly like vanilla. Rendered 16 tall (vanilla's model height) even though
 * collision is 1.5 blocks (BlockShape_Boxes' SHAPE_FENCE); a taller mesh would
 * also push side UVs off the top of the tile. Each rail runs from the block
 * edge to the post centre (8) and pokes 2px into the post rather than butting
 * it flush, so the post always hides the rail's inner end -- no coincident
 * internal faces to z-fight. connectMask: bit0=-X 1=+X 2=-Z 3=+Z. */
static u32 mesh_fence(u8 connectMask, int g, BlockQuadFn emit, void *ctx) {
	int connW = connectMask & 1, connE = (connectMask >> 1) & 1;
	int connN = (connectMask >> 2) & 1, connS = (connectMask >> 3) & 1;
	int tt = g_topTile[g], tb = g_bottomTile[g];
	u32 n = 0;
	n += emit_box(6, 0, 6, 10, 16, 10, g, tt, tb, 0, emit, ctx); /* centre post */
	if (connN) { n += emit_box(7, 6, 0, 9, 9, 8, g, tt, tb, 0, emit, ctx);
	             n += emit_box(7, 12, 0, 9, 15, 8, g, tt, tb, 0, emit, ctx); }
	if (connS) { n += emit_box(7, 6, 8, 9, 9, 16, g, tt, tb, 0, emit, ctx);
	             n += emit_box(7, 12, 8, 9, 15, 16, g, tt, tb, 0, emit, ctx); }
	if (connW) { n += emit_box(0, 6, 7, 8, 9, 9, g, tt, tb, 0, emit, ctx);
	             n += emit_box(0, 12, 7, 8, 15, 9, g, tt, tb, 0, emit, ctx); }
	if (connE) { n += emit_box(8, 6, 7, 16, 9, 9, g, tt, tb, 0, emit, ctx);
	             n += emit_box(8, 12, 7, 16, 15, 9, g, tt, tb, 0, emit, ctx); }
	return n;
}

/* The real closed-gate frame -- two end posts, a merged centre post, and an
 * upper+lower bar on each side -- ground-truthed against
 * block/fence_gate_closed.json. The canonical model spans X (thin in Z at
 * 7..9); facing WEST/EAST swaps X<->Z. (Vanilla's two adjacent inner posts
 * [6..8]/[8..10] are merged into one [6..10] centre post -- we don't
 * reproduce their per-part UVs, so the split served no purpose.) Open gates
 * aren't animated swinging against the post, so they render as an empty gap,
 * matching their 0 collision boxes. */
static u32 mesh_fence_gate(u8 param, int g, BlockQuadFn emit, void *ctx) {
	int facing = param & 3;
	int open = (param & 4) != 0;
	if (open) return 0;
	int swap = (facing % 2) != 0;   /* WEST/EAST -> gate spans Z */
	int tt = g_topTile[g], tb = g_bottomTile[g];
	u32 n = 0;
	n += emit_box_axis(swap,  0, 5, 7,  2, 16, 9, g, tt, tb, 0, emit, ctx); /* left post  */
	n += emit_box_axis(swap, 14, 5, 7, 16, 16, 9, g, tt, tb, 0, emit, ctx); /* right post */
	n += emit_box_axis(swap,  6, 6, 7, 10, 15, 9, g, tt, tb, 0, emit, ctx); /* centre post*/
	n += emit_box_axis(swap,  2, 6, 7,  6,  9, 9, g, tt, tb, 0, emit, ctx); /* lower left */
	n += emit_box_axis(swap,  2,12, 7,  6, 15, 9, g, tt, tb, 0, emit, ctx); /* upper left */
	n += emit_box_axis(swap, 10, 6, 7, 14,  9, 9, g, tt, tb, 0, emit, ctx); /* lower right*/
	n += emit_box_axis(swap, 10,12, 7, 14, 15, 9, g, tt, tb, 0, emit, ctx); /* upper right*/
	return n;
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

/* The real anvil silhouette -- wide base, a lower lip, a thin waist, and the
 * wide face/horn top -- as 4 stacked boxes, ground-truthed against
 * block/anvil.json (base [2,0,2..14,4,14], lower lip [4,4,3..12,5,13], waist
 * [6,5,4..10,10,12], top [3,10,0..13,16,16]). The canonical model's long axis
 * (the top/horn) runs along Z; facing WEST/EAST swaps X<->Z so it runs along X
 * instead, matching BlockShape_Boxes' SHAPE_ANVIL axis choice. Damage (bits2+)
 * only changes the top texture in vanilla, not the shape, so it's ignored.
 * Only the top box's up-face uses the anvil working-surface texture
 * (ANVIL_TOP_TILE, vanilla's #top); every other face uses anvil_base (the
 * side tile g). The box-projected UV for that up-face lands on the tile's
 * [3..13]x[0..16] region -- exactly vanilla anvil.json's #top uv -- so it maps
 * 1:1 (bar the 180deg spin vanilla adds, which this no-rotation engine skips). */
static u32 mesh_anvil(u8 param, int g, BlockQuadFn emit, void *ctx) {
	int facing = param & 3;
	int swap = (facing % 2) != 0;   /* WEST/EAST -> long axis along X */
	int tb = g_bottomTile[g];
	u32 n = 0;
	n += emit_box_axis(swap, 2, 0,  2, 14,  4, 14, g, g, tb, 0, emit, ctx); /* base */
	n += emit_box_axis(swap, 4, 4,  3, 12,  5, 13, g, g, tb, 0, emit, ctx); /* lip  */
	n += emit_box_axis(swap, 6, 5,  4, 10, 10, 12, g, g, tb, 0, emit, ctx); /* waist*/
	n += emit_box_axis(swap, 3, 10, 0, 13, 16, 16, g, ANVIL_TOP_TILE, tb, 0, emit, ctx); /* top */
	return n;
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
	/* Floating book: a small closed book, static (no bob/spin/page-flip and no
	 * tilt -- this engine has no rotation). Leather cover on the two flat faces
	 * (up f==3 / down f==2), white page edges on the four sides. Emitted
	 * directly (not via emit_box) with whole=1 so each purpose-made crop
	 * (ENCHANT_BOOK_COVER_TILE / _PAGES_TILE, see tools/build_atlas.py's
	 * load_book_tiles) fills its whole face; the grid-aligned projection
	 * emit_box uses would map only a few texels of that crop onto each face.
	 * See BlockQuadFn's `whole` doc in block_shapes.h. */
	int f;
	for (f = 0; f < 6; f++) {
		int tile = (f == 2 || f == 3) ? ENCHANT_BOOK_COVER_TILE : ENCHANT_BOOK_PAGES_TILE;
		emit(ctx, f, 5, 13, 5, 11, 15, 11, tile, 1);
		n++;
	}
	return n;
}

/* Real wall geometry -- an 8x8x16 centre post plus a 6-wide, 13-tall bar
 * reaching toward each connected side -- ground-truthed against
 * block/wall_post.json (post [4,0,4..12,16,12]) and block/wall_n.json (side
 * bar [5,0,0..11,13,4]). The "narrowed straight run": when a wall connects on
 * exactly two opposite sides and nothing else, vanilla drops the tall post and
 * renders one continuous low bar (block/wall_ns.json). We can't see whether a
 * block sits on top (vanilla's other reason to keep the post), so we assume
 * not -- correct for open straight runs, the common case. connectMask:
 * bit0=-X 1=+X 2=-Z 3=+Z. */
static u32 mesh_wall(u8 connectMask, int g, BlockQuadFn emit, void *ctx) {
	int connW = connectMask & 1, connE = (connectMask >> 1) & 1;
	int connN = (connectMask >> 2) & 1, connS = (connectMask >> 3) & 1;
	int tt = g_topTile[g], tb = g_bottomTile[g];
	if (connN && connS && !connW && !connE)
		return emit_box(5, 0, 0, 11, 13, 16, g, tt, tb, 0, emit, ctx); /* N-S run */
	if (connW && connE && !connN && !connS)
		return emit_box(0, 0, 5, 16, 13, 11, g, tt, tb, 0, emit, ctx); /* W-E run */
	u32 n = emit_box(4, 0, 4, 12, 16, 12, g, tt, tb, 0, emit, ctx);    /* post */
	if (connN) n += emit_box(5, 0, 0, 11, 13, 4, g, tt, tb, 0, emit, ctx);
	if (connS) n += emit_box(5, 0, 12, 11, 13, 16, g, tt, tb, 0, emit, ctx);
	if (connW) n += emit_box(0, 0, 5, 4, 13, 11, g, tt, tb, 0, emit, ctx);
	if (connE) n += emit_box(12, 0, 5, 16, 13, 11, g, tt, tb, 0, emit, ctx);
	return n;
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

/* Two diagonal crossed planes spanning the block corner-to-corner, the vanilla
 * block/cross model (flowers, saplings, dead bush) and block/tallgrass (grass,
 * fern) -- both are the same two-quad cross, just different textures. Emitted
 * via the free-quad path because the planes are 45deg, not axis-aligned box
 * faces. `g` already resolves to the (biome-tinted where relevant) plant tile,
 * and for a DOUBLE_PLANT half it's that half's own top/bottom cross texture.
 * No top/bottom faces exist, so g_topTile/g_bottomTile don't apply. Culling is
 * off, so each plane's single quad shows from both sides. */
static u32 mesh_cross(int g, BlockFreeQuadFn ef, void *ctx) {
	ef(ctx, 0, 0, 0, 16, 0, 16, 16, 16, 16, 0, 16, 0, g);  /* NW<->SE diagonal */
	ef(ctx, 16, 0, 0, 0, 0, 16, 0, 16, 16, 16, 16, 0, g);  /* NE<->SW diagonal */
	return 2;
}

/* The vanilla torch is two full-tile crossed billboards (block/torch's two thin
 * planes at x[7,9] and z[7,9]); the torch_on texture is mostly transparent with
 * the stick+flame down its centre column, so alpha-cutout planes read as a
 * torch. whole=1 forces the full tile onto each plane (the box projection would
 * otherwise clip it to a sliver, and would sample outside the tile once the
 * wall offset below pushes coords negative). This engine has no rotation, so
 * wall torches (meta 1..4, BlockTorch.getStateFromMeta: 1=E 2=W 3=S 4=N facing)
 * aren't tilted -- instead the whole model is shoved against its support wall
 * (opposite its facing) and raised a little, a bounded approximation like the
 * project's other non-rotating shapes. meta 5 = standing on the floor. */
static u32 mesh_torch(u8 param, int g, BlockQuadFn emit, void *ctx) {
	int meta = param & 7;
	s16 dx = 0, dy = 0, dz = 0;
	switch (meta) {
	case 1: dx = -6; dy = 3; break; /* facing E -> against west (-X) wall  */
	case 2: dx =  6; dy = 3; break; /* facing W -> against east (+X) wall  */
	case 3: dz = -6; dy = 3; break; /* facing S -> against north (-Z) wall */
	case 4: dz =  6; dy = 3; break; /* facing N -> against south (+Z) wall */
	default: break;                 /* 5 = standing, centred               */
	}
	/* x-plane (west/east faces) and z-plane (north/south faces); both full-tile */
	emit(ctx, 0, (s16)(dx+7), (s16)(dy+0), (s16)(dz+0), (s16)(dx+9), (s16)(dy+16), (s16)(dz+16), g, 1);
	emit(ctx, 1, (s16)(dx+7), (s16)(dy+0), (s16)(dz+0), (s16)(dx+9), (s16)(dy+16), (s16)(dz+16), g, 1);
	emit(ctx, 4, (s16)(dx+0), (s16)(dy+0), (s16)(dz+7), (s16)(dx+16), (s16)(dy+16), (s16)(dz+9), g, 1);
	emit(ctx, 5, (s16)(dx+0), (s16)(dy+0), (s16)(dz+7), (s16)(dx+16), (s16)(dy+16), (s16)(dz+9), g, 1);
	return 4;
}

/* A single ladder plane flush against one wall (block/ladder: a plane at 15.2
 * on the wall the ladder's facing points away from). param = BlockLadder facing
 * meta (2=N 3=S 4=W 5=E). We emit just the one inner face of a 1px-thick box on
 * that wall (culling off makes it double-sided), with whole=1 for the full
 * ladder tile. Passable -- see BlockShape_Boxes' SHAPE_LADDER (no collision). */
static u32 mesh_ladder(u8 param, int g, BlockQuadFn emit, void *ctx) {
	int facing = param & 7;
	switch (facing) {
	case 3:  emit(ctx, 5, 0, 0, 0, 16, 16, 1, g, 1); break;   /* faces +Z, on -Z wall */
	case 4:  emit(ctx, 0, 15, 0, 0, 16, 16, 16, g, 1); break; /* faces -X, on +X wall */
	case 5:  emit(ctx, 1, 0, 0, 0, 1, 16, 16, g, 1); break;   /* faces +X, on -X wall */
	default: emit(ctx, 4, 0, 0, 15, 16, 16, 16, g, 1); break; /* N: faces -Z, on +Z wall */
	}
	return 1;
}

/* Vine planes, one per set metadata bit (BlockVine: 1=S 2=W 4=N 8=E, each a
 * plane flush against that wall -- block/vine_1 etc.). A vine's meta can carry
 * several sides at once (e.g. 12 = N+E). Same single-inner-face + whole=1 trick
 * as the ladder; a bare 0 (only the computed "up" section vanilla derives, not
 * stored in meta) falls back to one +Z plane so it never vanishes. Passable. */
static u32 mesh_vine(u8 param, int g, BlockQuadFn emit, void *ctx) {
	u32 n = 0;
	if (param & 1) { emit(ctx, 4, 0, 0, 15, 16, 16, 16, g, 1); n++; } /* S -> +Z wall */
	if (param & 2) { emit(ctx, 1, 0, 0, 0, 1, 16, 16, g, 1); n++; }   /* W -> -X wall */
	if (param & 4) { emit(ctx, 5, 0, 0, 0, 16, 16, 1, g, 1); n++; }   /* N -> -Z wall */
	if (param & 8) { emit(ctx, 0, 15, 0, 0, 16, 16, 16, g, 1); n++; } /* E -> +X wall */
	if (n == 0) { emit(ctx, 4, 0, 0, 15, 16, 16, 16, g, 1); n++; }
	return n;
}

/* Pressure plate: block/pressure_plate_up, a thin 14x1x14 slab inset 1px,
 * sitting on the floor. The down (-Y) face is hidden against the block below,
 * so it's skipped. Box-projected UVs land on the plate texture's [1..15]
 * region, matching vanilla's model UVs. Passable (no collision). */
static u32 mesh_plate(int g, BlockQuadFn emit, void *ctx) {
	return emit_box(1, 0, 1, 15, 1, 15, g, g_topTile[g], g_bottomTile[g],
	                (u8)(1 << 2), emit, ctx);
}

/* Chest as a plain inset solid box matching its collision (BlockChest bounds).
 * Vanilla renders chests as an animated TESR entity model with a dedicated
 * unwrapped texture and a lid; this engine has neither, so a wood box (the
 * CHEST id's planks tile) is a deliberate, non-interactable stand-in -- it
 * reads as a chest-sized crate. Facing (param) is unused: the box is symmetric
 * and every face uses the same tile. */
static u32 mesh_chest(int g, BlockQuadFn emit, void *ctx) {
	return emit_box(1, 0, 1, 15, 14, 15, g, g_topTile[g], g_bottomTile[g], 0, emit, ctx);
}

/* Skull as a small head box textured with the default Steve head crops
 * (SKULL_*_TILE, built from entity/steve.png -- see tools/build_atlas.py). Real
 * skulls are a TESR that also picks a player skin / rotates by nibble; per the
 * task we just default every skull to Steve's head. Placement mirrors
 * BlockShape_Boxes' SHAPE_SKULL (floor vs. the four wall mounts). */
static u32 mesh_skull(u8 param, BlockQuadFn emit, void *ctx) {
	int meta = param & 7;
	s16 x0, y0, z0, x1, y1, z1;
	switch (meta) {
	case 2:  x0=4; y0=4; z0=8; x1=12; y1=12; z1=16; break; /* +Z wall */
	case 3:  x0=4; y0=4; z0=0; x1=12; y1=12; z1=8;  break; /* -Z wall */
	case 4:  x0=8; y0=4; z0=4; x1=16; y1=12; z1=12; break; /* +X wall */
	case 5:  x0=0; y0=4; z0=4; x1=8;  y1=12; z1=12; break; /* -X wall */
	default: x0=4; y0=0; z0=4; x1=12; y1=8;  z1=12; break; /* floor   */
	}
	return emit_box(x0, y0, z0, x1, y1, z1, SKULL_SIDE_TILE, SKULL_TOP_TILE,
	                SKULL_BOTTOM_TILE, 0, emit, ctx);
}

u32 BlockShape_Mesh(u8 shape, u8 param, u8 connectMask, int g,
                    BlockQuadFn emit, BlockFreeQuadFn emitFree, void *ctx) {
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
	case SHAPE_CROSS:      return mesh_cross(g, emitFree, ctx);
	case SHAPE_TORCH:      return mesh_torch(param, g, emit, ctx);
	case SHAPE_LADDER:     return mesh_ladder(param, g, emit, ctx);
	case SHAPE_VINE:       return mesh_vine(param, g, emit, ctx);
	case SHAPE_PLATE:      return mesh_plate(g, emit, ctx);
	case SHAPE_CHEST:      return mesh_chest(g, emit, ctx);
	case SHAPE_SKULL:      return mesh_skull(param, emit, ctx);
	default:               return 0;
	}
}

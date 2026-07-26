#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "interact.h"
#include "items.h"
#include "inventory.h"
#include "block_props_gen.h"   /* g_blockProps[], g_familyPlace[], g_blockVariant[] */
#include "block_book_gen.h"    /* DESTROY_STAGE_COUNT */

#define DEG2RAD 0.017453292519943295

/* PlayerControllerMP.getBlockReachDistance(): 4.5 outside creative. */
#define REACH_DISTANCE 4.5

/* Minecraft.rightClickDelayTimer: ticks between repeats while use is held. */
#define PLACE_REPEAT_DELAY 4

/* Our face indices (0:-X 1:+X 2:-Y 3:+Y 4:-Z 5:+Z) as EnumFacing.getIndex()
 * values (DOWN=0 UP=1 NORTH=2 SOUTH=3 WEST=4 EAST=5) and as block offsets. */
static const int faceToFacing[6] = { 4, 5, 0, 1, 2, 3 };
static const int faceStep[6][3] = {
	{-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1},
};
/* EnumFacing.getHorizontal() order (0=SOUTH,1=WEST,2=NORTH,3=EAST) mapped to
 * the getIndex() values above. */
static const int hIndexToFacing[4] = { 3, 4, 2, 5 };

#define FACE_DOWN 2
#define FACE_UP   3

/* ---- targeting --------------------------------------------------------- */

/* Minecraft.getMouseOver: a ray from the eye along the look vector, out to the
 * controller's reach. The look vector matches camera.c's convention (yaw 0
 * faces -Z, positive pitch looks up). */
static void update_target(Interact *in, const World *w, const Player *p) {
	double yaw = p->yaw * DEG2RAD, pitch = p->pitch * DEG2RAD;
	double cp = cos(pitch);
	double lx = -sin(yaw) * cp;
	double ly =  sin(pitch);
	double lz = -cos(yaw) * cp;

	double ex = p->x, ey = p->y + 1.62, ez = p->z;
	in->hasTarget = World_RayTrace(w, ex, ey, ez,
	                               ex + lx * REACH_DISTANCE,
	                               ey + ly * REACH_DISTANCE,
	                               ez + lz * REACH_DISTANCE, &in->target);
}

static int held_item(Player *p) {
	ItemStack *s = Inventory_GetCurrentItem(&p->inventory);
	return s ? s->item : -1;
}

/* ---- drops ------------------------------------------------------------- */

/* Block.dropBlockAsItemWithChance + getItemDropped + quantityDropped, driven
 * by the generated per-id drop table. dropDenom > 1 is vanilla's 1-in-N roll
 * (gravel's flint, tall grass's seeds), falling back to dropAlt. */
static void drop_block(ItemWorld *iw, int id, int bx, int by, int bz) {
	const BlockProps *b = &g_blockProps[id];
	int span = (int)b->dropMax - (int)b->dropMin;
	int count = (int)b->dropMin + (span > 0 ? (rand() % (span + 1)) : 0);

	int j;
	for (j = 0; j < count; j++) {
		int drop = b->drop;
		if (b->dropDenom > 1 && (rand() % b->dropDenom) != 0) drop = b->dropAlt;
		if (drop < 0) continue;
		ItemStack s = { (s16)drop, 0, 1 };
		ItemWorld_SpawnAt(iw, bx, by, bz, s);
	}
}

/* ItemInWorldManager.tryHarvestBlock's core: remove the block, and only spawn
 * its drops when the held item was actually able to harvest it. */
static void destroy_block(Interact *in, World *w, Player *p, ItemWorld *iw,
                          int bx, int by, int bz) {
	int id = World_GetBlock(w, bx, by, bz);
	if (id < 0) return;
	int canHarvest = Block_CanHarvest(id, held_item(p));
	if (!World_SetBlock(w, bx, by, bz, -1)) return;
	if (canHarvest) drop_block(iw, id, bx, by, bz);
	(void)in;
}

/* ---- mining ------------------------------------------------------------ */

static int hitting_position(const Interact *in, Player *p, int bx, int by, int bz) {
	return in->curBx == bx && in->curBy == by && in->curBz == bz &&
	       in->curItem == held_item(p);
}

/* PlayerControllerMP.resetBlockRemoving. */
static void reset_block_removing(Interact *in) {
	if (in->isHittingBlock) {
		in->isHittingBlock = 0;
		in->curBlockDamage = 0.0f;
	}
}

/* PlayerControllerMP.clickBlock: begin (or restart) mining a block, breaking
 * it outright when one tick of progress is already >= 1.0 (dirt with a shovel,
 * flowers with anything). */
static void click_block(Interact *in, World *w, Player *p, ItemWorld *iw,
                        int bx, int by, int bz) {
	if (in->isHittingBlock && hitting_position(in, p, bx, by, bz)) return;

	int id = World_GetBlock(w, bx, by, bz);
	if (id < 0) return;

	if (Block_PlayerRelativeHardness(id, held_item(p), p->onGround) >= 1.0f) {
		destroy_block(in, w, p, iw, bx, by, bz);
		in->isHittingBlock = 0;
		in->curBlockDamage = 0.0f;
		in->blockHitDelay = 5;
		return;
	}

	in->isHittingBlock = 1;
	in->curBx = bx; in->curBy = by; in->curBz = bz;
	in->curItem = held_item(p);
	in->curBlockDamage = 0.0f;
}

/* PlayerControllerMP.onPlayerDamageBlock. */
static void damage_block(Interact *in, World *w, Player *p, ItemWorld *iw,
                         int bx, int by, int bz) {
	if (in->blockHitDelay > 0) { in->blockHitDelay--; return; }

	if (!in->isHittingBlock || !hitting_position(in, p, bx, by, bz)) {
		click_block(in, w, p, iw, bx, by, bz);
		return;
	}

	int id = World_GetBlock(w, bx, by, bz);
	if (id < 0) { in->isHittingBlock = 0; return; }

	in->curBlockDamage +=
		Block_PlayerRelativeHardness(id, held_item(p), p->onGround);

	if (in->curBlockDamage >= 1.0f) {
		in->isHittingBlock = 0;
		destroy_block(in, w, p, iw, bx, by, bz);
		in->curBlockDamage = 0.0f;
		in->blockHitDelay = 5;
	}
}

/* ---- placing ----------------------------------------------------------- */

static inline int is_replaceable(int id) {
	return id < 0 || g_blockProps[id].replaceable;
}

/* EntityPlayer.getHorizontalFacing() as an EnumFacing.getHorizontal() index
 * (0=SOUTH,1=WEST,2=NORTH,3=EAST). Minecraft's yaw 0 faces +Z (south) while
 * this engine's faces -Z, hence the 180 degree shift. */
static int player_hindex(const Player *p) {
	double mcYaw = (double)p->yaw + 180.0;
	return ((int)floor(mcYaw * 4.0 / 360.0 + 0.5)) & 3;
}

/* Block.onBlockPlaced: derive the data value the new block should carry from
 * the click, then map it back to a global id through the family's variant
 * table. Falls back to the picked-up id when the family doesn't reorient or
 * the computed variant was never scanned into the palette. */
static int placed_id(int heldId, int face, double hitY, const Player *p) {
	const BlockProps *b = &g_blockProps[heldId];
	int kind = g_familyPlace[b->family];
	if (kind == PLACE_PLAIN) return heldId;

	int h = player_hindex(p);
	/* Shared "did the player click the top half?" test used by stairs and
	 * slabs (BlockStairs/BlockSlab.onBlockPlaced). */
	int bottomHalf = (face != FACE_DOWN) && (face == FACE_UP || hitY <= 0.5);
	int meta = b->data;

	switch (kind) {
	case PLACE_STAIR:
		/* BlockStairs.getMetaFromState: bits0-1 = 5 - facing.getIndex(),
		 * bit2 = upside down. */
		meta = (5 - hIndexToFacing[h]) | (bottomHalf ? 0 : 4);
		break;
	case PLACE_SLAB:
		meta = (b->data & 7) | (bottomHalf ? 0 : 8);
		break;
	case PLACE_PILLAR:
		/* BlockLog.EnumAxis.fromFacingAxis: Y=0, X=4, Z=8 over the variant. */
		meta = (b->data & 3) |
		       ((face == 0 || face == 1) ? 4 : (face == 4 || face == 5) ? 8 : 0);
		break;
	case PLACE_TORCH:
		/* BlockTorch.getMetaFromState: 1=E 2=W 3=S 4=N, anything else stands
		 * on the floor. */
		meta = (face == 1) ? 1 : (face == 0) ? 2
		     : (face == 5) ? 3 : (face == 4) ? 4 : 5;
		break;
	case PLACE_HFACING:
		/* BlockChest/BlockFurnace.onBlockPlacedBy: faces the player. */
		meta = hIndexToFacing[(h + 2) & 3];
		break;
	case PLACE_LADDER:
		/* BlockLadder.onBlockPlaced: hangs on the wall that was clicked. */
		if (face == FACE_UP || face == FACE_DOWN) return heldId;
		meta = faceToFacing[face];
		break;
	case PLACE_GATE:
		/* BlockFenceGate: meta is the horizontal index directly, closed. */
		meta = h;
		break;
	case PLACE_ANVIL:
		/* BlockAnvil.onBlockPlaced: facing is rotated a quarter turn; the
		 * damage level (bits 2-3) comes along from the stack. */
		meta = ((h + 1) & 3) | (b->data & 12);
		break;
	default:
		return heldId;
	}

	s16 v = g_blockVariant[b->family][meta & 15];
	return v >= 0 ? v : heldId;
}

/* World.checkNoEntityCollision for the one entity that exists: the player. */
static int fits_around_player(const World *w, const Player *p, int id,
                              int bx, int by, int bz) {
	BlockAABB boxes[2];
	int n = World_BlockBoxesFor(w, id, bx, by, bz, boxes);
	double hw = 0.6 * 0.5;
	double pminX = p->x - hw, pmaxX = p->x + hw;
	double pminY = p->y,      pmaxY = p->y + 1.8;
	double pminZ = p->z - hw, pmaxZ = p->z + hw;
	int k;
	for (k = 0; k < n; k++) {
		if (bx + boxes[k].x1 > pminX && bx + boxes[k].x0 < pmaxX &&
		    by + boxes[k].y1 > pminY && by + boxes[k].y0 < pmaxY &&
		    bz + boxes[k].z1 > pminZ && bz + boxes[k].z0 < pmaxZ)
			return 0;
	}
	return 1;
}

/* ItemBlock.onItemUse. Returns 1 if a block was placed. */
static int place_block(World *w, Player *p, const BlockHit *hit) {
	ItemStack *held = Inventory_GetCurrentItem(&p->inventory);
	if (!held || held->count == 0) return 0;
	if (held->item < 0 || held->item >= NUM_BLOCK_IDS) return 0;  /* not an ItemBlock */

	int bx = hit->bx, by = hit->by, bz = hit->bz;
	double hitY = hit->hy - by;                  /* hitY, relative to the clicked block */

	if (!is_replaceable(World_GetBlock(w, bx, by, bz))) {
		bx += faceStep[hit->face][0];
		by += faceStep[hit->face][1];
		bz += faceStep[hit->face][2];
	}
	if (!World_InBounds(w, bx, by, bz)) return 0;
	if (!is_replaceable(World_GetBlock(w, bx, by, bz))) return 0;

	int id = placed_id(held->item, hit->face, hitY, p);
	if (!fits_around_player(w, p, id, bx, by, bz)) return 0;
	if (!World_SetBlock(w, bx, by, bz, id)) return 0;

	if (--held->count == 0) { held->item = -1; held->meta = 0; }
	return 1;
}

/* ---- public ------------------------------------------------------------ */

void Interact_Init(Interact *in) {
	memset(in, 0, sizeof(*in));
	in->curItem = -1;
	in->curBy = -1;
}

void Interact_Tick(Interact *in, World *w, Player *p, ItemWorld *iw,
                   int attackHeld, int useHeld, int frozen) {
	update_target(in, w, p);

	if (in->placeDelay > 0) in->placeDelay--;

	if (frozen) { reset_block_removing(in); return; }

	/* Minecraft.runTick's sendClickBlockToController: keep damaging while the
	 * attack button is held and a block is under the crosshair, otherwise
	 * abandon any dig in progress. */
	if (attackHeld && in->hasTarget)
		damage_block(in, w, p, iw, in->target.bx, in->target.by, in->target.bz);
	else
		reset_block_removing(in);

	/* Minecraft.rightClickMouse, repeat-limited the same way. */
	if (useHeld && in->hasTarget && in->placeDelay == 0) {
		if (place_block(w, p, &in->target)) in->placeDelay = PLACE_REPEAT_DELAY;
	} else if (!useHeld) {
		in->placeDelay = 0;
	}
}

int Interact_BreakStage(const Interact *in) {
	if (!in->isHittingBlock) return -1;
	int stage = (int)(in->curBlockDamage * 10.0f) - 1;
	if (stage < 0) return -1;
	if (stage >= DESTROY_STAGE_COUNT) stage = DESTROY_STAGE_COUNT - 1;
	return stage;
}

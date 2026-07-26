#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>

#include "entityitem.h"
#include "helditem.h"          /* HeldItem_SetupGX / *_DrawBlockMesh / *_DrawFlatMesh */
#include "items.h"
#include "block_faces_gen.h"   /* NUM_BLOCK_IDS */

/* ---- EntityItem constants (1.8.9) -------------------------------------- */
#define ITEM_WIDTH        0.25
#define ITEM_HEIGHT       0.25
#define ITEM_GRAVITY      0.03999999910593033   /* motionY -= 0.04          */
#define ITEM_AIR_DRAG     0.9800000190734863
#define ITEM_SLIPPERINESS 0.6                   /* default block friction   */
#define ITEM_BOUNCE      (-0.5)
#define ITEM_LIFETIME     6000                  /* ticks before despawn     */
#define ITEM_PICKUP_DELAY 10                    /* setDefaultPickupDelay()  */

/* EntityPlayer.onLivingUpdate's collide sweep: bb.expand(1.0, 0.5, 1.0). */
#define PICKUP_EXPAND_XZ  1.0
#define PICKUP_EXPAND_Y   0.5

/* Player box, mirroring player.c's constants (kept local rather than exported
 * so the physics module stays self-contained). */
#define PLAYER_WIDTH      0.6
#define PLAYER_HEIGHT     1.8

static inline double frand(void) { return (double)rand() / ((double)RAND_MAX + 1.0); }

/* ---- axis-aligned bounding boxes ---------------------------------------- */
typedef struct { double minX, minY, minZ, maxX, maxY, maxZ; } AABB;

static AABB item_bb(const EntityItem *e) {
	double hw = ITEM_WIDTH * 0.5;
	AABB b = { e->x - hw, e->y, e->z - hw,
	           e->x + hw, e->y + ITEM_HEIGHT, e->z + hw };
	return b;
}

static AABB bb_offset(AABB b, double dx, double dy, double dz) {
	b.minX += dx; b.maxX += dx;
	b.minY += dy; b.maxY += dy;
	b.minZ += dz; b.maxZ += dz;
	return b;
}

/* AxisAlignedBB.addCoord: grow the box along the direction of motion. */
static AABB bb_addcoord(AABB b, double dx, double dy, double dz) {
	if (dx < 0.0) b.minX += dx; else b.maxX += dx;
	if (dy < 0.0) b.minY += dy; else b.maxY += dy;
	if (dz < 0.0) b.minZ += dz; else b.maxZ += dz;
	return b;
}

static inline int ifloor(double d) { return (int)floor(d); }

/* Entity.moveEntity's swept collision, reduced to what an item needs: no
 * step-up, no sneak ledge guard, no riding. Resolves Y, then X, then Z against
 * the same candidate block set, exactly like player.c's moveEntity does for
 * the player (the shared helpers are kept private to each module because the
 * player's version carries the step-up machinery this one must not have). */
static void item_move(EntityItem *e, const World *w,
                      double dx, double dy, double dz) {
	AABB bb = item_bb(e);
	double wantX = dx, wantY = dy, wantZ = dz;

	AABB grown = bb_addcoord(bb, dx, dy, dz);
	int x0 = ifloor(grown.minX),       x1 = ifloor(grown.maxX + 1.0);
	int y0 = ifloor(grown.minY) - 1,   y1 = ifloor(grown.maxY + 1.0);
	int z0 = ifloor(grown.minZ),       z1 = ifloor(grown.maxZ + 1.0);

	int bx, by, bz, k, axis;
	/* axis 0 = Y, 1 = X, 2 = Z (Minecraft's resolution order) */
	for (axis = 0; axis < 3; axis++) {
		for (bx = x0; bx < x1; bx++)
		for (bz = z0; bz < z1; bz++)
		for (by = y0; by < y1; by++) {
			BlockAABB boxes[2];
			int n = World_BlockBoxes(w, bx, by, bz, boxes);
			for (k = 0; k < n; k++) {
				double bx0 = bx + boxes[k].x0, bx1 = bx + boxes[k].x1;
				double by0 = by + boxes[k].y0, by1 = by + boxes[k].y1;
				double bz0 = bz + boxes[k].z0, bz1 = bz + boxes[k].z1;
				if (axis == 0) {
					if (bb.maxX > bx0 && bb.minX < bx1 && bb.maxZ > bz0 && bb.minZ < bz1) {
						if (dy > 0.0 && bb.maxY <= by0)      { double d = by0 - bb.maxY; if (d < dy) dy = d; }
						else if (dy < 0.0 && bb.minY >= by1) { double d = by1 - bb.minY; if (d > dy) dy = d; }
					}
				} else if (axis == 1) {
					if (bb.maxY > by0 && bb.minY < by1 && bb.maxZ > bz0 && bb.minZ < bz1) {
						if (dx > 0.0 && bb.maxX <= bx0)      { double d = bx0 - bb.maxX; if (d < dx) dx = d; }
						else if (dx < 0.0 && bb.minX >= bx1) { double d = bx1 - bb.minX; if (d > dx) dx = d; }
					}
				} else {
					if (bb.maxX > bx0 && bb.minX < bx1 && bb.maxY > by0 && bb.minY < by1) {
						if (dz > 0.0 && bb.maxZ <= bz0)      { double d = bz0 - bb.maxZ; if (d < dz) dz = d; }
						else if (dz < 0.0 && bb.minZ >= bz1) { double d = bz1 - bb.minZ; if (d > dz) dz = d; }
					}
				}
			}
		}
		if (axis == 0)      bb = bb_offset(bb, 0.0, dy, 0.0);
		else if (axis == 1) bb = bb_offset(bb, dx, 0.0, 0.0);
		else                bb = bb_offset(bb, 0.0, 0.0, dz);
	}

	e->x = (bb.minX + bb.maxX) / 2.0;
	e->y = bb.minY;
	e->z = (bb.minZ + bb.maxZ) / 2.0;

	e->onGround = (wantY != dy) && wantY < 0.0;
	if (wantX != dx) e->motionX = 0.0;
	if (wantY != dy) e->motionY = 0.0;
	if (wantZ != dz) e->motionZ = 0.0;
}

/* ---- spawning ---------------------------------------------------------- */

void ItemWorld_Init(ItemWorld *iw) {
	memset(iw, 0, sizeof(*iw));
}

static EntityItem *alloc_item(ItemWorld *iw) {
	int i;
	for (i = 0; i < MAX_ENTITY_ITEMS; i++)
		if (!iw->e[i].alive) return &iw->e[i];
	/* Full: recycle the oldest, so a burst of drops never silently vanishes
	 * the *newest* ones (which are the ones the player is standing over). */
	EntityItem *oldest = &iw->e[0];
	for (i = 1; i < MAX_ENTITY_ITEMS; i++)
		if (iw->e[i].age > oldest->age) oldest = &iw->e[i];
	return oldest;
}

void ItemWorld_SpawnAt(ItemWorld *iw, int bx, int by, int bz, ItemStack stack) {
	if (stack.count == 0 || stack.item < 0) return;

	/* Block.spawnAsEntity: a random point in the middle half of the block. */
	double f = 0.5;
	double d0 = frand() * f + (1.0 - f) * 0.5;
	double d1 = frand() * f + (1.0 - f) * 0.5;
	double d2 = frand() * f + (1.0 - f) * 0.5;

	EntityItem *e = alloc_item(iw);
	memset(e, 0, sizeof(*e));
	e->alive = 1;
	e->x = bx + d0; e->y = by + d1; e->z = bz + d2;
	e->prevX = e->x; e->prevY = e->y; e->prevZ = e->z;
	/* EntityItem's constructor: a small random hop out of the block. */
	e->motionX = frand() * 0.2 - 0.1;
	e->motionY = 0.2;
	e->motionZ = frand() * 0.2 - 0.1;
	e->hoverStart = (float)(frand() * M_PI * 2.0);
	e->pickupDelay = ITEM_PICKUP_DELAY;
	e->stack = stack;
}

/* ---- merging ----------------------------------------------------------- */

/* EntityItem.combineItems, minus the NBT/subtype tests (a stack here is just
 * an id + meta + count). Returns 1 if `a` was consumed into `b`. */
static int combine_items(EntityItem *a, EntityItem *b) {
	if (a == b || !a->alive || !b->alive) return 0;
	if (a->stack.item != b->stack.item || a->stack.meta != b->stack.meta) return 0;
	int limit = Item_MaxStack(a->stack.item);
	if (limit <= 1) return 0;
	if (b->stack.count + a->stack.count > limit) return 0;

	b->stack.count += a->stack.count;
	if (a->pickupDelay > b->pickupDelay) b->pickupDelay = a->pickupDelay;
	if (a->age < b->age) b->age = a->age;
	a->alive = 0;
	return 1;
}

/* searchForOtherItemsNearby: bb.expand(0.5, 0, 0.5). */
static void search_nearby(ItemWorld *iw, EntityItem *e) {
	AABB a = item_bb(e);
	a.minX -= 0.5; a.maxX += 0.5;
	a.minZ -= 0.5; a.maxZ += 0.5;
	int i;
	for (i = 0; i < MAX_ENTITY_ITEMS && e->alive; i++) {
		EntityItem *o = &iw->e[i];
		if (o == e || !o->alive) continue;
		AABB b = item_bb(o);
		if (b.maxX <= a.minX || b.minX >= a.maxX) continue;
		if (b.maxY <= a.minY || b.minY >= a.maxY) continue;
		if (b.maxZ <= a.minZ || b.minZ >= a.maxZ) continue;
		/* vanilla merges into whichever side holds more, so the survivor is
		 * stable regardless of iteration order */
		if (o->stack.count < e->stack.count) combine_items(o, e);
		else                                 combine_items(e, o);
	}
}

/* ---- tick -------------------------------------------------------------- */

/* EntityItem.onCollideWithPlayer. */
static void collide_with_player(EntityItem *e, Player *p) {
	if (e->pickupDelay > 0) return;
	if (!Inventory_AddItemStack(&p->inventory, &e->stack)) return;
	if (e->stack.count == 0) e->alive = 0;
}

void ItemWorld_Tick(ItemWorld *iw, const World *w, Player *p) {
	/* The player's pickup box (EntityPlayer.onLivingUpdate). */
	double hw = PLAYER_WIDTH * 0.5;
	AABB pb = { p->x - hw - PICKUP_EXPAND_XZ, p->y - PICKUP_EXPAND_Y,
	            p->z - hw - PICKUP_EXPAND_XZ,
	            p->x + hw + PICKUP_EXPAND_XZ, p->y + PLAYER_HEIGHT + PICKUP_EXPAND_Y,
	            p->z + hw + PICKUP_EXPAND_XZ };

	int i;
	for (i = 0; i < MAX_ENTITY_ITEMS; i++) {
		EntityItem *e = &iw->e[i];
		if (!e->alive) continue;

		if (e->pickupDelay > 0) e->pickupDelay--;

		e->prevX = e->x; e->prevY = e->y; e->prevZ = e->z;
		e->motionY -= ITEM_GRAVITY;
		item_move(e, w, e->motionX, e->motionY, e->motionZ);

		int movedBlock = (int)e->prevX != (int)e->x ||
		                 (int)e->prevY != (int)e->y ||
		                 (int)e->prevZ != (int)e->z;
		if (movedBlock || e->age % 25 == 0) search_nearby(iw, e);
		if (!e->alive) continue;

		double f = e->onGround ? ITEM_SLIPPERINESS * ITEM_AIR_DRAG : ITEM_AIR_DRAG;
		e->motionX *= f;
		e->motionY *= ITEM_AIR_DRAG;
		e->motionZ *= f;
		if (e->onGround) e->motionY *= ITEM_BOUNCE;

		e->age++;
		if (e->age >= ITEM_LIFETIME) { e->alive = 0; continue; }

		/* Fell out of the world (skywars maps are islands): don't keep
		 * simulating something nobody will ever reach again. */
		if (e->y < w->miny - 8) { e->alive = 0; continue; }

		AABB b = item_bb(e);
		if (b.maxX > pb.minX && b.minX < pb.maxX &&
		    b.maxY > pb.minY && b.minY < pb.maxY &&
		    b.maxZ > pb.minZ && b.minZ < pb.maxZ)
			collide_with_player(e, p);
	}
}

/* ---- rendering --------------------------------------------------------- */

/* RenderEntityItem: dropped items are quarter-scale, hover on a slow sine and
 * spin around Y. Blocks render as the same shaded cube the held-item view
 * uses; flat items as a single card that also spins (vanilla billboards them,
 * but a spin reads the same at this size and needs no per-item matrix work). */
#define DROP_SCALE 0.25f

void ItemWorld_Draw(const ItemWorld *iw, Mtx view, float alpha) {
	int any = 0, i;
	for (i = 0; i < MAX_ENTITY_ITEMS; i++) if (iw->e[i].alive) { any = 1; break; }
	if (!any) return;

	HeldItem_SetupGX();

	for (i = 0; i < MAX_ENTITY_ITEMS; i++) {
		const EntityItem *e = &iw->e[i];
		if (!e->alive) continue;

		float t = (float)e->age + alpha;
		float bob = sinf(t / 10.0f + e->hoverStart) * 0.1f + 0.1f;
		float spin = (t / 20.0f + e->hoverStart) * (180.0f / (float)M_PI);

		double ix = e->prevX + (e->x - e->prevX) * alpha;
		double iy = e->prevY + (e->y - e->prevY) * alpha;
		double iz = e->prevZ + (e->z - e->prevZ) * alpha;

		Mtx m, r, mv;
		guMtxScale(m, DROP_SCALE * WORLD_BLOCK_SIZE,
		              DROP_SCALE * WORLD_BLOCK_SIZE,
		              DROP_SCALE * WORLD_BLOCK_SIZE);
		guMtxRotDeg(r, 'y', spin);
		guMtxConcat(r, m, m);
		guMtxTransApply(m, m,
		                (float)(ix * WORLD_BLOCK_SIZE),
		                (float)((iy + DROP_SCALE * 0.5 + bob) * WORLD_BLOCK_SIZE),
		                (float)(iz * WORLD_BLOCK_SIZE));
		guMtxConcat(view, m, mv);
		GX_LoadPosMtxImm(mv, GX_PNMTX0);

		if (e->stack.item < NUM_BLOCK_IDS) HeldItem_DrawBlockMesh(e->stack.item);
		else                               HeldItem_DrawFlatMesh(e->stack.item);
	}

	World_SetupRenderState();
}

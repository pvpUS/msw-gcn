#ifndef MSW_ENTITYITEM_H
#define MSW_ENTITYITEM_H

#include <gccore.h>
#include "world.h"
#include "inventory.h"
#include "player.h"

/* net.minecraft.entity.item.EntityItem: the loose item stacks a broken block
 * turns into, bouncing on the ground until the player walks over them.
 *
 * Ported behaviour (1.8.9 / MCP-919):
 *   - Block.spawnAsEntity's random spawn offset inside the broken block and
 *     EntityItem's random initial motion / hover phase;
 *   - onUpdate: gravity 0.04/tick, Entity.moveEntity swept-AABB collision
 *     against the world, 0.98 air drag (block slipperiness on the ground),
 *     the -0.5 bounce, and the 6000-tick (5 minute) despawn;
 *   - searchForOtherItemsNearby/combineItems merging of touching stacks;
 *   - EntityPlayer.onLivingUpdate's pickup sweep over the player box expanded
 *     by (1.0, 0.5, 1.0), gated on the 10-tick setDefaultPickupDelay, feeding
 *     InventoryPlayer.addItemStackToInventory.
 *
 * Dropped, thrown and despawned-item behaviours beyond that (lava, water,
 * explosions, owners/throwers, achievements, pickup sounds) are out of scope
 * for a world with no other entities in it. */

/* Hard cap on simultaneously live drops; well past what breaking blocks by
 * hand can produce, and old items despawn on their own. */
#define MAX_ENTITY_ITEMS 96

typedef struct {
	int    alive;
	double x, y, z;                 /* feet position, block units       */
	double prevX, prevY, prevZ;     /* previous tick, for interpolation */
	double motionX, motionY, motionZ;
	int    onGround;
	ItemStack stack;
	int    age;                     /* ticks alive; despawns at 6000    */
	int    pickupDelay;             /* delayBeforeCanPickup             */
	float  hoverStart;              /* per-item bob/spin phase          */
} EntityItem;

typedef struct {
	EntityItem e[MAX_ENTITY_ITEMS];
} ItemWorld;

void ItemWorld_Init(ItemWorld *iw);

/* Block.spawnAsEntity(world, pos, stack): drop `stack` from inside block
 * (bx,by,bz) with vanilla's random offset and initial motion. */
void ItemWorld_SpawnAt(ItemWorld *iw, int bx, int by, int bz, ItemStack stack);

/* One 20 Hz tick: physics, merging, despawn, and pickup into p->inventory. */
void ItemWorld_Tick(ItemWorld *iw, const World *w, Player *p);

/* Draw every live drop as a small spinning, bobbing item. `alpha` is the same
 * inter-tick fraction the view matrix was built with. Must run after
 * World_Draw (it reuses the perspective projection) and before Hud_Draw. */
void ItemWorld_Draw(const ItemWorld *iw, Mtx view, float alpha);

#endif

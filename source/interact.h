#ifndef MSW_INTERACT_H
#define MSW_INTERACT_H

#include <gccore.h>
#include "world.h"
#include "player.h"
#include "entityitem.h"

/* Block breaking and placing: a port of net.minecraft.client.multiplayer
 * .PlayerControllerMP (the mining-progress state machine) plus the parts of
 * Minecraft.getMouseOver, ItemBlock.onItemUse and Block.harvestBlock that
 * single-player needs.
 *
 * Per 20 Hz tick, Interact_Tick:
 *   1. ray-traces from the eye out to getBlockReachDistance() to find the
 *      targeted block (World_RayTrace);
 *   2. with attack held, accumulates curBlockDamageMP by
 *      Block.getPlayerRelativeBlockHardness() -- so mining speed follows
 *      vanilla hardness, tool class and tool tier exactly -- and destroys the
 *      block once it reaches 1.0, spawning Block.getItemDropped()'s drops as
 *      EntityItems when the held item can harvest it;
 *   3. with use pressed, runs ItemBlock.onItemUse: offset to the clicked side
 *      unless the clicked block is replaceable, refuse if the new block would
 *      intersect the player, derive the placed block's data value from the
 *      click (Block.onBlockPlaced: stair facing/half, slab half, log axis,
 *      torch/ladder/chest/gate/anvil facing), then decrement the stack.
 *
 * Not modelled: creative mode, tool durability, block sounds/particles, tile
 * entities, and the multi-block placement rules of doors/beds. */

typedef struct {
	/* Minecraft.objectMouseOver */
	int      hasTarget;
	BlockHit target;

	/* PlayerControllerMP state */
	int   isHittingBlock;
	int   curBx, curBy, curBz;   /* currentBlock                          */
	int   curItem;               /* currentItemHittingBlock (-1 = empty)  */
	float curBlockDamage;        /* curBlockDamageMP, 0..1                */
	int   blockHitDelay;         /* ticks before the next dig may start   */
	int   placeDelay;            /* Minecraft.rightClickDelayTimer        */
} Interact;

void Interact_Init(Interact *in);

/* One tick. `attackHeld` is the mine button held down, `useHeld` the place
 * button. `frozen` (inventory screen open) suppresses both but still refreshes
 * the target so the outline stays put. */
void Interact_Tick(Interact *in, World *w, Player *p, ItemWorld *iw,
                   int attackHeld, int useHeld, int frozen);

/* Destroy-stage for the crack overlay: 0..DESTROY_STAGE_COUNT-1 while mining,
 * -1 when nothing is being mined (matches vanilla's
 * (int)(curBlockDamageMP * 10) - 1). */
int Interact_BreakStage(const Interact *in);

#endif

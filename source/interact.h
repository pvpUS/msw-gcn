#ifndef MSW_INTERACT_H
#define MSW_INTERACT_H

#include <gccore.h>
#include "world.h"
#include "player.h"
#include "entityitem.h"
#include "entity.h"
#include "input.h"

/* Block breaking and placing: a port of net.minecraft.client.multiplayer
 * .PlayerControllerMP (the mining-progress state machine) plus the parts of
 * Minecraft.getMouseOver, ItemBlock.onItemUse and Block.harvestBlock that
 * single-player needs.
 *
 * Per 20 Hz tick, Interact_Tick:
 *   1. ray-traces from the eye out to getBlockReachDistance() to find the
 *      targeted block (World_RayTrace), then runs the entity pass (T18) to see
 *      whether something hittable is in front of it;
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

/* ---- outbound intent (T14) ----------------------------------------------
 * In a live game every one of the actions above has a packet behind it, and
 * the state machine that decides *when* is exactly the one above -- so rather
 * than duplicate it, or hand this module a socket, each tick leaves a short
 * list of what it did. main.c forwards them; offline nobody reads them.
 *
 * Four is the most a single tick can produce: an abort of the previous dig, a
 * start of a new one, its immediate completion, and a place. */
#define INTERACT_EVENT_MAX 4

enum {
	INTERACT_EV_DIG = 0,
	INTERACT_EV_PLACE,
	INTERACT_EV_USE,     /* right-click with nothing under the crosshair */
};

/* C07PacketPlayerDigging's status, in its own numbering. */
enum {
	DIG_START = 0,
	DIG_ABORT = 1,
	DIG_STOP  = 2,
};

/* INTERACT_EV_USE status. */
enum {
	USE_START   = 0,
	USE_RELEASE = 1,
};

typedef struct {
	u8  kind;                /* INTERACT_EV_*                              */
	u8  status;              /* DIG_* / USE_*                              */
	s16 bx, by, bz;          /* the *clicked* block, local coordinates     */
	u8  face;                /* EnumFacing.getIndex()                      */
	u8  curX, curY, curZ;    /* place: the hit point in sixteenths of a block */
} InteractEvent;

typedef struct {
	/* Minecraft.objectMouseOver, split in two. `hasEntity` wins when set --
	 * vanilla's objectMouseOver is one hit of one type, and the entity pass
	 * only reports something strictly nearer than the block. */
	int       hasTarget;
	BlockHit  target;
	int       hasEntity;
	EntityHit entity;

	/* PlayerControllerMP state */
	int   isHittingBlock;
	int   curBx, curBy, curBz;   /* currentBlock                          */
	int   curItem;               /* currentItemHittingBlock (-1 = empty)  */
	float curBlockDamage;        /* curBlockDamageMP, 0..1                */
	int   blockHitDelay;         /* ticks before the next dig may start   */
	int   placeDelay;            /* Minecraft.rightClickDelayTimer        */
	int   useWasHeld;            /* for the bow/food release edge         */
	int   useStarted;            /* a USE_START is outstanding            */

	InteractEvent ev[INTERACT_EVENT_MAX];
	int           evCount;       /* cleared at the top of every tick      */
} Interact;

void Interact_Init(Interact *in);

/* One tick. `input` supplies the two mouse buttons (already gated by the miss
 * lockout); pass a cleared PlayerInput to suppress both while still refreshing
 * the target, which is what the inventory screen does. `ew` is the remote
 * entity table for the T18 targeting pass, or NULL offline.
 *
 * Drops are suppressed when `p->serverDriven`: in a live game they arrive as
 * real entities from the server, and spawning local ones as well would double
 * every broken block. */
void Interact_Tick(Interact *in, World *w, Player *p, ItemWorld *iw,
                   const PlayerInput *input, const EntityWorld *ew);

/* Destroy-stage for the crack overlay: 0..DESTROY_STAGE_COUNT-1 while mining,
 * -1 when nothing is being mined (matches vanilla's
 * (int)(curBlockDamageMP * 10) - 1). */
int Interact_BreakStage(const Interact *in);

#endif

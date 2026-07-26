#ifndef MSW_NETGAME_H
#define MSW_NETGAME_H

#include <gccore.h>
#include "world.h"
#include "entity.h"
#include "hud.h"
#include "inventory.h"

/* NetGame -- the console's end of a GCLink session.
 *
 * Everything the proxy says about the game lands here: which map is running,
 * which blocks have changed, where every entity is, what the player's health
 * and inventory are, and what the server has put on the action bar. It is one
 * struct and one drain function because a session is one thing, and because
 * main.c's frame loop should read as "poll, tick, draw" rather than as a
 * switch over twenty message types.
 *
 * **Receive only, for now.** Milestone 1 is spectating: the console listens,
 * renders, and sends nothing but the PONG that net.c answers on its own. That
 * is deliberate and it is the whole risk-management strategy of the plan --
 * sending movement without the C0B sprint edges, the S08->C06 reply and the
 * teleport-epoch guard gets the account kicked within seconds, and the failure
 * mode is silently dropped packets. So the send half is T22's, landed whole,
 * and the struct below already carries the two fields it will need
 * (`teleportEpoch`, `selfEid`) so that nothing here has to move when it does.
 */
typedef struct {
	s32 selfEid;         /* the console's own entity, never drawn          */
	u8  teleportEpoch;   /* echoed in every MOVE once T22 sends any        */
	int gameMode;        /* 0 survival, 3 spectator (T26)                  */
	int gameState;       /* GCLINK_GAME_*                                  */

	/* The map. `mapIndex` is what is loaded and `wantMap` what the proxy last
	 * asked for; they differ for exactly as long as it takes main.c to reload
	 * the world, which is why NetGame_Poll stops draining when they do. */
	int mapIndex, wantMap;
	s32 originX, originY, originZ;   /* absolute -> local, for diagnostics */

	/* The last TELEPORT, in local block units and engine angles. In spectate
	 * mode this is what parks the camera on the player the proxy is logged in
	 * as, so the console is looking at the game rather than at the map's
	 * origin corner. */
	double tpX, tpY, tpZ;
	float  tpYaw, tpPitch;
	int    tpPending;

	float health;
	int   heldSlot;

	EntityWorld ents;
	HudNet      hud;
	Inventory   inv;     /* mirror of the server's; T15 makes it authoritative */

	/* Counters, so the on-screen readout can say whether anything is actually
	 * arriving. A live-looking link that has delivered nothing is otherwise
	 * indistinguishable from a working one that is just quiet. */
	u32 msgs, blockSets, entityAdds;
} NetGame;

void NetGame_Init(NetGame *ng);

/* Drain everything the socket has and apply it. `w` is the loaded world, or
 * NULL before there is one (block updates that arrive then are dropped -- the
 * proxy re-sends the whole diff when the console attaches).
 *
 * Returns 1 when `wantMap != mapIndex`, i.e. the caller must load that map and
 * call NetGame_MapLoaded. The drain stops at that point rather than running on,
 * so the join-time block diff that follows MAP_SELECT is not thrown away
 * against a world that has not been loaded yet. */
int  NetGame_Poll(NetGame *ng, World *w);

/* One 20 Hz tick: entity interpolation and the action bar's fade. */
void NetGame_Tick(NetGame *ng);

/* Tell the session which map is now resident. */
void NetGame_MapLoaded(NetGame *ng, int index);

/* Drop every entity and the chat backlog -- a map change or a lost link. */
void NetGame_Reset(NetGame *ng);

#endif

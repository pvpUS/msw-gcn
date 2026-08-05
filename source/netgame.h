#ifndef MSW_NETGAME_H
#define MSW_NETGAME_H

#include <gccore.h>
#include "world.h"
#include "entity.h"
#include "hud.h"
#include "inventory.h"
#include "player.h"
#include "interact.h"
#include "combat.h"

/* NetGame -- the console's end of a GCLink session.
 *
 * Everything the proxy says about the game lands here: which map is running,
 * which blocks have changed, where every entity is, what the player's health
 * and inventory are, and what the server has put on the action bar. It is one
 * struct and one drain function because a session is one thing, and because
 * main.c's frame loop should read as "poll, tick, draw" rather than as a
 * switch over twenty message types.
 *
 * **The send half is all here too, and it is the risky half.** The plan is
 * blunt about this: sending movement without the C0B sprint edges, the
 * S08 -> C06 reply and the teleport-epoch guard gets the account kicked within
 * seconds, and the failure mode is *silently dropped packets* rather than
 * anything visible. The C0B edges and the C06 reply live in the proxy, which is
 * the right place for a protocol state machine; the epoch lives here, because
 * only the console knows which MOVEs it had already queued when the teleport
 * arrived.
 */
typedef struct {
	s32 selfEid;         /* the console's own entity, never drawn          */
	u8  teleportEpoch;   /* bumped by TELEPORT, echoed in every MOVE       */
	int gameMode;        /* 0 survival, 3 spectator (T26)                  */
	int gameState;       /* GCLINK_GAME_*                                  */

	/* The map. `mapIndex` is what is loaded and `wantMap` what the proxy last
	 * asked for; they differ for exactly as long as it takes main.c to reload
	 * the world, which is why NetGame_Poll stops draining when they do. */
	int mapIndex, wantMap;
	s32 originX, originY, originZ;   /* absolute -> local, for diagnostics */

	/* The last TELEPORT, in local block units and engine angles. */
	double tpX, tpY, tpZ;
	float  tpYaw, tpPitch;
	int    tpPending;

	float health;
	int   heldSlot;

	/* USE_STATE: the server says an item use is in progress, so the console
	 * applies vanilla's 0.2x movement. Predicting it locally would be closer
	 * to vanilla but would also diverge the moment the server disagreed about
	 * whether the bow was drawn at all. */
	int usingItem;

	/* Whether to send movement at all. Off in the spectate-only build and
	 * until the first TELEPORT has landed. (The "send a full position at least
	 * every 20 ticks" rule lives in the proxy, with the rest of the C03/C04/
	 * C05/C06 selection -- the console sends one MOVE per tick and lets the
	 * protocol state machine decide what that becomes.) */
	int sendMovement;

	EntityWorld ents;
	HudNet      hud;

	/* Counters, so the on-screen readout can say whether anything is actually
	 * arriving. A live-looking link that has delivered nothing is otherwise
	 * indistinguishable from a working one that is just quiet. */
	u32 msgs, blockSets, entityAdds;
} NetGame;

void NetGame_Init(NetGame *ng);

/* Drain everything the socket has and apply it. `w` is the loaded world, or
 * NULL before there is one (block updates that arrive then are dropped -- the
 * proxy re-sends the whole diff when the console attaches). `p` is the local
 * player, or NULL while spectating with the free-fly camera; when it is
 * present the server owns its health, inventory, held slot, position and
 * velocity outright.
 *
 * Returns 1 when `wantMap != mapIndex`, i.e. the caller must load that map and
 * call NetGame_MapLoaded. The drain stops at that point rather than running on,
 * so the join-time block diff that follows MAP_SELECT is not thrown away
 * against a world that has not been loaded yet. */
int  NetGame_Poll(NetGame *ng, World *w, Player *p);

/* One 20 Hz tick: entity interpolation and the action bar's fade. */
void NetGame_Tick(NetGame *ng);

/* Tell the session which map is now resident. */
void NetGame_MapLoaded(NetGame *ng, int index);

/* Drop every entity and the chat backlog -- a map change or a lost link. */
void NetGame_Reset(NetGame *ng);

/* ---- console -> proxy (T22) --------------------------------------------
 * Every one of these is a no-op unless the link is NET_READY; Net_Send says
 * so, and pushing that test up here would only spread it around. */

/* The 20 Hz movement message. Call once per simulated tick, after Player_Tick,
 * and only in survival or spectator -- never before the first TELEPORT has
 * arrived, because until the server's own position has been echoed back its
 * `hasMoved` stays false and everything sent is silently discarded. */
void NetGame_SendMove(NetGame *ng, const Player *p);

/* Drain one tick of interact.c's intent (dig / place / item use) onto the
 * link. Cheap and total: the state machine that produced them is vanilla's, so
 * there is nothing to decide here. */
void NetGame_SendInteract(NetGame *ng, const Interact *it);

/* Drain combat.c's: the swing (which always goes, hit or miss) and the attack.
 * The proxy puts the held-slot packet in front of the attack, because the
 * server computes damage from *its* idea of what is in your hand. */
void NetGame_SendCombat(NetGame *ng, Combat *c);

/* The held hotbar slot, when the player changed it locally. Absolute. */
void NetGame_SendHeldSlot(NetGame *ng, int slot);

/* One click in the inventory screen: an engine slot index and GCLINK_CLICK_*.
 * The caller applies the click locally as well -- this only tells the server. */
void NetGame_SendWindowClick(NetGame *ng, int slot, int button);

/* One line of chat or a command. The proxy truncates to 100 characters and
 * rate-limits to about one a second, because Spigot kicks on chat spam. */
void NetGame_SendChat(NetGame *ng, const char *text);

/* GCLINK_ACTION_*: drop item, drop stack, resync. */
void NetGame_SendAction(NetGame *ng, int action);

#endif

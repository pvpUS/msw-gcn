#ifndef MSW_COMBAT_H
#define MSW_COMBAT_H

#include <gccore.h>
#include "player.h"
#include "input.h"
#include "interact.h"

/* Minecraft.clickMouse's dispatch, and nothing else.
 *
 * The whole of combat on this client is: swing the arm, name the entity that
 * was under the crosshair, and lock attacks out for ten ticks if there was
 * nothing there. Everything that looks like combat -- damage, knockback,
 * critical hits, fire aspect, the sprint reset -- happens on the server and
 * arrives back as HEALTH, SELF_VELOCITY and ENTITY_ANIM.
 *
 * **No local damage prediction whatsoever, deliberately.** 1.8's
 * EntityLivingBase.attackEntityFrom returns early on the client
 * (EntityLivingBase.java:869-872), so vanilla predicts no damage, no knockback
 * and -- the subtle one -- no sprint reset. Adding any of them here would not
 * make hits land sooner, it would make the console disagree with the server
 * about where everyone is. There is also **no attack cooldown in 1.8**; that
 * arrived in 1.9, and adding one would halve this client's damage output
 * against every real player on the server.
 */

typedef struct {
	/* Filled by Combat_Tick, drained by the caller into GCLink. -1 / 0 when
	 * there is nothing to send. */
	s32 pendingAttackEid;
	int pendingSwing;

	/* The entity the crosshair is on, for the HUD. -1 when none. */
	s32 targetEid;
} Combat;

void Combat_Init(Combat *c);

/* One 20 Hz tick, after Interact_Tick has refreshed the target. `in` is
 * consumed for the attack edge and is where a miss records its lockout, so it
 * is not const. */
void Combat_Tick(Combat *c, Player *p, PlayerInput *in, const Interact *it);

/* Take the queued attack, or -1. Clears it. */
s32 Combat_TakeAttack(Combat *c);

/* Take the queued swing, or 0. Clears it. */
int Combat_TakeSwing(Combat *c);

#endif

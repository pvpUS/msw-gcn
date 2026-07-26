#include <string.h>

#include "combat.h"

void Combat_Init(Combat *c) {
	memset(c, 0, sizeof(*c));
	c->pendingAttackEid = -1;
	c->targetEid = -1;
}

void Combat_Tick(Combat *c, Player *p, PlayerInput *in, const Interact *it) {
	c->targetEid = it->hasEntity ? it->entity.eid : -1;

	/* A spectator's clicks do nothing; sending them would be an attack from
	 * something the server does not consider present. */
	if (p->gameMode == 3) return;

	if (!in->attackEdge) return;

	/* Minecraft.clickMouse's order, exactly: swing first, unconditionally,
	 * then decide what was hit. The arm moves even on a whiff, and that is the
	 * only feedback a miss gets. */
	Pose_Swing(&p->pose);
	c->pendingSwing = 1;

	if (it->hasEntity) {
		c->pendingAttackEid = it->entity.eid;
		return;
	}
	if (it->hasTarget) {
		/* The block is already being dug by Interact_Tick's held-attack path;
		 * clicking one is not a miss and must not start the lockout. */
		return;
	}
	Input_Miss(in);
}

s32 Combat_TakeAttack(Combat *c) {
	s32 eid = c->pendingAttackEid;
	c->pendingAttackEid = -1;
	return eid;
}

int Combat_TakeSwing(Combat *c) {
	int s = c->pendingSwing;
	c->pendingSwing = 0;
	return s;
}

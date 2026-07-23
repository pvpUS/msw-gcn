#ifndef MSW_PLAYER_H
#define MSW_PLAYER_H

#include <gccore.h>
#include "world.h"

/* A first-person player entity with Minecraft 1.8.9 movement physics
 * (EntityLivingBase.moveEntityWithHeading + Entity.moveEntity collision).
 *
 * All state is kept in Minecraft "block units" (1 block edge = 1.0), the same
 * space World_BlockSolid() queries. Rendering scales up by WORLD_BLOCK_SIZE.
 * The physics simulation is fixed-step at 20 Hz (Player_Tick); look/aim runs
 * every rendered frame (Player_Look) and positions are interpolated for the
 * view matrix. */
typedef struct {
	/* position of the feet (bottom-centre of the AABB), block units */
	double x, y, z;
	double prevX, prevY, prevZ;   /* previous tick, for render interpolation */

	double motionX, motionY, motionZ;

	float yaw;    /* degrees, camera convention (yaw 0 faces -Z) */
	float pitch;  /* degrees, clamped to [-89, 89]              */

	int onGround;
	int isCollidedHorizontally;
	int isCollidedVertically;
	int jumpTicks;
	int sprinting;
} Player;

/* Place the player at the world's spawn point, looking forward, at rest. */
void Player_Spawn(Player *p, const World *w);

/* Per-frame: read the C-stick and turn the view (smooth at render rate). */
void Player_Look(Player *p, int chan);

/* One 20 Hz physics tick: samples the movement stick + buttons, applies the
 * Minecraft movement/gravity/friction step and resolves block collisions. */
void Player_Tick(Player *p, const World *w, int chan);

/* Build the eye view matrix. `alpha` in [0,1] interpolates between the last
 * two ticks for smooth motion at the render frame rate. */
void Player_GetViewMatrix(const Player *p, float alpha, Mtx v);

#endif

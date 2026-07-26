#ifndef MSW_PLAYER_H
#define MSW_PLAYER_H

#include <gccore.h>
#include "world.h"
#include "inventory.h"
#include "input.h"
#include "pose.h"

/* A first-person player entity with Minecraft 1.8.9 movement physics
 * (EntityLivingBase.moveEntityWithHeading + Entity.moveEntity collision).
 *
 * All state is kept in Minecraft "block units" (1 block edge = 1.0), the same
 * space World_BlockSolid() queries. Rendering scales up by WORLD_BLOCK_SIZE.
 * The physics simulation is fixed-step at 20 Hz (Player_Tick); look/aim runs
 * every rendered frame (Player_Look) and positions are interpolated for the
 * view matrix.
 *
 * Nothing here reads a controller. Everything the player does arrives as a
 * PlayerInput (input.h), which is what lets the same physics be driven by a
 * pad, by a scripted test, and -- the point of the exercise -- be turned into
 * the 20 Hz movement message the server validates against. */

/* Damage sources, only as finely as the feedback needs. `srcX`/`srcZ` give the
 * hit a direction (attackedAtYaw -> the camera tilt); a source with no position
 * passes PLAYER_DAMAGE_NO_SOURCE. */
enum {
	DMG_GENERIC = 0,
	DMG_FALL,        /* the only locally predicted source                   */
	DMG_PLAYER,
	DMG_PROJECTILE,
	DMG_VOID,
};

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
	int sneaking;

	/* ---- fluids (T21) -----------------------------------------------------
	 * Recomputed every tick from the blocks the AABB overlaps. Being in one
	 * takes a different branch of moveEntityWithHeading entirely -- and, more
	 * to the point, standing *on* water instead of in it reads to the server as
	 * hovering, which is eighty ticks from a "Flying is not enabled" kick. */
	int inWater, inLava;

	/* ---- health / damage (EntityLivingBase, 1.8.9) ------------------------
	 * health is in half-heart units matching Minecraft (max 20 = 10 hearts).
	 * fallDistance accumulates while airborne (Entity.updateFallState) and is
	 * cashed in on landing. The hurtResistantTime window gives brief post-hit
	 * invulnerability so a single landing can't be counted twice. */
	float health;
	float fallDistance;
	int   hurtResistantTime;
	float lastDamage;

	/* ---- server authority (T15/T22) ---------------------------------------
	 * In a live game the server owns health, death and drops, and three places
	 * here would otherwise predict them and then fight S06 UpdateHealth:
	 * player_fall, player_respawn, and interact.c's local drop spawn. One flag
	 * gates all three. */
	int serverDriven;
	int gameMode;        /* 0 survival, 3 spectator (T26)                    */

	/* Item use (eating a golden apple, drawing the bow). The server tells the
	 * console when a use is active and the console applies vanilla's 0.2x
	 * movement, or the position stream diverges for the whole 32 ticks. */
	int itemInUse, itemInUseCount;

	/* spawn point, cached for respawn-on-death (block units) */
	double spawnX, spawnY, spawnZ;

	/* EntityPlayer.inventory -- see inventory.h. */
	Inventory inventory;

	/* Animation state shared with remote entities (pose.h): limb swing, arm
	 * swing, hurt timer. The first-person item arc and the hurt camera tilt
	 * both read it, and so does the third-person model if the local player is
	 * ever drawn. */
	Pose pose;
} Player;

/* Max health (SharedMonsterAttributes.maxHealth default): 20 = 10 hearts. */
#define PLAYER_MAX_HEALTH 20.0f

/* EntityPlayer eye height, and the AABB the collision and fluid tests use. */
#define PLAYER_EYE_HEIGHT   1.62
#define PLAYER_WIDTH        0.6
#define PLAYER_HEIGHT       1.8

/* Place the player at the world's spawn point, looking forward, at rest. */
void Player_Spawn(Player *p, const World *w);

/* Per-frame: turn the view by the sampled stick deltas (degrees). */
void Player_Look(Player *p, float dYaw, float dPitch);

/* One 20 Hz physics tick: applies `in`'s movement intent through the
 * Minecraft movement/gravity/friction step, resolves block collisions, updates
 * the fluid and fall-damage state, and ticks the Pose. Pass a cleared input
 * (Input_Clear) to gate control off while still simulating -- that is what the
 * inventory screen does. */
void Player_Tick(Player *p, const World *w, const PlayerInput *in);

/* Spectator flight (T26). Death on MegaSkywars is a game-mode change and
 * nothing else -- no death screen, no respawn packet -- so this is where a
 * dead player spends the rest of the round.
 *
 * No gravity and no collision, because the server has set noClip and
 * allowFlying for a spectator and its own moveEntity does nothing but assign
 * the position; matching that is what keeps the position streams identical.
 * The speed is capped well inside NetHandlerPlayServer's "moved too quickly"
 * threshold (dist^2 > 100 per tick) rather than left to the stick. */
void Player_TickSpectator(Player *p, const PlayerInput *in);

/* EntityLivingBase.attackEntityFrom + damageEntity, reduced to the no-armor/
 * no-absorption case: applies `amount` half-hearts, honoring the
 * hurtResistantTime window, and records the hit's direction so the camera can
 * tilt away from it. (srcX, srcZ) is the attacker's position in block units;
 * pass the player's own for a hit with no direction. */
void Player_Damage(Player *p, float amount, int source, double srcX, double srcZ);

/* S12 EntityVelocity aimed at us -- knockback. Server-authoritative and never
 * predicted: 1.8's attackEntityFrom returns early client-side, so vanilla
 * predicts no knockback either. Applying what does arrive is not optional --
 * ignore it and the server moves you while you do not, which is permanent
 * rubberband for as long as you are being hit. */
void Player_SetVelocity(Player *p, double mx, double my, double mz);

/* Snap to an absolute position and stop dead: the S08 teleport. Zeroes motion
 * and fall distance, as EntityPlayerSP.setPositionAndUpdate does. */
void Player_Teleport(Player *p, double x, double y, double z,
                     float yaw, float pitch);

/* Build the eye view matrix. `alpha` in [0,1] interpolates between the last
 * two ticks for smooth motion at the render frame rate. Includes the hurt
 * camera tilt -- in 1.8 that tilt is the *entire* local damage feedback, there
 * is no red screen vignette. */
void Player_GetViewMatrix(const Player *p, float alpha, Mtx v);

#endif

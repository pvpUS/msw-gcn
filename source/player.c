#include <math.h>
#include <stdlib.h>
#include <gccore.h>

#include "player.h"
#include "camera.h"
#include "world.h"
#include "items.h"
#include "block_props_gen.h"   /* g_blockProps[].material / .data */

/* ---- Minecraft 1.8.9 physics constants (block units, per 20 Hz tick) --- */
#define DEG2RAD             0.017453292519943295f
#define PITCH_LIMIT         89.0f
#define STEP_HEIGHT         0.6
#define GRAVITY             0.08                 /* motionY -= 0.08          */
#define AIR_DRAG_Y          0.9800000190734863   /* motionY *= 0.98          */
#define BASE_SLIPPERINESS   0.6                  /* default block friction   */
#define GROUND_FRICTION_BASE 0.91
#define WALK_SPEED          0.10000000149011612  /* movementSpeed base value */
#define SPRINT_SPEED_MULT   1.2999999523162842   /* +30% sprint modifier     */
#define SPEED_IN_AIR        0.02                  /* jumpMovementFactor base  */
#define JUMP_UPWARDS_MOTION 0.41999998688697815  /* getJumpUpwardsMotion()   */
#define JUMP_COOLDOWN_TICKS 10

/* moveEntityWithHeading's liquid branches. Water drags to 0.8 of the previous
 * tick's speed (0.9 while sprinting -- swimming forward is faster), lava to
 * half, and both accelerate at a flat 0.02 regardless of what is underfoot. */
#define WATER_ACCEL         0.02
#define WATER_DRAG          0.800000011920929
#define WATER_SPRINT_DRAG   0.9
#define LAVA_DRAG           0.5
#define LIQUID_SINK         0.02                 /* motionY -= 0.02          */
#define LIQUID_JUMP         0.03999999910593033  /* jump held while in one   */
#define LIQUID_LEDGE_BUMP   0.30000001192092896  /* climb out onto a ledge   */

/* EntityLivingBase.maxHurtResistantTime: ticks of invulnerability after a hit
 * (the damage test uses half of this). */
#define MAX_HURT_RESISTANT_TIME 20

/* moveFlying's magic constant is exactly (0.6*0.91)^3, so on default ground
 * the acceleration factor 0.16277136/f4^3 reduces to 1.0. */
#define MOVE_FLYING_CONST   0.16277136

/* EntityPlayer.onLivingUpdate: eating, drinking or drawing a bow cuts movement
 * input to a fifth. Without it the console's position stream diverges from the
 * server's for the whole 32 ticks of every golden apple. */
#define ITEM_USE_SLOWDOWN   0.2f

/* EntityRenderer.hurtCameraEffect: the roll, in degrees, at the peak of the
 * flinch. In 1.8 this tilt is the *entire* local damage feedback -- there is no
 * red screen tint on the player's own view. */
#define HURT_TILT_DEGREES   14.0f

/* Spectator flight, blocks per tick. NetHandlerPlayServer kicks on a single
 * tick's dist^2 over 100, i.e. 10 blocks; this is a twentieth of that, which
 * is still 10 blocks a second and comfortably faster than walking. */
#define SPECTATOR_SPEED     0.5

/* ---- axis-aligned bounding box ---------------------------------------- */
typedef struct { double minX, minY, minZ, maxX, maxY, maxZ; } AABB;

static AABB player_bb(const Player *p) {
	double hw = PLAYER_WIDTH * 0.5;
	AABB b = { p->x - hw, p->y, p->z - hw,
	           p->x + hw, p->y + PLAYER_HEIGHT, p->z + hw };
	return b;
}

static AABB bb_offset(AABB b, double dx, double dy, double dz) {
	b.minX += dx; b.maxX += dx;
	b.minY += dy; b.maxY += dy;
	b.minZ += dz; b.maxZ += dz;
	return b;
}

/* AxisAlignedBB.expand: negative arguments shrink, which is how both fluid
 * tests are written in vanilla. */
static AABB bb_expand(AABB b, double dx, double dy, double dz) {
	b.minX -= dx; b.maxX += dx;
	b.minY -= dy; b.maxY += dy;
	b.minZ -= dz; b.maxZ += dz;
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

/* AxisAlignedBB.calculate{X,Y,Z}Offset with an arbitrary block-local
 * collision box (x0..x1 etc., world-absolute) as `this` and the moving
 * player box as `other`. Bounds used to be hardcoded to a full unit cube
 * (bx..bx+1); now generalized so non-cube block shapes (slabs, stairs,
 * fences, ...) can supply their own partial box via World_BlockBoxes --
 * full cubes still pass {bx,bx+1} etc. so behavior is unchanged for them. */
static double calcXOffset(double x0, double y0, double z0,
                          double x1, double y1, double z1, AABB o, double off) {
	if (o.maxY > y0 && o.minY < y1 && o.maxZ > z0 && o.minZ < z1) {
		if (off > 0.0 && o.maxX <= x0)      { double d = x0 - o.maxX; if (d < off) off = d; }
		else if (off < 0.0 && o.minX >= x1) { double d = x1 - o.minX; if (d > off) off = d; }
	}
	return off;
}
static double calcYOffset(double x0, double y0, double z0,
                          double x1, double y1, double z1, AABB o, double off) {
	if (o.maxX > x0 && o.minX < x1 && o.maxZ > z0 && o.minZ < z1) {
		if (off > 0.0 && o.maxY <= y0)      { double d = y0 - o.maxY; if (d < off) off = d; }
		else if (off < 0.0 && o.minY >= y1) { double d = y1 - o.minY; if (d > off) off = d; }
	}
	return off;
}
static double calcZOffset(double x0, double y0, double z0,
                          double x1, double y1, double z1, AABB o, double off) {
	if (o.maxX > x0 && o.minX < x1 && o.maxY > y0 && o.minY < y1) {
		if (off > 0.0 && o.maxZ <= z0)      { double d = z0 - o.maxZ; if (d < off) off = d; }
		else if (off < 0.0 && o.minZ >= z1) { double d = z1 - o.minZ; if (d > off) off = d; }
	}
	return off;
}

/* World.getCollidingBoundingBoxes(...).isEmpty(): 1 if no block's collision
 * box intersects `b`. Used by the sneak ledge guard and the swim-out test.
 * Liquids have no collision box at all (World_BlockBoxes returns none), so
 * this is already liquid-transparent. */
static int no_collision(const World *w, AABB b) {
	int x0 = ifloor(b.minX), x1 = ifloor(b.maxX);
	int y0 = ifloor(b.minY), y1 = ifloor(b.maxY);
	int z0 = ifloor(b.minZ), z1 = ifloor(b.maxZ);
	int bx, by, bz;
	for (bx = x0; bx <= x1; bx++)
		for (bz = z0; bz <= z1; bz++)
			for (by = y0; by <= y1; by++) {
				BlockAABB boxes[2];
				int n = World_BlockBoxes(w, bx, by, bz, boxes);
				int k;
				for (k = 0; k < n; k++) {
					double wx0 = bx + boxes[k].x0, wx1 = bx + boxes[k].x1;
					double wy0 = by + boxes[k].y0, wy1 = by + boxes[k].y1;
					double wz0 = bz + boxes[k].z0, wz1 = bz + boxes[k].z1;
					if (wx1 > b.minX && wx0 < b.maxX &&
					    wy1 > b.minY && wy0 < b.maxY &&
					    wz1 > b.minZ && wz0 < b.maxZ)
						return 0;
				}
			}
	return 1;
}

/* ---- fluids (T21) ------------------------------------------------------
 * The engine's water and lava are full opaque-looking cubes on screen and
 * nothing at all to the collision code: World_BlockBoxes returns no boxes for
 * them. Being *inside* one is therefore not a collision question but a
 * containment one, and these three functions are World.isMaterialInBB,
 * World.handleMaterialAcceleration's containment half, and World.isAnyLiquid.
 *
 * Getting this right is the cheapest kick prevention in the plan. A player who
 * stands on a water surface is, from the server's point of view, hovering in
 * mid-air, and eighty ticks of that is "Flying is not enabled on this
 * server". */

/* BlockLiquid.getLiquidHeightPercent: a source or a falling column is full
 * height; a flowing level 1-7 steps down in ninths. */
static float liquid_height_pct(int level) {
	if (level >= 8) level = 0;
	return (float)(level + 1) / 9.0f;
}

/* World.isMaterialInBB: any block of `mat` overlapping the box at all. */
static int material_in_bb(const World *w, AABB b, int mat) {
	int x0 = ifloor(b.minX), x1 = ifloor(b.maxX + 1.0);
	int y0 = ifloor(b.minY), y1 = ifloor(b.maxY + 1.0);
	int z0 = ifloor(b.minZ), z1 = ifloor(b.maxZ + 1.0);
	int bx, by, bz;
	for (bx = x0; bx < x1; bx++)
		for (by = y0; by < y1; by++)
			for (bz = z0; bz < z1; bz++) {
				int id = World_GetBlock(w, bx, by, bz);
				if (id >= 0 && g_blockProps[id].material == mat) return 1;
			}
	return 0;
}

/* World.handleMaterialAcceleration, minus the flow-acceleration vector this
 * engine does not model: is any block of `mat` in the box, *and* does the box
 * reach the fluid's own surface height rather than merely its cell. */
static int material_reaches_surface(const World *w, AABB b, int mat) {
	int x0 = ifloor(b.minX), x1 = ifloor(b.maxX + 1.0);
	int y0 = ifloor(b.minY), y1 = ifloor(b.maxY + 1.0);
	int z0 = ifloor(b.minZ), z1 = ifloor(b.maxZ + 1.0);
	int bx, by, bz;
	for (bx = x0; bx < x1; bx++)
		for (by = y0; by < y1; by++)
			for (bz = z0; bz < z1; bz++) {
				int id = World_GetBlock(w, bx, by, bz);
				if (id < 0 || g_blockProps[id].material != mat) continue;
				double surface = (double)(by + 1) -
				                 (double)liquid_height_pct(g_blockProps[id].data);
				if ((double)y1 >= surface) return 1;
			}
	return 0;
}

/* World.isAnyLiquid. */
static int any_liquid(const World *w, AABB b) {
	int x0 = ifloor(b.minX), x1 = ifloor(b.maxX + 1.0);
	int y0 = ifloor(b.minY), y1 = ifloor(b.maxY + 1.0);
	int z0 = ifloor(b.minZ), z1 = ifloor(b.maxZ + 1.0);
	int bx, by, bz;
	for (bx = x0; bx < x1; bx++)
		for (by = y0; by < y1; by++)
			for (bz = z0; bz < z1; bz++)
				if (Block_IsLiquid(World_GetBlock(w, bx, by, bz))) return 1;
	return 0;
}

/* Entity.handleWaterMovement / Entity.isInLava, with vanilla's exact shrinks:
 * water wants the box lifted 0.4 off the feet so a puddle underfoot does not
 * count, lava the same idea a little narrower. */
static int in_water(const Player *p, const World *w) {
	AABB b = player_bb(p);
	b = bb_expand(b, 0.0, -0.4000000059604645, 0.0);
	b = bb_expand(b, -0.001, -0.001, -0.001);
	return material_reaches_surface(w, b, MAT_WATER);
}

static int in_lava(const Player *p, const World *w) {
	AABB b = bb_expand(player_bb(p), -0.10000000149011612, -0.4000000059604645,
	                   -0.10000000149011612);
	return material_in_bb(w, b, MAT_LAVA);
}

/* Entity.isOffsetPositionInLiquid: could the box move there and still be in a
 * liquid with nothing solid in the way. This is the ledge bump -- swimming
 * into a wall with liquid at head height pushes you up and out of the water,
 * which is what climbing out of a pool feels like. */
static int offset_in_liquid(const Player *p, const World *w,
                            double dx, double dy, double dz) {
	AABB b = bb_offset(player_bb(p), dx, dy, dz);
	return no_collision(w, b) && !any_liquid(w, b);
}

/* Integer block range covering an AABB, matching getCollidingBoundingBoxes.
 * (y range starts one below to mirror Minecraft's k-1 lower bound.) */
typedef struct { int x0, x1, y0, y1, z0, z1; } Range;
static Range bb_range(AABB b) {
	Range r;
	r.x0 = ifloor(b.minX);        r.x1 = ifloor(b.maxX + 1.0);
	r.y0 = ifloor(b.minY) - 1;    r.y1 = ifloor(b.maxY + 1.0);
	r.z0 = ifloor(b.minZ);        r.z1 = ifloor(b.maxZ + 1.0);
	return r;
}

static double sweepX(const World *w, Range r, AABB o, double off) {
	int bx, by, bz;
	for (bx = r.x0; bx < r.x1; bx++)
		for (bz = r.z0; bz < r.z1; bz++)
			for (by = r.y0; by < r.y1; by++) {
				BlockAABB boxes[2];
				int n = World_BlockBoxes(w, bx, by, bz, boxes);
				int k;
				for (k = 0; k < n; k++)
					off = calcXOffset(bx + boxes[k].x0, by + boxes[k].y0, bz + boxes[k].z0,
					                   bx + boxes[k].x1, by + boxes[k].y1, bz + boxes[k].z1,
					                   o, off);
			}
	return off;
}
static double sweepY(const World *w, Range r, AABB o, double off) {
	int bx, by, bz;
	for (bx = r.x0; bx < r.x1; bx++)
		for (bz = r.z0; bz < r.z1; bz++)
			for (by = r.y0; by < r.y1; by++) {
				BlockAABB boxes[2];
				int n = World_BlockBoxes(w, bx, by, bz, boxes);
				int k;
				for (k = 0; k < n; k++)
					off = calcYOffset(bx + boxes[k].x0, by + boxes[k].y0, bz + boxes[k].z0,
					                   bx + boxes[k].x1, by + boxes[k].y1, bz + boxes[k].z1,
					                   o, off);
			}
	return off;
}
static double sweepZ(const World *w, Range r, AABB o, double off) {
	int bx, by, bz;
	for (bx = r.x0; bx < r.x1; bx++)
		for (bz = r.z0; bz < r.z1; bz++)
			for (by = r.y0; by < r.y1; by++) {
				BlockAABB boxes[2];
				int n = World_BlockBoxes(w, bx, by, bz, boxes);
				int k;
				for (k = 0; k < n; k++)
					off = calcZOffset(bx + boxes[k].x0, by + boxes[k].y0, bz + boxes[k].z0,
					                   bx + boxes[k].x1, by + boxes[k].y1, bz + boxes[k].z1,
					                   o, off);
			}
	return off;
}

/* ---- Entity.moveEntity: swept AABB collision with step-up -------------- */
static void moveEntity(Player *p, const World *w,
                       double x, double y, double z, int sneaking) {
	AABB bb = player_bb(p);

	double d3 = x, d4 = y, d5 = z;

	/* Sneak ledge guard: if standing and sneaking, shorten horizontal motion
	 * so the box never leaves solid ground below it (no walking off edges). */
	int flag = p->onGround && sneaking;
	if (flag) {
		double d6 = 0.05;
		for (; x != 0.0 && no_collision(w, bb_offset(bb, x, -1.0, 0.0)); d3 = x) {
			if (x < d6 && x >= -d6) x = 0.0;
			else if (x > 0.0)       x -= d6;
			else                    x += d6;
		}
		for (; z != 0.0 && no_collision(w, bb_offset(bb, 0.0, -1.0, z)); d5 = z) {
			if (z < d6 && z >= -d6) z = 0.0;
			else if (z > 0.0)       z -= d6;
			else                    z += d6;
		}
		for (; x != 0.0 && z != 0.0 && no_collision(w, bb_offset(bb, x, -1.0, z)); d5 = z) {
			if (x < d6 && x >= -d6) x = 0.0;
			else if (x > 0.0)       x -= d6;
			else                    x += d6;
			d3 = x;
			if (z < d6 && z >= -d6) z = 0.0;
			else if (z > 0.0)       z -= d6;
			else                    z += d6;
		}
	}

	Range r = bb_range(bb_addcoord(bb, x, y, z));

	/* resolve Y, then X, then Z against the same candidate set */
	y = sweepY(w, r, bb, y);
	bb = bb_offset(bb, 0.0, y, 0.0);
	int flag1 = p->onGround || (d4 != y && d4 < 0.0);

	x = sweepX(w, r, bb, x);
	bb = bb_offset(bb, x, 0.0, 0.0);

	z = sweepZ(w, r, bb, z);
	bb = bb_offset(bb, 0.0, 0.0, z);

	/* auto step-up over <= STEP_HEIGHT ledges when blocked horizontally */
	if (STEP_HEIGHT > 0.0f && flag1 && (d3 != x || d5 != z)) {
		double d11 = x, d7 = y, d8 = z;
		AABB bbAfter = bb;        /* result without stepping (axisalignedbb3) */
		bb = player_bb(p);        /* reset to the original box (axisalignedbb) */

		y = STEP_HEIGHT;
		Range rs = bb_range(bb_addcoord(bb, d3, y, d5));

		/* variant A: expand then step */
		AABB a4 = bb;
		AABB a5 = bb_addcoord(a4, d3, 0.0, d5);
		double d9 = sweepY(w, rs, a5, y);
		a4 = bb_offset(a4, 0.0, d9, 0.0);
		double d15 = sweepX(w, rs, a4, d3);
		a4 = bb_offset(a4, d15, 0.0, 0.0);
		double d16 = sweepZ(w, rs, a4, d5);
		a4 = bb_offset(a4, 0.0, 0.0, d16);

		/* variant B: step then move */
		AABB a14 = bb;
		double d17 = sweepY(w, rs, a14, y);
		a14 = bb_offset(a14, 0.0, d17, 0.0);
		double d18 = sweepX(w, rs, a14, d3);
		a14 = bb_offset(a14, d18, 0.0, 0.0);
		double d19 = sweepZ(w, rs, a14, d5);
		a14 = bb_offset(a14, 0.0, 0.0, d19);

		double d20 = d15 * d15 + d16 * d16;
		double d10 = d18 * d18 + d19 * d19;
		if (d20 > d10) { x = d15; z = d16; y = -d9;  bb = a4; }
		else           { x = d18; z = d19; y = -d17; bb = a14; }

		y = sweepY(w, rs, bb, y);
		bb = bb_offset(bb, 0.0, y, 0.0);

		/* keep the stepped result only if it went further horizontally */
		if (d11 * d11 + d8 * d8 >= x * x + z * z) {
			x = d11; y = d7; z = d8; bb = bbAfter;
		}
	}

	/* resetPositionToBB */
	p->x = (bb.minX + bb.maxX) / 2.0;
	p->y = bb.minY;
	p->z = (bb.minZ + bb.maxZ) / 2.0;

	p->isCollidedHorizontally = (d3 != x) || (d5 != z);
	p->isCollidedVertically   = (d4 != y);
	p->onGround = p->isCollidedVertically && d4 < 0.0;

	if (d3 != x) p->motionX = 0.0;
	if (d5 != z) p->motionZ = 0.0;
	/* Block.onLanded: any vertical collision (landing or head-bump) kills
	 * vertical velocity. Without this a resting player accumulates downward
	 * speed toward terminal velocity and plummets when walking off a ledge. */
	if (d4 != y) p->motionY = 0.0;
}

/* Entity.moveFlying: accelerate along the facing direction. Uses the camera
 * yaw basis (forward = (-sin,-cos), right = (cos,-sin)); this is the
 * Minecraft formula with Z negated to match this project's yaw convention. */
static void moveFlying(Player *p, double strafe, double forward, double friction) {
	double f = strafe * strafe + forward * forward;
	if (f < 1.0e-4) return;
	f = sqrt(f);
	if (f < 1.0) f = 1.0;
	f = friction / f;
	strafe *= f;
	forward *= f;
	double s = sin(p->yaw * DEG2RAD);
	double c = cos(p->yaw * DEG2RAD);
	p->motionX += forward * (-s) + strafe * ( c);
	p->motionZ += forward * (-c) + strafe * (-s);
}

/* EntityLivingBase.jump (+ EntityPlayer sprint boost, in camera yaw basis). */
static void player_jump(Player *p) {
	p->motionY = JUMP_UPWARDS_MOTION;
	if (p->sprinting) {
		double s = sin(p->yaw * DEG2RAD);
		double c = cos(p->yaw * DEG2RAD);
		p->motionX += (-s) * 0.2;
		p->motionZ += (-c) * 0.2;
	}
}

/* EntityLivingBase.moveEntityWithHeading. Three branches, in vanilla's order:
 * water, lava, then the ground/air one everything else takes. The liquid
 * branches ignore block friction and step-up entirely -- you do not walk in
 * water, you swim through it. */
static void moveWithHeading(Player *p, const World *w,
                            double strafe, double forward, int sneaking) {
	if (p->inWater) {
		double y0 = p->y;
		double drag = p->sprinting ? WATER_SPRINT_DRAG : WATER_DRAG;

		moveFlying(p, strafe, forward, WATER_ACCEL);
		moveEntity(p, w, p->motionX, p->motionY, p->motionZ, sneaking);
		p->motionX *= drag;
		p->motionY *= WATER_DRAG;
		p->motionZ *= drag;
		p->motionY -= LIQUID_SINK;

		if (p->isCollidedHorizontally &&
		    offset_in_liquid(p, w, p->motionX,
		                     p->motionY + 0.6000000238418579 - p->y + y0,
		                     p->motionZ))
			p->motionY = LIQUID_LEDGE_BUMP;
		return;
	}

	if (p->inLava) {
		double y0 = p->y;

		moveFlying(p, strafe, forward, WATER_ACCEL);
		moveEntity(p, w, p->motionX, p->motionY, p->motionZ, sneaking);
		p->motionX *= LAVA_DRAG;
		p->motionY *= LAVA_DRAG;
		p->motionZ *= LAVA_DRAG;
		p->motionY -= LIQUID_SINK;

		if (p->isCollidedHorizontally &&
		    offset_in_liquid(p, w, p->motionX,
		                     p->motionY + 0.6000000238418579 - p->y + y0,
		                     p->motionZ))
			p->motionY = LIQUID_LEDGE_BUMP;
		return;
	}

	/* friction of the block under the feet (default 0.6 -> 0.546 on ground) */
	double f4 = p->onGround ? BASE_SLIPPERINESS * GROUND_FRICTION_BASE
	                        : GROUND_FRICTION_BASE;

	double aiSpeed = WALK_SPEED * (p->sprinting ? SPRINT_SPEED_MULT : 1.0);
	double jumpFactor = SPEED_IN_AIR * (p->sprinting ? SPRINT_SPEED_MULT : 1.0);

	double accel = p->onGround
	             ? aiSpeed * (MOVE_FLYING_CONST / (f4 * f4 * f4))
	             : jumpFactor;

	moveFlying(p, strafe, forward, accel);

	/* recompute f4 for the post-move friction (matches Minecraft's re-read) */
	f4 = p->onGround ? BASE_SLIPPERINESS * GROUND_FRICTION_BASE
	                 : GROUND_FRICTION_BASE;

	moveEntity(p, w, p->motionX, p->motionY, p->motionZ, sneaking);

	p->motionY -= GRAVITY;
	p->motionY *= AIR_DRAG_Y;
	p->motionX *= f4;
	p->motionZ *= f4;
}

/* ---- health / fall damage (EntityLivingBase + Entity, 1.8.9) ----------- */

void Player_Damage(Player *p, float amount, int source,
                   double srcX, double srcZ) {
	/* attackEntityFrom: dead entities and (here) the invulnerability window. */
	if (p->health <= 0.0f) return;

	int flinch;
	if ((float)p->hurtResistantTime > (float)MAX_HURT_RESISTANT_TIME / 2.0f) {
		/* still invulnerable: only the *extra* over the last hit lands, and it
		 * does not restart the flinch -- that is why a fire tick during a fight
		 * does not reset the ten-tick red flash. */
		if (amount <= p->lastDamage) return;
		p->health -= (amount - p->lastDamage);   /* damageEntity(amount-last) */
		p->lastDamage = amount;
		flinch = 0;
	} else {
		p->lastDamage = amount;
		p->hurtResistantTime = MAX_HURT_RESISTANT_TIME;
		p->health -= amount;                     /* damageEntity(amount) */
		flinch = 1;
	}

	/* Where the hit came from, relative to the way the player is facing --
	 * which is the axis the camera tilts about. A source with no position
	 * (falling, the void) gets vanilla's coin flip rather than a fixed side.
	 * Recorded on both branches, as vanilla does, so a follow-up hit from
	 * behind still turns the tilt even while the first one is still decaying. */
	double dx = srcX - p->x, dz = srcZ - p->z;
	if (dx * dx + dz * dz < 1.0e-4)
		p->pose.attackedAtYaw = (rand() & 1) ? 180.0f : 0.0f;
	else
		p->pose.attackedAtYaw =
			Pose_WrapDegrees(Pose_YawOf(dx, dz, p->yaw) - p->yaw);

	if (flinch) Pose_Hurt(&p->pose, p->pose.attackedAtYaw);
	(void)source;

	/* setHealth clamps to [0, maxHealth] */
	if (p->health < 0.0f) p->health = 0.0f;
	if (p->health > PLAYER_MAX_HEALTH) p->health = PLAYER_MAX_HEALTH;
}

void Player_SetVelocity(Player *p, double mx, double my, double mz) {
	p->motionX = mx;
	p->motionY = my;
	p->motionZ = mz;
	/* Knockback lifts you off the ground; leaving onGround set would let the
	 * next tick's friction eat it before it moved anything. */
	if (my > 0.0) p->onGround = 0;
}

void Player_Teleport(Player *p, double x, double y, double z,
                     float yaw, float pitch) {
	p->x = x; p->y = y; p->z = z;
	p->prevX = x; p->prevY = y; p->prevZ = z;
	p->motionX = p->motionY = p->motionZ = 0.0;
	p->yaw = yaw;
	p->pitch = pitch;
	if (p->pitch >  PITCH_LIMIT) p->pitch =  PITCH_LIMIT;
	if (p->pitch < -PITCH_LIMIT) p->pitch = -PITCH_LIMIT;
	p->fallDistance = 0.0f;
	p->onGround = 0;
	p->isCollidedHorizontally = p->isCollidedVertically = 0;
	Pose_Init(&p->pose, p->yaw, p->pitch);
}

/* EntityLivingBase.fall: distance over 3 blocks turns into half-heart damage. */
static void player_fall(Player *p, float distance) {
	int i = (int)ceil((double)distance - 3.0);   /* ceiling_float_int(distance-3) */
	if (i > 0) Player_Damage(p, (float)i, DMG_FALL, p->x, p->z);
}

/* Entity.updateFallState: accumulate fall height while airborne, cash it in on
 * landing. `dy` is the actual vertical displacement resolved this tick. */
static void updateFallState(Player *p, double dy) {
	if (p->onGround) {
		if (p->fallDistance > 0.0f) {
			if (!p->serverDriven) player_fall(p, p->fallDistance);
			p->fallDistance = 0.0f;
		}
	} else if (dy < 0.0) {
		p->fallDistance -= (float)dy;
	}
}

static void player_respawn(Player *p) {
	p->x = p->spawnX; p->y = p->spawnY; p->z = p->spawnZ;
	p->prevX = p->x; p->prevY = p->y; p->prevZ = p->z;
	p->motionX = p->motionY = p->motionZ = 0.0;
	p->onGround = 0;
	p->health = PLAYER_MAX_HEALTH;
	p->fallDistance = 0.0f;
	p->hurtResistantTime = 0;
	p->lastDamage = 0.0f;
}

/* ---- public ------------------------------------------------------------ */

void Player_Spawn(Player *p, const World *w) {
	p->x = w->spawnx;
	p->y = w->spawny;
	p->z = w->spawnz;
	p->prevX = p->x; p->prevY = p->y; p->prevZ = p->z;
	p->motionX = p->motionY = p->motionZ = 0.0;
	p->yaw = 0.0f;
	p->pitch = 0.0f;
	p->onGround = 0;
	p->isCollidedHorizontally = 0;
	p->isCollidedVertically = 0;
	p->jumpTicks = 0;
	p->sprinting = 0;
	p->sneaking = 0;
	p->inWater = p->inLava = 0;

	p->spawnX = p->x; p->spawnY = p->y; p->spawnZ = p->z;
	p->health = PLAYER_MAX_HEALTH;
	p->fallDistance = 0.0f;
	p->hurtResistantTime = 0;
	p->lastDamage = 0.0f;
	p->serverDriven = 0;
	p->gameMode = 0;
	p->itemInUse = 0;
	p->itemInUseCount = 0;
	Pose_Init(&p->pose, p->yaw, p->pitch);
	Inventory_Init(&p->inventory);
}

void Player_Look(Player *p, float dYaw, float dPitch) {
	p->yaw   += dYaw;
	p->pitch += dPitch;
	if (p->pitch >  PITCH_LIMIT) p->pitch =  PITCH_LIMIT;
	if (p->pitch < -PITCH_LIMIT) p->pitch = -PITCH_LIMIT;
}

void Player_Tick(Player *p, const World *w, const PlayerInput *in) {
	p->prevX = p->x;
	p->prevY = p->y;
	p->prevZ = p->z;

	if (p->jumpTicks > 0) p->jumpTicks--;
	/* EntityLivingBase.onEntityUpdate: tick down the post-hit invuln window. */
	if (p->hurtResistantTime > 0) p->hurtResistantTime--;
	if (p->itemInUseCount > 0) p->itemInUseCount--;

	/* Entity.onEntityUpdate's fluid pass, before movement. Being in one zeroes
	 * the fall distance every tick, which is exactly what makes an MLG water
	 * bucket work: land in the block you just placed and the accumulated fall
	 * never gets cashed in. */
	p->inWater = in_water(p, w);
	p->inLava  = in_lava(p, w);
	if (p->inWater || p->inLava) p->fallDistance = 0.0f;

	float forward = in->moveForward;
	float strafe  = in->moveStrafe;
	int jump  = in->jump;
	int sneak = in->sneak;
	p->sneaking = sneak;

	/* Sprint requires walking forward and not sneaking (EntityPlayerSP). */
	if (in->sprintHeld && forward > 0.0f && !sneak && !p->itemInUse) p->sprinting = 1;
	if (forward <= 0.0f || p->isCollidedHorizontally || sneak) p->sprinting = 0;

	if (sneak) { strafe *= 0.3f; forward *= 0.3f; }
	if (p->itemInUse) { strafe *= ITEM_USE_SLOWDOWN; forward *= ITEM_USE_SLOWDOWN; }

	/* clamp tiny residual motion to zero (EntityLivingBase.onLivingUpdate) */
	if (fabs(p->motionX) < 0.005) p->motionX = 0.0;
	if (fabs(p->motionY) < 0.005) p->motionY = 0.0;
	if (fabs(p->motionZ) < 0.005) p->motionZ = 0.0;

	/* jump handling -- in a liquid it is a steady push up rather than a leap */
	if (jump) {
		if (p->inWater || p->inLava) {
			p->motionY += LIQUID_JUMP;
		} else if (p->onGround && p->jumpTicks == 0) {
			player_jump(p);
			p->jumpTicks = JUMP_COOLDOWN_TICKS;
		}
	} else {
		p->jumpTicks = 0;
	}

	/* moveStrafing/moveForward decay (EntityLivingBase.onLivingUpdate) */
	strafe  *= 0.98f;
	forward *= 0.98f;

	double preY = p->y;
	moveWithHeading(p, w, strafe, forward, sneak);

	/* Entity.updateFallState is called from moveEntity with the resolved
	 * vertical displacement; do it here with the same delta once per tick. */
	updateFallState(p, p->y - preY);

	/* Offline, fall damage is the only way to lose health and a lethal fall
	 * just respawns at the spawn point. In a live game the server owns this
	 * entirely -- it cancels lethal damage and drops you into spectator (T26),
	 * so predicting a respawn here would fight S06 UpdateHealth. */
	if (!p->serverDriven && p->health <= 0.0f) player_respawn(p);

	/* Animation last, off the movement that actually resolved. */
	Pose_Tick(&p->pose, p->x - p->prevX, p->z - p->prevZ, p->yaw, p->pitch);
	p->pose.sneaking  = (u8)sneak;
	p->pose.sprinting = (u8)p->sprinting;
	p->pose.onGround  = (u8)p->onGround;
}

void Player_TickSpectator(Player *p, const PlayerInput *in) {
	p->prevX = p->x;
	p->prevY = p->y;
	p->prevZ = p->z;

	double s = sin(p->yaw * DEG2RAD);
	double c = cos(p->yaw * DEG2RAD);
	double fwd = in->moveForward, str = in->moveStrafe;

	p->motionX = (fwd * (-s) + str * ( c)) * SPECTATOR_SPEED;
	p->motionZ = (fwd * (-c) + str * (-s)) * SPECTATOR_SPEED;
	p->motionY = (in->jump ? SPECTATOR_SPEED : 0.0) -
	             (in->sneak ? SPECTATOR_SPEED : 0.0);

	p->x += p->motionX;
	p->y += p->motionY;
	p->z += p->motionZ;

	/* A spectator is never on the ground and never falling; saying otherwise
	 * would have the server run handleFalling against a body it is not
	 * simulating. */
	p->onGround = 0;
	p->isCollidedHorizontally = p->isCollidedVertically = 0;
	p->fallDistance = 0.0f;
	p->sprinting = 0;
	p->sneaking = 0;
	p->inWater = p->inLava = 0;

	Pose_Tick(&p->pose, p->x - p->prevX, p->z - p->prevZ, p->yaw, p->pitch);
}

void Player_GetViewMatrix(const Player *p, float alpha, Mtx v) {
	double ix = p->prevX + (p->x - p->prevX) * alpha;
	double iy = p->prevY + (p->y - p->prevY) * alpha;
	double iz = p->prevZ + (p->z - p->prevZ) * alpha;

	/* Reuse the camera's tested look-matrix math; eye is in world units. */
	Camera cam;
	Camera_Init(&cam,
	            (float)(ix * WORLD_BLOCK_SIZE),
	            (float)((iy + PLAYER_EYE_HEIGHT) * WORLD_BLOCK_SIZE),
	            (float)(iz * WORLD_BLOCK_SIZE),
	            p->yaw, p->pitch);
	Camera_GetViewMatrix(&cam, v);

	/* EntityRenderer.hurtCameraEffect, applied in eye space so it rolls the
	 * whole view rather than steering the camera: turn the roll axis to face
	 * the attacker, roll, turn back. The curve is sin(f^4 * pi) -- a sharp
	 * lurch that decays over the ten hurt ticks. */
	if (p->pose.hurtTime > 0 && p->pose.maxHurtTime > 0) {
		float f = ((float)p->pose.hurtTime - alpha) / (float)p->pose.maxHurtTime;
		if (f > 0.0f) {
			f = sinf(f * f * f * f * (float)M_PI);
			float a = p->pose.attackedAtYaw;
			Mtx h, r;
			guMtxRotDeg(h, 'y', a);
			guMtxRotDeg(r, 'z', -f * HURT_TILT_DEGREES);
			guMtxConcat(h, r, h);
			guMtxRotDeg(r, 'y', -a);
			guMtxConcat(h, r, h);
			guMtxConcat(h, v, v);
		}
	}
}

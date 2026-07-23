#include <math.h>
#include <gccore.h>

#include "player.h"
#include "camera.h"
#include "world.h"

/* ---- input tuning (controller, not gameplay physics) ------------------ */
#define DEG2RAD        0.017453292519943295f
#define STICK_DEADZONE 12        /* C-stick look deadzone (raw units)       */
#define MOVE_DEADZONE  30        /* main-stick push past this = full +/-1    */
#define LOOK_SPEED     2.2f      /* degrees per frame at full C-stick tilt  */
#define PITCH_LIMIT    89.0f
#define SPRINT_TRIGGER 100       /* analog R threshold to count as sprint   */

/* ---- Minecraft 1.8.9 physics constants (block units, per 20 Hz tick) --- */
#define PLAYER_WIDTH        0.6
#define PLAYER_HEIGHT       1.8
#define PLAYER_EYE_HEIGHT   1.62
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

/* moveFlying's magic constant is exactly (0.6*0.91)^3, so on default ground
 * the acceleration factor 0.16277136/f4^3 reduces to 1.0. */
#define MOVE_FLYING_CONST   0.16277136

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
 * box intersects `b`. Used by the sneak ledge guard. */
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

/* EntityLivingBase.moveEntityWithHeading (ground/air branch only). */
static void moveWithHeading(Player *p, const World *w,
                            double strafe, double forward, int sneaking) {
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

/* ---- input ------------------------------------------------------------ */
static float deadzone(int raw) {
	if (raw >  STICK_DEADZONE) return (float)(raw - STICK_DEADZONE) / (127 - STICK_DEADZONE);
	if (raw < -STICK_DEADZONE) return (float)(raw + STICK_DEADZONE) / (127 - STICK_DEADZONE);
	return 0.0f;
}

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
}

void Player_Look(Player *p, int chan) {
	p->yaw   -= deadzone(PAD_SubStickX(chan)) * LOOK_SPEED;
	p->pitch += deadzone(PAD_SubStickY(chan)) * LOOK_SPEED;
	if (p->pitch >  PITCH_LIMIT) p->pitch =  PITCH_LIMIT;
	if (p->pitch < -PITCH_LIMIT) p->pitch = -PITCH_LIMIT;
}

void Player_Tick(Player *p, const World *w, int chan) {
	p->prevX = p->x;
	p->prevY = p->y;
	p->prevZ = p->z;

	if (p->jumpTicks > 0) p->jumpTicks--;

	/* Minecraft's MovementInputFromOptions maps the WASD keys to moveForward/
	 * moveStrafe of exactly +/-1 (keyboard input is binary). The GameCube
	 * analog stick can't reach magnitude 1.0 at full deflection -- physically
	 * it tops out near raw +/-100, well under the +/-127 range -- so a naive
	 * normalisation caps speed below Minecraft's and never satisfies the
	 * sprint test. We therefore digitise it: past the deadzone counts as a
	 * full +/-1 in that axis, giving true Minecraft walk speed and diagonals. */
	int rawX = PAD_StickX(chan);
	int rawY = PAD_StickY(chan);
	float forward = (rawY >  MOVE_DEADZONE) ?  1.0f
	              : (rawY < -MOVE_DEADZONE) ? -1.0f : 0.0f;   /* up = +forward   */
	float strafe  = (rawX >  MOVE_DEADZONE) ?  1.0f
	              : (rawX < -MOVE_DEADZONE) ? -1.0f : 0.0f;   /* right = +strafe */

	u32 held = PAD_ButtonsHeld(chan);
	int jump  = (held & PAD_BUTTON_A) != 0;
	int sneak = (held & PAD_BUTTON_B) != 0;
	/* sprint on R: the digital click or the analog trigger past a threshold */
	int sprintHeld = (held & PAD_TRIGGER_R) != 0 || PAD_TriggerR(chan) > SPRINT_TRIGGER;

	/* Sprint requires walking forward and not sneaking (EntityPlayerSP). */
	if (sprintHeld && forward > 0.0f && !sneak) p->sprinting = 1;
	if (forward <= 0.0f || p->isCollidedHorizontally || sneak) p->sprinting = 0;

	if (sneak) { strafe *= 0.3f; forward *= 0.3f; }

	/* clamp tiny residual motion to zero (EntityLivingBase.onLivingUpdate) */
	if (fabs(p->motionX) < 0.005) p->motionX = 0.0;
	if (fabs(p->motionY) < 0.005) p->motionY = 0.0;
	if (fabs(p->motionZ) < 0.005) p->motionZ = 0.0;

	/* jump handling */
	if (jump) {
		if (p->onGround && p->jumpTicks == 0) {
			player_jump(p);
			p->jumpTicks = JUMP_COOLDOWN_TICKS;
		}
	} else {
		p->jumpTicks = 0;
	}

	/* moveStrafing/moveForward decay (EntityLivingBase.onLivingUpdate) */
	strafe  *= 0.98f;
	forward *= 0.98f;

	moveWithHeading(p, w, strafe, forward, sneak);
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
}

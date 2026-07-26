#include <math.h>
#include <string.h>

#include "pose.h"

#define DEG2RAD 0.017453292519943295
#define RAD2DEG 57.29577951308232

/* EntityLivingBase.onLivingUpdate's threshold for "is this entity actually
 * going anywhere": the squared horizontal displacement below which the body
 * keeps whatever facing it had rather than snapping to the noise in a
 * sub-pixel position update. Vanilla's literal. */
#define MOVING_EPSILON_SQ 0.0025000002f

/* updateDistance's head/body clamp. The head may lead the body by this many
 * degrees; past it the body is dragged round to follow. */
#define HEAD_YAW_LIMIT 75.0f

/* How fast the body catches up with the direction of travel, per tick. */
#define BODY_TURN_RATE 0.3f

float Pose_WrapDegrees(float a) {
	a = fmodf(a, 360.0f);
	if (a >= 180.0f) a -= 360.0f;
	if (a < -180.0f) a += 360.0f;
	return a;
}

float Pose_LerpAngle(float prev, float cur, float alpha) {
	return prev + Pose_WrapDegrees(cur - prev) * alpha;
}

float Pose_YawOf(double dx, double dz, float fallback) {
	if (dx * dx + dz * dz < 1e-9) return fallback;
	/* This engine's forward for yaw t is (-sin t, -cos t) -- see camera.c and
	 * player.c's moveFlying -- so the yaw facing (dx, dz) is atan2(-dx, -dz).
	 * Vanilla's own atan2(dz, dx) - 90 is the same identity written in
	 * Minecraft's yaw convention, which runs the other way; converting the
	 * angle at the packet boundary and then using vanilla's formula here would
	 * mirror every entity's facing. */
	return (float)(atan2(-dx, -dz) * RAD2DEG);
}

void Pose_Init(Pose *p, float yaw, float pitch) {
	memset(p, 0, sizeof(*p));
	p->headYaw = p->prevHeadYaw = yaw;
	p->renderYawOffset = p->prevRenderYawOffset = yaw;
	p->pitch = p->prevPitch = pitch;
	p->maxHurtTime = POSE_HURT_TICKS;
}

/* EntityLivingBase.updateDistance: turn the body toward `travelYaw`, then pull
 * it the rest of the way if the head has ended up more than 75 degrees off it.
 * The result is that a player walking sideways keeps facing you until their
 * neck runs out, and only then does the whole body swing round. */
static void update_distance(Pose *p, float travelYaw) {
	float d = Pose_WrapDegrees(travelYaw - p->renderYawOffset);
	p->renderYawOffset += d * BODY_TURN_RATE;

	float off = Pose_WrapDegrees(p->headYaw - p->renderYawOffset);
	if (off < -HEAD_YAW_LIMIT) off = -HEAD_YAW_LIMIT;
	if (off >  HEAD_YAW_LIMIT) off =  HEAD_YAW_LIMIT;
	p->renderYawOffset = p->headYaw - off;
	/* Past ~50 degrees vanilla adds a nudge on top, so a body that is being
	 * dragged round by the head keeps moving instead of sitting on the clamp. */
	if (off * off > 2500.0f) p->renderYawOffset += off * 0.2f;
}

void Pose_Tick(Pose *p, double dx, double dz, float yaw, float pitch) {
	p->prevLimbSwingAmount   = p->limbSwingAmount;
	p->prevRenderYawOffset   = p->renderYawOffset;
	p->prevHeadYaw           = p->headYaw;
	p->prevPitch             = p->pitch;
	p->prevSwingProgress     = p->swingProgress;

	p->headYaw = yaw;
	p->pitch   = pitch;

	/* Limb swing: amplitude chases the entity's actual speed (x4, saturating
	 * at a walk) and the phase advances by the amplitude, so a stationary
	 * entity's legs stop where they are rather than drifting. */
	float speed = (float)sqrt(dx * dx + dz * dz) * 4.0f;
	if (speed > 1.0f) speed = 1.0f;
	p->limbSwingAmount += (speed - p->limbSwingAmount) * 0.4f;
	p->limbSwing       += p->limbSwingAmount;

	/* Which way the body points. Direction of travel, unless the entity is
	 * standing still (keep the last body angle) or mid-swing (vanilla snaps
	 * the body to the head, so a hit lands facing the target). */
	float travelYaw = p->renderYawOffset;
	if ((float)(dx * dx + dz * dz) > MOVING_EPSILON_SQ)
		travelYaw = Pose_YawOf(dx, dz, p->renderYawOffset);
	if (p->isSwingInProgress) travelYaw = p->headYaw;
	update_distance(p, travelYaw);

	/* updateArmSwingProgress. swingProgressInt starts at -1 so the first tick
	 * of a swing lands on 0, matching swingItem's reset. */
	if (p->isSwingInProgress) {
		p->swingProgressInt++;
		if (p->swingProgressInt >= POSE_SWING_TICKS) {
			p->swingProgressInt  = 0;
			p->isSwingInProgress = 0;
		}
	} else {
		p->swingProgressInt = 0;
	}
	p->swingProgress = (float)p->swingProgressInt / (float)POSE_SWING_TICKS;

	if (p->hurtTime > 0) p->hurtTime--;
}

void Pose_Swing(Pose *p) {
	/* A swing already past halfway may be restarted -- that is what makes
	 * held-button mining look continuous rather than stuttering on a reset. */
	if (!p->isSwingInProgress ||
	    p->swingProgressInt >= POSE_SWING_TICKS / 2 ||
	    p->swingProgressInt < 0) {
		p->swingProgressInt  = -1;
		p->isSwingInProgress = 1;
	}
}

void Pose_Hurt(Pose *p, float attackerRelYaw) {
	p->hurtTime = p->maxHurtTime = POSE_HURT_TICKS;
	p->attackedAtYaw = attackerRelYaw;
}

float Pose_SwingProgress(const Pose *p, float alpha) {
	float d = p->swingProgress - p->prevSwingProgress;
	if (d < 0.0f) d += 1.0f;    /* wrapped past the end of a swing */
	return p->prevSwingProgress + d * alpha;
}

void Pose_LimbSwing(const Pose *p, float alpha, float *swing, float *amount) {
	float amt = p->prevLimbSwingAmount +
	            (p->limbSwingAmount - p->prevLimbSwingAmount) * alpha;
	/* limbSwing has already been advanced by a whole tick's worth, so the
	 * interpolated phase is found by rewinding the unspent fraction rather
	 * than by keeping a previous value (RendererLivingEntity.doRender). */
	*swing  = p->limbSwing - p->limbSwingAmount * (1.0f - alpha);
	*amount = amt;
}

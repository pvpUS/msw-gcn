#ifndef MSW_POSE_H
#define MSW_POSE_H

#include <gccore.h>

/* Pose -- the RendererLivingEntity-facing slice of EntityLivingBase.
 *
 * Everything a humanoid needs in order to be *drawn* moving: which way the
 * body is turned as against the head, how far through a walk cycle the limbs
 * are, whether an arm is mid-swing, and whether the thing was hit recently.
 * None of it is physics; all of it is state the renderer reads.
 *
 * It lives in its own module because two very different things need exactly
 * the same block of it. A remote player (entity.c) has its animation driven by
 * positions arriving over the network, and the local player (player.c) has its
 * own driven by the pad -- but `ModelBiped.setRotationAngles` does not care
 * which, and neither should the code that feeds it.
 *
 * **Scope note.** This is T17's shared core, built now because T9's entity
 * models cannot animate without it. T17's remaining half -- embedding a Pose in
 * `Player`, and moving helditem.c's file-static bob phase onto it to drive
 * vanilla's `transformFirstPersonItem` arc -- waits on T16's input struct, and
 * is deliberately not done here: nothing in the local player's path reads a
 * Pose yet, so adding the member early would only be a field nobody ticks.
 *
 * Angles are degrees in **this engine's** convention throughout (yaw 0 faces
 * -Z, positive pitch looks up), because that is what arrives from the proxy
 * and what the view matrix uses. The conversion from Minecraft's happens once,
 * at the packet boundary, and never again.
 */

/* EntityLivingBase.getArmSwingAnimationEnd() with no haste: an arm swing runs
 * 6 ticks, which at 20 Hz is the 300 ms every 1.8 melee animation is timed
 * against. */
#define POSE_SWING_TICKS 6

/* Entity.hurtTime / maxHurtTime on taking damage. Ten ticks of red flash on a
 * remote entity, and ten ticks of camera tilt on the local player (T19). */
#define POSE_HURT_TICKS 10

typedef struct {
	/* Walk cycle. `limbSwing` is a free-running phase accumulated at the rate
	 * the entity is actually moving, and `limbSwingAmount` is how far the
	 * limbs swing -- 0 standing still, 1 at a sprint. Both are read
	 * interpolated at render time; see Pose_LimbSwing. */
	float limbSwing, limbSwingAmount, prevLimbSwingAmount;

	/* Arm swing. The int is the authoritative 0..POSE_SWING_TICKS counter;
	 * the two floats are it normalised to 0..1 for this tick and the last, so
	 * the render can interpolate across a tick boundary. */
	int   swingProgressInt;
	float swingProgress, prevSwingProgress;
	u8    isSwingInProgress;

	/* Damage feedback. hurtTime counts down from maxHurtTime; attackedAtYaw is
	 * the direction the hit came from, *relative to the entity's own yaw*,
	 * which is what the camera tilt rotates about. */
	int   hurtTime, maxHurtTime;
	float attackedAtYaw;

	/* Body vs head. The wire carries one yaw per entity, so the body angle is
	 * derived from which way the entity is travelling and the head is allowed
	 * to lead it by up to 75 degrees -- exactly what vanilla does, and the
	 * reason a player strafing past you still looks at you. */
	float renderYawOffset, prevRenderYawOffset;
	float headYaw, prevHeadYaw;
	float pitch, prevPitch;

	u8    sneaking, sprinting, onGround;
} Pose;

/* Zero a Pose and point the body and head at `yaw`, so a freshly spawned
 * entity does not spend its first tick swinging round from zero. */
void Pose_Init(Pose *p, float yaw, float pitch);

/* One 20 Hz tick. `dx`/`dz` are this tick's horizontal displacement in blocks
 * and `yaw`/`pitch` the entity's current look angles; everything else follows
 * from them.
 *
 * This is EntityLivingBase.onLivingUpdate's animation half plus updateDistance,
 * in that order: save the previous values, fold the movement into the limb
 * swing, turn the body toward the direction of travel and clamp the head
 * against it, then advance the arm swing and tick down the hurt timer. */
void Pose_Tick(Pose *p, double dx, double dz, float yaw, float pitch);

/* EntityLivingBase.swingItem: start an arm swing, or restart one that is past
 * halfway. Called on the local player when they attack and on a remote entity
 * when ENTITY_ANIM(SWING) arrives. */
void Pose_Swing(Pose *p);

/* The animation half of attackEntityFrom: 10 ticks of hurt, recorded as coming
 * from `attackerRelYaw` degrees off the entity's own facing. */
void Pose_Hurt(Pose *p, float attackerRelYaw);

/* ---- render-time reads, all interpolated by `alpha` in [0,1] ------------- */

/* EntityLivingBase.getSwingProgress: 0 at rest, sweeping 0->1 over the six
 * ticks of a swing. Drives both the third-person arm arc and (T17) the
 * first-person item arc. */
float Pose_SwingProgress(const Pose *p, float alpha);

/* The two limb-swing values ModelBiped.setRotationAngles wants. `swing` is the
 * phase and `amount` the amplitude; vanilla derives the phase by rewinding
 * rather than by keeping a previous value, which is why this is one call. */
void  Pose_LimbSwing(const Pose *p, float alpha, float *swing, float *amount);

/* Shortest-path angle interpolation (RendererLivingEntity.interpolateRotation),
 * so a body crossing +/-180 does not spin the long way round. */
float Pose_LerpAngle(float prev, float cur, float alpha);

/* Wrap to (-180, 180]. Exposed because callers that build their own angles --
 * the nametag pass, the hurt-tilt -- need the same wrap. */
float Pose_WrapDegrees(float a);

/* The engine yaw that faces along the horizontal direction (dx, dz), i.e. the
 * inverse of the (-sin yaw, -cos yaw) forward vector player.c and camera.c
 * use. Returns `fallback` when the vector is too short to have a direction. */
float Pose_YawOf(double dx, double dz, float fallback);

#endif

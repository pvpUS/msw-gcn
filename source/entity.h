#ifndef MSW_ENTITY_H
#define MSW_ENTITY_H

#include <gccore.h>
#include "pose.h"
#include "world.h"
#include "hud.h"      /* HudTag, for the nametag pass */
#include "gclink.h"   /* GCLINK_ENT_*, GCLINK_EFLAG_*, GCLINK_ANIM_* */

/* The remote entity table -- everything on screen that the console does not
 * simulate. Players, the ender dragon, dropped items and the five projectile
 * types the proxy lets through; nothing else ever arrives (entities.js filters
 * cosmetic mobs, armor stands and particle trails before they take a slot).
 *
 * The console owns no entity logic at all. There is no AI, no collision and no
 * physics here: positions arrive over GCLink at 20 Hz and the only work done
 * locally is smoothing them out and animating what that motion implies. That is
 * the whole point of the split -- a GameCube can afford to *draw* sixteen
 * players fighting, and cannot afford to work out what they are doing.
 */

/* 128 slots, not 64: sixteen players, their arrows and snowballs, and one
 * 192-stone death drop overflow 64 during a real fight. The proxy applies the
 * same cap with nearest-first eviction, so this array is the console's half of
 * an agreement rather than an independent limit -- see entities.js. */
#define ENTITY_MAX 128

/* Names are capped at 24 characters on the wire (GC_S_ENTITY_ADD), which is
 * comfortably past Minecraft's own 16-character limit even after a team
 * prefix has been stripped. */
#define ENTITY_NAME_MAX 25

/* EntityOtherPlayerMP.setPositionAndRotation2's posRotationIncrements. A
 * position update is walked in over three ticks rather than applied outright:
 * the wire's fixed-point is 1/32 of a block and the proxy drops sub-pixel
 * deltas, so applied raw an entity standing still visibly buzzes. The cost is
 * ~150 ms of smoothing lag on something the console cannot interact with
 * anyway (attacks are server-authoritative -- see T19). */
#define ENTITY_LERP_TICKS 3

typedef struct {
	s32   eid;
	u8    type;     /* GCLINK_ENT_*                                        */
	u8    flags;    /* GCLINK_EFLAG_*                                      */
	u8    alive;
	u8    colour;   /* team colour, 0-15 as an MC colour code, 0xFF = none */

	/* Local block units -- the same space World_GetBlock takes. The proxy has
	 * already subtracted the map origin, so the console never sees an absolute
	 * server coordinate. prev/current are a tick apart and rendering
	 * interpolates between them with the same alpha the player's view uses. */
	double x, y, z;
	double prevX, prevY, prevZ;
	float  yaw, pitch;          /* engine convention: yaw 0 faces -Z, pitch up */

	/* Where the server last said it is, and how many ticks are left to get
	 * there. See ENTITY_LERP_TICKS. */
	double tx, ty, tz;
	float  tyaw, tpitch;
	int    lerpTicks;

	int   held;                  /* engine item id in the hand, -1 = empty   */
	char  name[ENTITY_NAME_MAX];
	Pose  pose;
	u32   age;                   /* ticks alive, for item bob/spin and wings */
} Entity;

/* Open-addressed eid -> slot index. Sized well past ENTITY_MAX so lookups stay
 * near one probe; ENTITY_HASH must be a power of two. */
#define ENTITY_HASH 256

typedef struct {
	Entity e[ENTITY_MAX];
	s16    map[ENTITY_HASH];   /* slot index, -1 empty, -2 tombstone */
	int    live;               /* occupied slots                     */
	int    used;               /* live + tombstones, for the rebuild */
	s32    selfEid;            /* never drawn; -1 until MAP_SELECT   */
} EntityWorld;

/* ---- table -------------------------------------------------------------- */

void Entity_WorldInit(EntityWorld *ew);
void Entity_WorldClear(EntityWorld *ew);          /* map change / disconnect */
void Entity_SetSelf(EntityWorld *ew, s32 eid);

Entity *Entity_Find(const EntityWorld *ew, s32 eid);

/* GC_S_ENTITY_ADD: take (or reuse) a slot for `eid`. Returns NULL only when
 * the table is full, which the proxy's own cap should make unreachable --
 * a NULL here means the two ends disagree about the cap, so it is dropped
 * rather than papered over by evicting something the proxy still tracks. */
Entity *Entity_Add(EntityWorld *ew, s32 eid, u8 type);

void Entity_Remove(EntityWorld *ew, s32 eid);

/* GC_S_ENTITY_MOVE: the server's position for this entity, walked in over
 * ENTITY_LERP_TICKS. Angles are already in the engine's convention. */
void Entity_MoveTo(Entity *e, double x, double y, double z,
                   float yaw, float pitch);

/* Snap outright, with no smoothing -- for a spawn, where there is no previous
 * position to interpolate from. */
void Entity_SetPos(Entity *e, double x, double y, double z,
                   float yaw, float pitch);

/* GC_S_ENTITY_ANIM. */
void Entity_Anim(Entity *e, u8 anim);

/* One 20 Hz tick for the whole table: step positions toward their targets and
 * run each entity's Pose. Call from the same accumulator loop as Player_Tick. */
void Entity_TickAll(EntityWorld *ew);

u32  Entity_Count(const EntityWorld *ew);

/* ---- targeting (T18) ----------------------------------------------------
 * Minecraft.getMouseOver's entity pass. The block trace runs first and
 * unchanged; this then asks whether anything hittable lies in front of
 * whatever the blocks stopped at.
 *
 * Reach is deliberately shorter than the block reach -- 3.0 against 4.5 -- and
 * the two are separate numbers in vanilla for a reason: a crosshair that
 * targets a player five blocks away sends an attack the server rejects. */
#define REACH_ENTITY 3.0

typedef struct {
	int    slot;             /* index into EntityWorld.e[]               */
	s32    eid;
	double t;                /* distance along the look vector, blocks   */
	double hx, hy, hz;       /* the intercept, in local block units      */
} EntityHit;

/* getCollisionBorderSize: every entity's box grows by this per side for
 * picking, so a player's 0.6 x 1.8 is targeted as 0.8 x 2.0. */
#define ENTITY_PICK_EXPAND 0.1

/* Trace from (ex,ey,ez) along the unit vector (lx,ly,lz) out to `maxDist`.
 * `blockT` is how far away the block trace hit (or maxDist when it missed) --
 * an entity only wins if it is strictly closer, which is what makes a player
 * standing against a wall targetable and one standing behind it not.
 *
 * Only entities the server will accept an attack against are considered:
 * dropped items, xp orbs, arrows, snowballs, pearls, potions and fishing
 * bobbers are never targets, because C02 ATTACK against one of them is a
 * *kick* ("Attempting to attack an invalid entity"), not a no-op.
 *
 * Returns 1 and fills `out` on a hit. */
int Entity_RayTrace(const EntityWorld *ew, double ex, double ey, double ez,
                    double lx, double ly, double lz,
                    double maxDist, double blockT, s32 excludeEid,
                    EntityHit *out);

/* ---- rendering ---------------------------------------------------------- */

/* One-time GX setup: the entity vertex format (GX_VTXFMT3 -- 0/1/2 are world,
 * HUD and helditem) and the two skin textures. Call once after World_InitGX. */
void Entity_InitGX(void);

/* Draw every live entity. `view` is the camera view matrix and `alpha` the
 * inter-tick fraction, exactly as passed to ItemWorld_Draw. Leaves the
 * pipeline restored to World_SetupRenderState. */
void Entity_Draw(const EntityWorld *ew, Mtx view, float alpha);

/* Project the nearest players' nametags into gui space, ready for
 * Hud_DrawTags. `view`/`proj` are the matrices the world was drawn with and
 * (eyeX,eyeY,eyeZ) the camera position in block units; `w`, when non-NULL, is
 * used to drop tags whose owner is behind terrain.
 *
 * Returns how many entries of `out` were filled. Tags past ENTITY_TAG_DISTANCE
 * are dropped outright and the rest are capped to the nearest `max` -- this is
 * the frame budget's main lever, so the culling is not optional. */
#define ENTITY_TAG_DISTANCE 48.0

int Entity_CollectTags(const EntityWorld *ew, const World *w,
                       Mtx view, Mtx44 proj,
                       double eyeX, double eyeY, double eyeZ,
                       int fbWidth, int efbHeight,
                       HudTag *out, int max);

#endif

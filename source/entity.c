#include <math.h>
#include <string.h>
#include <gccore.h>
#include <ogc/tpl.h>

#include "entity.h"
#include "helditem.h"          /* HeldItem_SetupGX / *_DrawBlockMesh / *_DrawFlatMesh */
#include "block_faces_gen.h"   /* NUM_BLOCK_IDS */
#include "block_book_gen.h"    /* ITEM_SNOWBALL_TILE */
#include "entity_tex_gen.h"    /* STEVE_TEX_W/H, DRAGON_TEX_W/H */
#include "steve_tpl.h"
#include "dragon_tpl.h"

/* Formats 0/1/2 are world / HUD / helditem; this is the fourth and last one
 * this engine uses. Same layout as helditem's -- float positions, RGBA8 vertex
 * colour, float texcoords -- but bound to a skin sheet rather than the block
 * atlas, which is the whole reason it cannot share format 2. */
#define ENT_FMT GX_VTXFMT3

#define DEG2RAD 0.017453292519943295f
#define PI_F    3.14159265358979323846f

/* ---- Minecraft model space ----------------------------------------------
 * Every box below is transcribed verbatim from MCP-919's ModelBiped and
 * ModelDragon, which means model units (1/16 of a block) with **+Y pointing
 * down** and the model's face on -Z. Converting that to this engine's world
 * is exactly what RenderLivingBase does:
 *
 *     translate(entity position)
 *     rotate(180 - renderYawOffset)   about +Y
 *     scale(-1, -1, 1)                MC model axes -> world axes
 *     scale(0.9375)                   RenderPlayer.preRenderCallback
 *     translate(0, -1.5078125, 0)     model root -> entity origin
 *
 * Two of those collapse into constants. `MODEL_UNIT` is 0.0625 * 0.9375, the
 * blocks a model unit is worth, and `MODEL_ROOT_Y` is 1.5078125 * 0.9375, how
 * far above the feet the model's origin sits -- which is why a player model is
 * 1.88 blocks tall against a 1.8-block hitbox, in this engine exactly as in
 * Minecraft.
 *
 * The rotation is *not* `180 - yaw`: the proxy already converted at the packet
 * boundary, and 180 - mc_yaw **is** the engine yaw, so the model rotates by the
 * entity's engine yaw directly. See pose.c's Pose_YawOf for the same identity
 * from the other side.
 */
#define MODEL_UNIT   0.05859375f
#define MODEL_ROOT_Y 1.41357421875f

/* The dragon is not a player: no 0.9375 shrink, so its units and root are
 * vanilla's unscaled ones. */
#define DRAGON_UNIT   0.0625f
#define DRAGON_ROOT_Y 1.5078125f

/* One box of a Minecraft model: minimum corner and size in model units, plus
 * the pixel offset its unwrap starts at in the sheet. `mirror` swaps the two X
 * faces, which is how vanilla builds a left arm out of a right arm's texture. */
typedef struct {
	float x, y, z;
	float dx, dy, dz;
	short tu, tv;
	u8    mirror;
} MBox;

/* A box's parent joint: where it pivots and how far it has turned. */
typedef struct {
	float rpx, rpy, rpz;
	float rx, ry, rz;      /* radians, applied Z then Y then X */
} MPart;

/* Directional shade per face, matching world.c's so a player standing on the
 * terrain is lit like it. The index is the *world* face after the (-1,-1,1)
 * conversion has flipped MC's Y: quad 2 is MC's -Y, which is the world's top.
 * Shading is baked pre-rotation, as the held-item cube's already is. */
static const u8 g_faceShade[6] = { 153, 153, 255, 128, 204, 204 };

/* ---- textures ----------------------------------------------------------- */
static TPLFile  steveTPL, dragonTPL;
static GXTexObj steveTex, dragonTex;

void Entity_InitGX(void) {
	GX_SetVtxAttrFmt(ENT_FMT, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
	GX_SetVtxAttrFmt(ENT_FMT, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(ENT_FMT, GX_VA_TEX0, GX_TEX_ST,   GX_F32,   0);

	/* Point sampling and no mipmaps, as vanilla ships these: a skin is 16x16
	 * pixel art per face and any filtering just smears it. */
	TPL_OpenTPLFromMemory(&steveTPL, (void *)steve_tpl, steve_tpl_size);
	TPL_GetTexture(&steveTPL, 0, &steveTex);
	GX_InitTexObjFilterMode(&steveTex, GX_NEAR, GX_NEAR);
	GX_InitTexObjWrapMode(&steveTex, GX_CLAMP, GX_CLAMP);

	TPL_OpenTPLFromMemory(&dragonTPL, (void *)dragon_tpl, dragon_tpl_size);
	TPL_GetTexture(&dragonTPL, 0, &dragonTex);
	GX_InitTexObjFilterMode(&dragonTex, GX_NEAR, GX_NEAR);
	GX_InitTexObjWrapMode(&dragonTex, GX_CLAMP, GX_CLAMP);
}

/* ---- the table ----------------------------------------------------------
 * Open addressing with tombstones, rebuilt from the entity array once the
 * probe chains get long. A linear scan of 128 slots would honestly be fast
 * enough, but ENTITY_MOVE is a batch of up to 128 lookups arriving 20 times a
 * second and this keeps that at one probe apiece. */

#define HASH_EMPTY (-1)
#define HASH_DEAD  (-2)
#define HASH_REBUILD_AT (ENTITY_HASH * 3 / 4)

static inline u32 hash_eid(s32 eid) {
	/* Knuth multiplicative -- entity ids from a Spigot server are a dense
	 * ascending run, which the low bits alone would scatter poorly across a
	 * table this size once the run wraps a game boundary. */
	return ((u32)eid * 2654435761u) & (ENTITY_HASH - 1);
}

static void hash_rebuild(EntityWorld *ew) {
	int i;
	for (i = 0; i < ENTITY_HASH; i++) ew->map[i] = HASH_EMPTY;
	ew->used = 0;
	for (i = 0; i < ENTITY_MAX; i++) {
		if (!ew->e[i].alive) continue;
		u32 h = hash_eid(ew->e[i].eid);
		while (ew->map[h] != HASH_EMPTY) h = (h + 1) & (ENTITY_HASH - 1);
		ew->map[h] = (s16)i;
		ew->used++;
	}
}

static void hash_put(EntityWorld *ew, s32 eid, int slot) {
	if (ew->used >= HASH_REBUILD_AT) hash_rebuild(ew);
	u32 h = hash_eid(eid);
	while (ew->map[h] >= 0) h = (h + 1) & (ENTITY_HASH - 1);
	if (ew->map[h] == HASH_EMPTY) ew->used++;
	ew->map[h] = (s16)slot;
}

static int hash_get(const EntityWorld *ew, s32 eid) {
	u32 h = hash_eid(eid);
	int probes = ENTITY_HASH;
	while (probes--) {
		s16 s = ew->map[h];
		if (s == HASH_EMPTY) return -1;
		if (s >= 0 && ew->e[s].alive && ew->e[s].eid == eid) return s;
		h = (h + 1) & (ENTITY_HASH - 1);
	}
	return -1;
}

static void hash_del(EntityWorld *ew, s32 eid) {
	u32 h = hash_eid(eid);
	int probes = ENTITY_HASH;
	while (probes--) {
		s16 s = ew->map[h];
		if (s == HASH_EMPTY) return;
		if (s >= 0 && ew->e[s].eid == eid) { ew->map[h] = HASH_DEAD; return; }
		h = (h + 1) & (ENTITY_HASH - 1);
	}
}

void Entity_WorldInit(EntityWorld *ew) {
	memset(ew, 0, sizeof(*ew));
	ew->selfEid = -1;
	hash_rebuild(ew);
}

void Entity_WorldClear(EntityWorld *ew) {
	s32 self = ew->selfEid;
	memset(ew->e, 0, sizeof(ew->e));
	ew->live = 0;
	hash_rebuild(ew);
	ew->selfEid = self;
}

void Entity_SetSelf(EntityWorld *ew, s32 eid) { ew->selfEid = eid; }

Entity *Entity_Find(const EntityWorld *ew, s32 eid) {
	int s = hash_get(ew, eid);
	return (s < 0) ? NULL : (Entity *)&ew->e[s];
}

Entity *Entity_Add(EntityWorld *ew, s32 eid, u8 type) {
	if (eid == ew->selfEid) return NULL;   /* the console is not an entity */

	int s = hash_get(ew, eid);
	if (s < 0) {
		for (s = 0; s < ENTITY_MAX; s++) if (!ew->e[s].alive) break;
		/* Full. The proxy caps at the same 128 with nearest-first eviction, so
		 * this can only mean the two ends have drifted apart -- dropping the
		 * add says so on the next perf-overlay glance, where evicting a slot
		 * the proxy still believes in would quietly desync the table. */
		if (s == ENTITY_MAX) return NULL;
		memset(&ew->e[s], 0, sizeof(Entity));
		hash_put(ew, eid, s);
		ew->live++;
	}

	Entity *e = &ew->e[s];
	e->eid   = eid;
	e->type  = type;
	e->alive = 1;
	e->held  = -1;
	e->colour = 0xFF;
	return e;
}

void Entity_Remove(EntityWorld *ew, s32 eid) {
	int s = hash_get(ew, eid);
	if (s < 0) return;
	ew->e[s].alive = 0;
	ew->live--;
	hash_del(ew, eid);
}

u32 Entity_Count(const EntityWorld *ew) { return (u32)ew->live; }

void Entity_SetPos(Entity *e, double x, double y, double z,
                   float yaw, float pitch) {
	e->x = e->prevX = e->tx = x;
	e->y = e->prevY = e->ty = y;
	e->z = e->prevZ = e->tz = z;
	e->yaw = e->tyaw = yaw;
	e->pitch = e->tpitch = pitch;
	e->lerpTicks = 0;
	Pose_Init(&e->pose, yaw, pitch);
}

void Entity_MoveTo(Entity *e, double x, double y, double z,
                   float yaw, float pitch) {
	e->tx = x; e->ty = y; e->tz = z;
	e->tyaw = yaw; e->tpitch = pitch;
	/* Zero ticks means the next Entity_TickAll assigns the target outright,
	 * which is what vanilla's base Entity does -- see gclink_ent_smoothed. */
	e->lerpTicks = gclink_ent_smoothed(e->type) ? ENTITY_LERP_TICKS : 0;
}

void Entity_Anim(Entity *e, u8 anim) {
	switch (anim) {
	case GCLINK_ANIM_SWING: Pose_Swing(&e->pose); break;
	/* No attacker direction on the wire -- the flash is what carries combat
	 * legibility (T19), and which way the model recoils is not. */
	case GCLINK_ANIM_HURT:  Pose_Hurt(&e->pose, 0.0f); break;
	case GCLINK_ANIM_DEATH: Pose_Hurt(&e->pose, 0.0f); break;
	default: break;
	}
}

void Entity_TickAll(EntityWorld *ew) {
	int i;
	for (i = 0; i < ENTITY_MAX; i++) {
		Entity *e = &ew->e[i];
		if (!e->alive) continue;

		e->prevX = e->x; e->prevY = e->y; e->prevZ = e->z;

		if (e->lerpTicks > 0) {
			/* EntityOtherPlayerMP.onUpdate: close a fraction of the remaining
			 * gap each tick rather than jumping, so a 20 Hz position stream
			 * arriving out of phase with the console's tick does not buzz.
			 * Players and the dragon only -- a projectile takes the else
			 * branch, because easing something that moves 3 blocks a tick
			 * leaves it trailing rather than smooth. */
			double n = (double)e->lerpTicks;
			e->x += (e->tx - e->x) / n;
			e->y += (e->ty - e->y) / n;
			e->z += (e->tz - e->z) / n;
			e->yaw   += Pose_WrapDegrees(e->tyaw - e->yaw) / (float)n;
			e->pitch += (e->tpitch - e->pitch) / (float)n;
			e->lerpTicks--;
		} else {
			e->x = e->tx; e->y = e->ty; e->z = e->tz;
			e->yaw = e->tyaw; e->pitch = e->tpitch;
		}

		e->pose.sneaking  = (e->flags & GCLINK_EFLAG_SNEAKING)  != 0;
		e->pose.sprinting = (e->flags & GCLINK_EFLAG_SPRINTING) != 0;
		Pose_Tick(&e->pose, e->x - e->prevX, e->z - e->prevZ, e->yaw, e->pitch);
		e->age++;
	}
}

/* ---- geometry ----------------------------------------------------------- */

/* One textured cuboid, as ModelBox unwraps it. The six quads and their UV
 * rects are vanilla's, in vanilla's order:
 *   0 +X   1 -X   2 -Y (world top)   3 +Y (world bottom)   4 -Z (face)   5 +Z
 * and within each quad the corner order is (uMax,vMin) (uMin,vMin)
 * (uMin,vMax) (uMax,vMax), which is TexturedQuad's constructor. */
static void emit_box(const MBox *b, float texW, float texH,
                     u8 tintR, u8 tintG, u8 tintB) {
	float x0 = b->x, x1 = b->x + b->dx;
	if (b->mirror) { float t = x0; x0 = x1; x1 = t; }
	float y0 = b->y, y1 = b->y + b->dy;
	float z0 = b->z, z1 = b->z + b->dz;

	const float px[8] = { x0, x1, x1, x0, x0, x1, x1, x0 };
	const float py[8] = { y0, y0, y1, y1, y0, y0, y1, y1 };
	const float pz[8] = { z0, z0, z0, z0, z1, z1, z1, z1 };
	static const u8 corner[6][4] = {
		{5,1,2,6}, {0,4,7,3}, {5,4,0,1}, {2,3,7,6}, {1,0,3,2}, {4,5,6,7}
	};

	float tu = b->tu, tv = b->tv, dx = b->dx, dy = b->dy, dz = b->dz;
	const float u0[6] = { tu+dz+dx, tu,       tu+dz,    tu+dz+dx,
	                      tu+dz,    tu+dz+dx+dz };
	const float u1[6] = { tu+dz+dx+dz, tu+dz, tu+dz+dx, tu+dz+dx+dx,
	                      tu+dz+dx,    tu+dz+dx+dz+dx };
	const float v0[6] = { tv+dz, tv+dz, tv, tv, tv+dz, tv+dz };
	const float v1[6] = { tv+dz+dy, tv+dz+dy, tv+dz, tv+dz, tv+dz+dy, tv+dz+dy };

	int f, v;
	GX_Begin(GX_QUADS, ENT_FMT, 24);
	for (f = 0; f < 6; f++) {
		u32 s = g_faceShade[f];
		u8 r = (u8)(s * tintR / 255), g = (u8)(s * tintG / 255),
		   bl = (u8)(s * tintB / 255);
		float uu[4] = { u1[f], u0[f], u0[f], u1[f] };
		float vv[4] = { v0[f], v0[f], v1[f], v1[f] };
		for (v = 0; v < 4; v++) {
			int c = corner[f][v];
			GX_Position3f32(px[c], py[c], pz[c]);
			GX_Color4u8(r, g, bl, 255);
			GX_TexCoord2f32(uu[v] / texW, vv[v] / texH);
		}
	}
	GX_End();
}

/* ModelRenderer.render's own transform: translate to the joint, then rotate Z,
 * Y, X -- in that order, which is what makes an arm's swing and its sway
 * compose the way vanilla's do. */
static void part_matrix(Mtx out, const MPart *p) {
	Mtx m, r;
	guMtxRotRad(m, 'x', p->rx);
	if (p->ry != 0.0f) { guMtxRotRad(r, 'y', p->ry); guMtxConcat(r, m, m); }
	if (p->rz != 0.0f) { guMtxRotRad(r, 'z', p->rz); guMtxConcat(r, m, m); }
	guMtxTransApply(m, m, p->rpx, p->rpy, p->rpz);
	guMtxCopy(m, out);
}

/* view * translate(entity) * rotate(bodyYaw) * scale(model units -> world).
 * Everything a part matrix hangs off. */
static void entity_base(Mtx out, Mtx view, double ix, double iy, double iz,
                        float bodyYaw, float unit, float rootY) {
	Mtx m, r;
	float s = unit * WORLD_BLOCK_SIZE;
	guMtxScale(m, -s, -s, s);                 /* MC model axes -> engine axes */
	guMtxRotDeg(r, 'y', bodyYaw); guMtxConcat(r, m, m);
	guMtxTransApply(m, m,
	                (f32)(ix * WORLD_BLOCK_SIZE),
	                (f32)((iy + rootY) * WORLD_BLOCK_SIZE),
	                (f32)(iz * WORLD_BLOCK_SIZE));
	guMtxConcat(view, m, out);
}

static void draw_part(Mtx base, const MPart *p, const MBox *b,
                      float texW, float texH, u8 tr, u8 tg, u8 tb) {
	Mtx m;
	part_matrix(m, p);
	guMtxConcat(base, m, m);
	GX_LoadPosMtxImm(m, GX_PNMTX0);
	emit_box(b, texW, texH, tr, tg, tb);
}

/* ---- the player model (ModelBiped) -------------------------------------- */

enum { BP_HEAD, BP_BODY, BP_ARM_R, BP_ARM_L, BP_LEG_R, BP_LEG_L, BP_COUNT };

static const MBox g_bipedBox[BP_COUNT] = {
	{ -4, -8, -4,  8,  8, 8,  0,  0, 0 },   /* head      */
	{ -4,  0, -2,  8, 12, 4, 16, 16, 0 },   /* body      */
	{ -3, -2, -2,  4, 12, 4, 40, 16, 0 },   /* right arm */
	{ -1, -2, -2,  4, 12, 4, 40, 16, 1 },   /* left arm  */
	{ -2,  0, -2,  4, 12, 4,  0, 16, 0 },   /* right leg */
	{ -2,  0, -2,  4, 12, 4,  0, 16, 1 },   /* left leg  */
};

/* ModelBiped.setRotationAngles, in order: walk cycle, held item, idle sway,
 * sneak, then the attack swing on top of all of it. */
static void biped_angles(MPart p[BP_COUNT], const Entity *e, float alpha) {
	float limbSwing, limbAmount;
	Pose_LimbSwing(&e->pose, alpha, &limbSwing, &limbAmount);

	float body  = Pose_LerpAngle(e->pose.prevRenderYawOffset,
	                             e->pose.renderYawOffset, alpha);
	float head  = Pose_LerpAngle(e->pose.prevHeadYaw, e->pose.headYaw, alpha);
	float pitch = e->pose.prevPitch + (e->pose.pitch - e->pose.prevPitch) * alpha;
	/* netHeadYaw. The engine's yaw runs the opposite way to Minecraft's, so
	 * the head's offset from the body is (body - head) here where vanilla
	 * writes (head - body); the two are the same angle. */
	float netHeadYaw = Pose_WrapDegrees(body - head);
	float headPitch  = -pitch;          /* MC: positive pitch looks down */
	float t = (float)e->age + alpha;
	int   sneak = e->pose.sneaking;

	memset(p, 0, sizeof(MPart) * BP_COUNT);
	p[BP_HEAD].rpy  = sneak ? 1.0f : 0.0f;
	p[BP_ARM_R].rpx = -5.0f; p[BP_ARM_R].rpy = 2.0f;
	p[BP_ARM_L].rpx =  5.0f; p[BP_ARM_L].rpy = 2.0f;
	p[BP_LEG_R].rpx = -1.9f; p[BP_LEG_L].rpx = 1.9f;
	p[BP_LEG_R].rpy = p[BP_LEG_L].rpy = sneak ? 9.0f : 12.0f;
	p[BP_LEG_R].rpz = p[BP_LEG_L].rpz = sneak ? 4.0f : 0.1f;

	p[BP_HEAD].ry = netHeadYaw * DEG2RAD;
	p[BP_HEAD].rx = headPitch  * DEG2RAD;

	float ls = limbSwing * 0.6662f;
	p[BP_ARM_R].rx = cosf(ls + PI_F) * 2.0f * limbAmount * 0.5f;
	p[BP_ARM_L].rx = cosf(ls)        * 2.0f * limbAmount * 0.5f;
	p[BP_LEG_R].rx = cosf(ls)        * 1.4f * limbAmount;
	p[BP_LEG_L].rx = cosf(ls + PI_F) * 1.4f * limbAmount;

	/* Something in the hand drops the arm halfway and tips it forward. */
	if (e->held >= 0)
		p[BP_ARM_R].rx = p[BP_ARM_R].rx * 0.5f - PI_F / 10.0f;

	/* The idle sway that keeps a standing player from looking frozen. */
	float sway = cosf(t * 0.09f) * 0.05f + 0.05f;
	p[BP_ARM_R].rz += sway;
	p[BP_ARM_L].rz -= sway;
	p[BP_ARM_R].rx += sinf(t * 0.067f) * 0.05f;
	p[BP_ARM_L].rx -= sinf(t * 0.067f) * 0.05f;

	if (sneak) {
		p[BP_BODY].rx   = 0.5f;
		p[BP_ARM_R].rx += 0.4f;
		p[BP_ARM_L].rx += 0.4f;
	}

	float sp = Pose_SwingProgress(&e->pose, alpha);
	if (sp > 0.0f) {
		/* The body twists into the swing and the arms are carried round with
		 * it, then the right arm sweeps through on top. Straight out of
		 * ModelBiped, and the reason a hit reads as a hit at 480p. */
		float by = sinf(sqrtf(sp) * PI_F * 2.0f) * 0.2f;
		p[BP_BODY].ry   = by;
		p[BP_ARM_R].rpz =  sinf(by) * 5.0f;
		p[BP_ARM_R].rpx = -cosf(by) * 5.0f;
		p[BP_ARM_L].rpz = -sinf(by) * 5.0f;
		p[BP_ARM_L].rpx =  cosf(by) * 5.0f;
		p[BP_ARM_R].ry += by;
		p[BP_ARM_L].ry += by;
		p[BP_ARM_L].rx += by;

		float f1 = 1.0f - sp;
		f1 = f1 * f1; f1 = f1 * f1; f1 = 1.0f - f1;
		float f2 = sinf(f1 * PI_F);
		float f3 = sinf(sp * PI_F) * -(p[BP_HEAD].rx - 0.7f) * 0.75f;
		p[BP_ARM_R].rx -= f2 * 1.2f + f3;
		p[BP_ARM_R].ry += by * 2.0f;
		p[BP_ARM_R].rz += sinf(sp * PI_F) * -0.4f;
	}
}

/* RenderLivingBase.setBrightness, as a vertex-colour multiply rather than a
 * second TEV stage: ten ticks of deep red decaying back to normal. Vanilla
 * lerps toward red where this darkens toward it, which at 480p reads the same
 * and costs nothing. This is the primary combat-legibility cue (T19) -- there
 * are no hit sounds and no crit particles on this target. */
static void hurt_tint(const Entity *e, float alpha, u8 *r, u8 *g, u8 *b) {
	*r = *g = *b = 255;
	if (e->pose.hurtTime <= 0 || e->pose.maxHurtTime <= 0) return;
	float f = ((float)e->pose.hurtTime - alpha) / (float)e->pose.maxHurtTime;
	if (f < 0.0f) f = 0.0f;
	*g = *b = (u8)(255.0f * (1.0f - 0.75f * f));
}

static void lerp_pos(const Entity *e, float alpha,
                     double *x, double *y, double *z) {
	*x = e->prevX + (e->x - e->prevX) * alpha;
	*y = e->prevY + (e->y - e->prevY) * alpha;
	*z = e->prevZ + (e->z - e->prevZ) * alpha;
}

static void draw_player(const Entity *e, Mtx view, float alpha) {
	double ix, iy, iz;
	lerp_pos(e, alpha, &ix, &iy, &iz);
	float bodyYaw = Pose_LerpAngle(e->pose.prevRenderYawOffset,
	                               e->pose.renderYawOffset, alpha);

	Mtx base;
	entity_base(base, view, ix, iy, iz, bodyYaw, MODEL_UNIT, MODEL_ROOT_Y);

	MPart p[BP_COUNT];
	biped_angles(p, e, alpha);

	u8 tr, tg, tb;
	hurt_tint(e, alpha, &tr, &tg, &tb);

	int i;
	for (i = 0; i < BP_COUNT; i++)
		draw_part(base, &p[i], &g_bipedBox[i],
		          STEVE_TEX_W, STEVE_TEX_H, tr, tg, tb);
}

/* The held item hangs off the right arm's tip, so it needs the arm's joint
 * transform -- rebuilt here rather than kept from draw_player because the item
 * is drawn in the atlas pass, a whole GX state change later. Six trig calls
 * against a pipeline flush is the cheap side of that trade. */
static void held_item_matrix(Mtx out, const Entity *e, Mtx view, float alpha) {
	double ix, iy, iz;
	lerp_pos(e, alpha, &ix, &iy, &iz);
	float bodyYaw = Pose_LerpAngle(e->pose.prevRenderYawOffset,
	                               e->pose.renderYawOffset, alpha);

	Mtx base, arm;
	entity_base(base, view, ix, iy, iz, bodyYaw, MODEL_UNIT, MODEL_ROOT_Y);

	MPart p[BP_COUNT];
	biped_angles(p, e, alpha);
	part_matrix(arm, &p[BP_ARM_R]);
	guMtxConcat(base, arm, arm);

	/* Into the hand (LayerHeldItem's -0.0625, 0.4375, 0.0625 in world units,
	 * which is -1, 7, 1 model units), then blow a unit cube up to the ~0.4
	 * blocks vanilla renders a held item at. */
	Mtx m;
	guMtxScale(m, 0.4f / MODEL_UNIT, 0.4f / MODEL_UNIT, 0.4f / MODEL_UNIT);
	guMtxTransApply(m, m, -1.0f, 9.0f, 1.0f);
	guMtxConcat(arm, m, out);
}

/* ---- the ender dragon (T12) ---------------------------------------------
 * A silhouette, not a port. ModelDragon is thirty-odd parts driven by a ring
 * buffer of the entity's own past positions, none of which crosses GCLink --
 * the console gets one position and one yaw, 20 times a second. So the neck
 * and tail are synthesised as arcs with a slow undulation and the wings beat
 * on a sine, which is all the plugin's one atmospheric mob needs. Box sizes
 * and texture offsets are still vanilla's, so it reads as an ender dragon
 * rather than as a shape. */

static const MBox g_dragonBody   = { -12,  0, -16, 24, 24, 64,   0,   0, 0 };
static const MBox g_dragonHead   = {  -8, -8, -10, 16, 16, 16, 112,  30, 0 };
static const MBox g_dragonLip    = {  -6, -1, -24, 12,  5, 16, 176,  44, 0 };
static const MBox g_dragonJaw    = {  -6,  0, -16, 12,  4, 16, 176,  65, 0 };
static const MBox g_dragonSpine  = {  -5, -5,  -5, 10, 10, 10, 192, 104, 0 };
static const MBox g_dragonWing   = { -56, -4,  -4, 56,  8,  8, 112,  88, 0 };
static const MBox g_dragonSkin   = { -56,  0,   2, 56,  0, 56, -56,  88, 0 };
static const MBox g_dragonTipBn  = { -56, -2,  -2, 56,  4,  4, 112, 136, 0 };
static const MBox g_dragonTipSk  = { -56,  0,   2, 56,  0, 56, -56, 144, 0 };

#define DRAGON_NECK 5
#define DRAGON_TAIL 12

static void draw_dragon(const Entity *e, Mtx view, float alpha) {
	double ix, iy, iz;
	lerp_pos(e, alpha, &ix, &iy, &iz);
	float bodyYaw = Pose_LerpAngle(e->pose.prevHeadYaw, e->pose.headYaw, alpha);
	float t = ((float)e->age + alpha) * 0.05f;   /* the undulation clock */

	/* The dragon faces the other way to everything else. RendererLivingEntity
	 * turns a body by `180 - renderYawOffset` (RendererLivingEntity.java:417),
	 * and that half turn is what the engine's yaw convention and the mirrored X
	 * in entity_base together reproduce -- so the player comes out right. But
	 * RenderDragon overrides rotateCorpse and turns by plain `-f`
	 * (RenderDragon.java:37), with no 180. Put it back here rather than in
	 * entity_base, which every other model depends on being as it is. */
	bodyYaw += 180.0f;

	Mtx base;
	entity_base(base, view, ix, iy, iz, bodyYaw, DRAGON_UNIT, DRAGON_ROOT_Y);

	u8 tr, tg, tb;
	hurt_tint(e, alpha, &tr, &tg, &tb);
	const float tw = DRAGON_TEX_W, th = DRAGON_TEX_H;

	MPart p;
	int i;

	memset(&p, 0, sizeof p);
	p.rpy = 4.0f; p.rpz = 8.0f;
	draw_part(base, &p, &g_dragonBody, tw, th, tr, tg, tb);

	/* Neck: five segments arcing forward and up from the shoulders, each a
	 * little smaller, with the head on the end. */
	float ny = 2.0f, nz = -14.0f;
	for (i = 0; i < DRAGON_NECK; i++) {
		float wave = sinf(t * 2.0f + i * 0.45f) * 3.0f;
		memset(&p, 0, sizeof p);
		p.rpx = wave * 0.4f; p.rpy = ny; p.rpz = nz;
		p.rx  = -0.18f;
		p.ry  = wave * 0.02f;
		draw_part(base, &p, &g_dragonSpine, tw, th, tr, tg, tb);
		ny -= 2.2f;
		nz -= 9.0f;
	}

	memset(&p, 0, sizeof p);
	p.rpy = ny + 1.0f; p.rpz = nz + 2.0f;
	p.rx  = -0.25f + sinf(t * 2.0f) * 0.06f;
	draw_part(base, &p, &g_dragonHead, tw, th, tr, tg, tb);
	draw_part(base, &p, &g_dragonLip,  tw, th, tr, tg, tb);
	/* The jaw hangs off the head's own joint, so it opens rather than floats. */
	MPart jaw = p;
	jaw.rpy += 4.0f; jaw.rpz -= 8.0f;
	jaw.rx  += 0.15f + sinf(t * 1.3f) * 0.12f;
	draw_part(base, &jaw, &g_dragonJaw, tw, th, tr, tg, tb);

	/* Tail: twelve segments trailing behind the body, shrinking and swinging. */
	float ty = 4.0f, tz = 50.0f;
	for (i = 0; i < DRAGON_TAIL; i++) {
		float wave = sinf(t * 1.6f - i * 0.5f);
		memset(&p, 0, sizeof p);
		p.rpx = wave * (2.0f + i * 0.9f);
		p.rpy = ty + i * 0.4f;
		p.rpz = tz + i * 8.5f;
		p.ry  = wave * 0.12f;
		draw_part(base, &p, &g_dragonSpine, tw, th, tr, tg, tb);
	}

	/* Both wings, the second in an X-mirrored space exactly as ModelDragon
	 * draws it -- culling is off, so no cull-face flip is needed with it. */
	float beat = cosf(t * 3.0f);
	int side;
	for (side = 0; side < 2; side++) {
		Mtx wingBase;
		if (side) {
			Mtx flip;
			guMtxScale(flip, -1.0f, 1.0f, 1.0f);
			guMtxConcat(base, flip, wingBase);
		} else {
			guMtxCopy(base, wingBase);
		}

		MPart w;
		memset(&w, 0, sizeof w);
		w.rpx = -12.0f; w.rpy = 5.0f; w.rpz = 2.0f;
		w.rx = 0.125f - beat * 0.2f;
		w.ry = 0.25f;
		w.rz = beat * 0.35f + 0.35f;
		draw_part(wingBase, &w, &g_dragonWing, tw, th, tr, tg, tb);
		draw_part(wingBase, &w, &g_dragonSkin, tw, th, tr, tg, tb);

		/* The tip is a child of the wing: build its matrix inside the wing's. */
		Mtx wm, tm;
		part_matrix(wm, &w);
		guMtxConcat(wingBase, wm, wm);
		MPart tip;
		memset(&tip, 0, sizeof tip);
		tip.rpx = -56.0f;
		tip.rz  = -(beat + 0.125f) * 0.8f;
		part_matrix(tm, &tip);
		guMtxConcat(wm, tm, tm);
		GX_LoadPosMtxImm(tm, GX_PNMTX0);
		emit_box(&g_dragonTipBn, tw, th, tr, tg, tb);
		emit_box(&g_dragonTipSk, tw, th, tr, tg, tb);
	}
}

/* ---- projectiles --------------------------------------------------------
 * Untextured boxes, drawn flat-coloured. The atlas has no arrow, pearl, potion
 * or bobber tile yet -- those are T13's, together with the rest of the kit's
 * art -- and a distinctly coloured dart reads better in a fight than a
 * placeholder sprite would. Snowballs and dropped items do have art, and go
 * through the atlas pass instead. */
static const MBox g_arrowShaft = { -1, -1, -8, 2, 2, 14, 0, 0, 0 };
static const MBox g_arrowHead  = { -1, -1,  6, 2, 2,  4, 0, 0, 0 };
static const MBox g_blob       = { -2, -2, -2, 4, 4,  4, 0, 0, 0 };

static void draw_projectile(const Entity *e, Mtx view, float alpha) {
	double ix, iy, iz;
	lerp_pos(e, alpha, &ix, &iy, &iz);
	float yaw   = Pose_LerpAngle(e->pose.prevHeadYaw, e->pose.headYaw, alpha);
	float pitch = e->pose.prevPitch + (e->pose.pitch - e->pose.prevPitch) * alpha;

	Mtx base;
	entity_base(base, view, ix, iy, iz, yaw, DRAGON_UNIT, 0.0f);

	MPart p;
	memset(&p, 0, sizeof p);
	/* Model +X is world -X, so the pitch about it flips sign on the way out;
	 * negating here points the nose the way the thing is actually flying. */
	p.rx = -pitch * DEG2RAD;

	if (e->type == GCLINK_ENT_ARROW) {
		draw_part(base, &p, &g_arrowShaft, 1.0f, 1.0f, 120, 110, 100);
		draw_part(base, &p, &g_arrowHead,  1.0f, 1.0f, 225, 225, 235);
		return;
	}

	u8 r = 220, g = 220, b = 220;
	switch (e->type) {
	case GCLINK_ENT_PEARL:  r =  40; g = 190; b = 165; break;
	case GCLINK_ENT_POTION: r = 210; g =  70; b = 220; break;
	case GCLINK_ENT_BOBBER: r = 200; g =  60; b =  60; break;
	default: break;
	}
	draw_part(base, &p, &g_blob, 1.0f, 1.0f, r, g, b);
}

/* ---- GX passes ---------------------------------------------------------- */

static void setup_common(void) {
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetAlphaCompare(GX_GEQUAL, 128, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	/* The (-1,-1,1) conversion preserves handedness, so winding is consistent
	 * -- but the dragon's wing membranes are zero-thickness and have to be
	 * visible from both sides, and this matches every other pass here. */
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
}

static void setup_skinned(GXTexObj *tex) {
	setup_common();
	GX_LoadTexObj(tex, GX_TEXMAP0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
}

/* Vertex colour only. The texcoords the emitter writes are still in the
 * vertex descriptor and simply go unread, exactly as hud.c's flat pass does. */
static void setup_solid(void) {
	setup_common();
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
}

void Entity_Draw(const EntityWorld *ew, Mtx view, float alpha) {
	if (ew->live == 0) return;

	/* Grouped by GX state rather than by entity, so a busy fight costs three
	 * pipeline changes instead of one per entity. */
	int i, any;

	for (i = 0, any = 0; i < ENTITY_MAX; i++) {
		const Entity *e = &ew->e[i];
		if (!e->alive || e->type != GCLINK_ENT_PLAYER) continue;
		if (e->flags & GCLINK_EFLAG_INVISIBLE) continue;
		if (!any) { setup_skinned(&steveTex); any = 1; }
		draw_player(e, view, alpha);
	}

	for (i = 0, any = 0; i < ENTITY_MAX; i++) {
		const Entity *e = &ew->e[i];
		if (!e->alive || e->type != GCLINK_ENT_DRAGON) continue;
		if (!any) { setup_skinned(&dragonTex); any = 1; }
		draw_dragon(e, view, alpha);
	}

	for (i = 0, any = 0; i < ENTITY_MAX; i++) {
		const Entity *e = &ew->e[i];
		if (!e->alive) continue;
		if (e->type != GCLINK_ENT_ARROW && e->type != GCLINK_ENT_PEARL &&
		    e->type != GCLINK_ENT_POTION && e->type != GCLINK_ENT_BOBBER) continue;
		if (!any) { setup_solid(); any = 1; }
		draw_projectile(e, view, alpha);
	}

	/* Anything drawn out of the block atlas: dropped items, snowballs, and
	 * whatever is in a player's hand. HeldItem_SetupGX owns this state. */
	for (i = 0, any = 0; i < ENTITY_MAX; i++) {
		const Entity *e = &ew->e[i];
		if (!e->alive) continue;

		int item = -1;
		Mtx m;
		if (e->type == GCLINK_ENT_ITEM || e->type == GCLINK_ENT_SNOWBALL) {
			item = (e->type == GCLINK_ENT_SNOWBALL && e->held < 0)
			     ? ITEM_SNOWBALL_TILE : e->held;
			if (item < 0) continue;
			double ix, iy, iz;
			lerp_pos(e, alpha, &ix, &iy, &iz);
			float t   = (float)e->age + alpha;
			float bob = sinf(t / 10.0f) * 0.1f + 0.1f;
			Mtx r;
			guMtxScale(m, 0.25f * WORLD_BLOCK_SIZE, 0.25f * WORLD_BLOCK_SIZE,
			              0.25f * WORLD_BLOCK_SIZE);
			guMtxRotDeg(r, 'y', t * 2.8f); guMtxConcat(r, m, m);
			guMtxTransApply(m, m,
			                (f32)(ix * WORLD_BLOCK_SIZE),
			                (f32)((iy + 0.125 + bob) * WORLD_BLOCK_SIZE),
			                (f32)(iz * WORLD_BLOCK_SIZE));
			guMtxConcat(view, m, m);
		} else if (e->type == GCLINK_ENT_PLAYER && e->held >= 0 &&
		           !(e->flags & GCLINK_EFLAG_INVISIBLE)) {
			item = e->held;
			held_item_matrix(m, e, view, alpha);
		} else {
			continue;
		}

		if (!any) { HeldItem_SetupGX(); any = 1; }
		GX_LoadPosMtxImm(m, GX_PNMTX0);
		if (item < NUM_BLOCK_IDS) HeldItem_DrawBlockMesh(item);
		else                      HeldItem_DrawFlatMesh(item);
	}

	World_SetupRenderState();
}

/* ---- nametags ----------------------------------------------------------- */

/* World point -> normalised device coordinates. `view` is libogc's 3x4 and
 * `proj` the 4x4 guPerspective left loaded; the w this divides by is -z_view,
 * so a point behind the camera fails the test rather than folding round onto
 * the screen. */
static int project(Mtx view, Mtx44 proj, double wx, double wy, double wz,
                   float *ndcX, float *ndcY) {
	float x = (float)wx, y = (float)wy, z = (float)wz;
	float vx = view[0][0]*x + view[0][1]*y + view[0][2]*z + view[0][3];
	float vy = view[1][0]*x + view[1][1]*y + view[1][2]*z + view[1][3];
	float vz = view[2][0]*x + view[2][1]*y + view[2][2]*z + view[2][3];

	float cw = proj[3][0]*vx + proj[3][1]*vy + proj[3][2]*vz + proj[3][3];
	if (cw < 0.01f) return 0;
	float cx = proj[0][0]*vx + proj[0][1]*vy + proj[0][2]*vz + proj[0][3];
	float cy = proj[1][0]*vx + proj[1][1]*vy + proj[1][2]*vz + proj[1][3];
	*ndcX = cx / cw;
	*ndcY = cy / cw;
	return 1;
}

/* FontRenderer's 16 colour codes as 0xRRGGBBAA. The same construction hud.c
 * uses; duplicated rather than exported because it is four lines and the
 * alternative is a header dependency in the other direction. */
static u32 team_colour(u8 code) {
	if (code > 15) return 0xFFFFFFFFu;
	int j = ((code >> 3) & 1) * 85;
	int r = ((code >> 2) & 1) * 170 + j;
	int g = ((code >> 1) & 1) * 170 + j;
	int b = ((code >> 0) & 1) * 170 + j;
	if (code == 6) r += 85;
	return ((u32)(r & 255) << 24) | ((u32)(g & 255) << 16) |
	       ((u32)(b & 255) << 8) | 0xFFu;
}

int Entity_CollectTags(const EntityWorld *ew, const World *w,
                       Mtx view, Mtx44 proj,
                       double eyeX, double eyeY, double eyeZ,
                       int fbWidth, int efbHeight,
                       HudTag *out, int max) {
	HudScreen sc = Hud_Screen(fbWidth, efbHeight);
	int n = 0, i;
	double worst = 0.0;      /* the furthest tag currently kept */
	int    worstAt = -1;

	if (max > HUD_TAG_MAX) max = HUD_TAG_MAX;
	/* Distances of the kept tags, parallel to `out` -- only ever `max` long,
	 * so the "is this nearer than the worst one" test stays a linear scan over
	 * sixteen doubles rather than a sort of a hundred and twenty-eight. */
	double dist[HUD_TAG_MAX];

	for (i = 0; i < ENTITY_MAX && max > 0; i++) {
		const Entity *e = &ew->e[i];
		if (!e->alive || e->type != GCLINK_ENT_PLAYER) continue;
		if (e->flags & GCLINK_EFLAG_INVISIBLE) continue;
		if (!e->name[0]) continue;

		double hx = e->x, hy = e->y + 2.3, hz = e->z;   /* height + 0.5 */
		double dx = hx - eyeX, dy = hy - eyeY, dz = hz - eyeZ;
		double d2 = dx*dx + dy*dy + dz*dz;
		/* The hard distance cull is the frame budget's main lever, so it comes
		 * before the projection and long before the ray-trace. */
		if (d2 > ENTITY_TAG_DISTANCE * ENTITY_TAG_DISTANCE) continue;
		if (n == max && d2 >= worst) continue;

		float ndcX, ndcY;
		if (!project(view, proj,
		             hx * WORLD_BLOCK_SIZE, hy * WORLD_BLOCK_SIZE,
		             hz * WORLD_BLOCK_SIZE, &ndcX, &ndcY)) continue;
		if (ndcX < -1.2f || ndcX > 1.2f || ndcY < -1.2f || ndcY > 1.2f) continue;

		/* Nothing depth-tests a flat overlay, so occlusion is decided here:
		 * the same grid walk the crosshair uses, eye to head. */
		if (w) {
			BlockHit hit;
			if (World_RayTrace(w, eyeX, eyeY, eyeZ, hx, hy, hz, &hit)) continue;
		}

		int slot;
		if (n < max) {
			slot = n++;
		} else {
			slot = worstAt;          /* evict the furthest one we kept */
		}

		HudTag *t = &out[slot];
		t->x = (short)((ndcX * 0.5f + 0.5f) * sc.w);
		t->y = (short)((0.5f - ndcY * 0.5f) * sc.h);
		t->colour = team_colour(e->colour);
		strncpy(t->text, e->name, HUD_TAG_TEXT - 1);
		t->text[HUD_TAG_TEXT - 1] = '\0';
		dist[slot] = d2;

		worst = 0.0; worstAt = 0;
		int k;
		for (k = 0; k < n; k++)
			if (dist[k] > worst) { worst = dist[k]; worstAt = k; }
	}
	return n;
}

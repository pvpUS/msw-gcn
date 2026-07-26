#include <math.h>
#include <gccore.h>

#include "helditem.h"
#include "world.h"           /* World_BindAtlas                                */
#include "inventory.h"
#include "atlas_gen.h"       /* ATLAS_COLS/CELL/PAD/TILE/TEX_W/TEX_H           */
#include "block_faces_gen.h" /* NUM_BLOCK_IDS, g_topTile[], g_bottomTile[]     */

/* Dedicated vertex format so this never disturbs the world's GX_VTXFMT0 (indexed
 * CLR0/TEX0) or the HUD's GX_VTXFMT1: POS as 3D floats + RGBA8 colour + float
 * texcoords, all streamed direct in immediate mode. */
#define HELD_FMT GX_VTXFMT2

/* ---- placement tunables (view space; camera at the origin looking down -Z,
 * +X right, +Y up). The item hangs in the lower-right corner. The perspective
 * projection is the one main.c leaves loaded (60 deg FOV, near plane 1.0), so
 * everything sits a little beyond z=-1 to clear the near plane even after the
 * cube's corners swing out under rotation. ------------------------------------*/

/* Held block (3D cube). */
#define HB_X      0.86f
#define HB_Y     -0.80f
#define HB_Z     -1.95f
#define HB_SCALE  0.82f
#define HB_YAW  (-45.0f)   /* spin so a vertical edge faces you (see 3 faces)  */
#define HB_PITCH  20.0f    /* tip the top toward the camera                    */

/* Held flat item (single card, e.g. the sword). */
#define HI_X      0.80f
#define HI_Y     -0.62f
#define HI_Z     -1.75f
#define HI_SCALE  1.15f
#define HI_YAW  (-8.0f)
#define HI_ROLL   38.0f    /* tilt so the blade points up and to the left      */

/* Unit-cube corners per face and their tile UV corners -- same face order and
 * winding convention as world.c (0:-X 1:+X 2:-Y 3:+Y(top) 4:-Z 5:+Z). Culling
 * is off and depth sorts the faces, so winding only needs to match the UV
 * table, not a front/back sense. */
static const s16 faceVerts[6][4][3] = {
	{ {0,0,0},{0,0,1},{0,1,1},{0,1,0} }, /* -X */
	{ {1,0,1},{1,0,0},{1,1,0},{1,1,1} }, /* +X */
	{ {0,0,0},{1,0,0},{1,0,1},{0,0,1} }, /* -Y */
	{ {0,1,1},{1,1,1},{1,1,0},{0,1,0} }, /* +Y */
	{ {1,0,0},{0,0,0},{0,1,0},{1,1,0} }, /* -Z */
	{ {0,0,1},{1,0,1},{1,1,1},{0,1,1} }, /* +Z */
};
/* UV corner (u,v) in {0,1}; v=0 is the tile's top edge. */
static const u8 faceUV[6][4][2] = {
	{ {0,1},{1,1},{1,0},{0,0} }, /* -X */
	{ {0,1},{1,1},{1,0},{0,0} }, /* +X */
	{ {0,0},{1,0},{1,1},{0,1} }, /* -Y */
	{ {0,0},{1,0},{1,1},{0,1} }, /* +Y */
	{ {0,1},{1,1},{1,0},{0,0} }, /* -Z */
	{ {0,1},{1,1},{1,0},{0,0} }, /* +Z */
};
/* Minecraft-style directional ambient shade per face (matches world.c). */
static const u8 faceShade[6] = { 153, 153, 128, 255, 204, 204 };

/* Full-tile UV rect for an atlas tile index (the padded interior, edge-to-edge;
 * the ATLAS_PAD border absorbs mip bleed). Same math as hud.c's tile_icon. */
static void tile_uv(int tile, float *u0, float *v0, float *u1, float *v1) {
	int col = tile % ATLAS_COLS, row = tile / ATLAS_COLS;
	float px0 = (float)(col * ATLAS_CELL + ATLAS_PAD);
	float py0 = (float)(row * ATLAS_CELL + ATLAS_PAD);
	*u0 = px0 / ATLAS_TEX_W;  *u1 = (px0 + ATLAS_TILE) / (float)ATLAS_TEX_W;
	*v0 = py0 / ATLAS_TEX_H;  *v1 = (py0 + ATLAS_TILE) / (float)ATLAS_TEX_H;
}

/* Build a model-view (view = identity) placing a unit shape centered at the
 * origin: scale, then roll(Z)/pitch(X)/yaw(Y), then translate into view space. */
static void build_mv(Mtx mv, float tx, float ty, float tz,
                     float pitch, float yaw, float roll, float sc) {
	Mtx m, r;
	guMtxScale(m, sc, sc, sc);
	if (roll  != 0.0f) { guMtxRotDeg(r, 'z', roll);  guMtxConcat(r, m, m); }
	if (pitch != 0.0f) { guMtxRotDeg(r, 'x', pitch); guMtxConcat(r, m, m); }
	if (yaw   != 0.0f) { guMtxRotDeg(r, 'y', yaw);   guMtxConcat(r, m, m); }
	guMtxTransApply(m, m, tx, ty, tz);   /* pre-multiply: translate in view space */
	guMtxCopy(m, mv);
}

/* A shaded, per-face-textured unit cube centered at the origin. */
void HeldItem_DrawBlockMesh(int item) {
	int f, v;
	for (f = 0; f < 6; f++) {
		int tile = (f == 3) ? g_topTile[item]
		         : (f == 2) ? g_bottomTile[item] : item;
		float u0, v0, u1, v1;
		tile_uv(tile, &u0, &v0, &u1, &v1);
		u8 sh = faceShade[f];
		GX_Begin(GX_QUADS, HELD_FMT, 4);
		for (v = 0; v < 4; v++) {
			GX_Position3f32((f32)faceVerts[f][v][0] - 0.5f,
			                (f32)faceVerts[f][v][1] - 0.5f,
			                (f32)faceVerts[f][v][2] - 0.5f);
			GX_Color4u8(sh, sh, sh, 255);
			GX_TexCoord2f32(faceUV[f][v][0] ? u1 : u0,
			                faceUV[f][v][1] ? v1 : v0);
		}
		GX_End();
	}
}

/* A single upright textured card in the XY plane (an item sprite, e.g. the
 * sword). Full-bright, double-sided (culling is off). */
void HeldItem_DrawFlatMesh(int tile) {
	float u0, v0, u1, v1;
	tile_uv(tile, &u0, &v0, &u1, &v1);
	GX_Begin(GX_QUADS, HELD_FMT, 4);
	GX_Position3f32(-0.5f, -0.5f, 0.0f); GX_Color4u8(255,255,255,255); GX_TexCoord2f32(u0, v1);
	GX_Position3f32( 0.5f, -0.5f, 0.0f); GX_Color4u8(255,255,255,255); GX_TexCoord2f32(u1, v1);
	GX_Position3f32( 0.5f,  0.5f, 0.0f); GX_Color4u8(255,255,255,255); GX_TexCoord2f32(u1, v0);
	GX_Position3f32(-0.5f,  0.5f, 0.0f); GX_Color4u8(255,255,255,255); GX_TexCoord2f32(u0, v0);
	GX_End();
}

void HeldItem_InitGX(void) {
	GX_SetVtxAttrFmt(HELD_FMT, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
	GX_SetVtxAttrFmt(HELD_FMT, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(HELD_FMT, GX_VA_TEX0, GX_TEX_ST,   GX_F32,   0);
}

void HeldItem_SetupGX(void) {
	World_BindAtlas();

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);

	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GX_SetAlphaCompare(GX_GEQUAL, 128, GX_AOP_AND, GX_ALWAYS, 0);  /* cutout */
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
}

void HeldItem_Draw(const Player *p, int fbWidth, int efbHeight, float alpha) {
	ItemStack *s = Inventory_GetCurrentItem((Inventory *)&p->inventory);
	if (!s || s->item < 0) return;          /* empty hand -> draw nothing */
	int item = s->item;

	/* Gentle view-bob: a small figure-eight sway that grows with walk speed and
	 * settles to zero at rest (so a static screenshot shows the neutral pose).
	 * The phase lives on the Pose and advances at the tick rate; this reads it
	 * interpolated, so the sway no longer depends on the frame time. */
	float phase = Pose_Bob(&p->pose, alpha);
	float amp = p->pose.prevLimbSwingAmount +
	            (p->pose.limbSwingAmount - p->pose.prevLimbSwingAmount) * alpha;
	float bobX =  sinf(phase)        * 0.025f * amp;
	float bobY = -fabsf(cosf(phase)) * 0.030f * amp;

	/* ItemRenderer.transformFirstPersonItem's swing arc, on top of this
	 * engine's own resting placement. `f` peaks late and `f1` early, which is
	 * what gives the motion its snap: the item whips down and across, then
	 * eases back over the remainder of the six ticks. */
	float sw = Pose_SwingProgress(&p->pose, alpha);
	float f  = sinf(sw * sw * (float)M_PI);
	float f1 = sinf(sqrtf(sw) * (float)M_PI);
	float swingYaw   = f  * -20.0f;
	float swingRoll  = f1 * -20.0f;
	float swingPitch = f1 * -80.0f;

	/* ---- GX state: a small 3D overlay drawn on top of the world ---- */
	HeldItem_SetupGX();

	/* Squeeze this pass's depth into the near [0,0.1] slice so the item draws in
	 * front of essentially all world geometry (which spans [0,1]) while its own
	 * faces still depth-sort correctly against each other. The perspective
	 * projection main.c loaded before World_Draw is still current -- we only set
	 * the model-view. */
	GX_SetViewport(0, 0, fbWidth, efbHeight, 0.0f, 0.1f);

	Mtx mv;
	if (item < NUM_BLOCK_IDS) {
		build_mv(mv, HB_X + bobX, HB_Y + bobY, HB_Z,
		         HB_PITCH + swingPitch, HB_YAW + swingYaw, swingRoll, HB_SCALE);
		GX_LoadPosMtxImm(mv, GX_PNMTX0);
		HeldItem_DrawBlockMesh(item);
	} else {
		build_mv(mv, HI_X + bobX, HI_Y + bobY, HI_Z,
		         swingPitch, HI_YAW + swingYaw, HI_ROLL + swingRoll, HI_SCALE);
		GX_LoadPosMtxImm(mv, GX_PNMTX0);
		HeldItem_DrawFlatMesh(item);
	}

	/* Restore the full depth range for whatever draws next. */
	GX_SetViewport(0, 0, fbWidth, efbHeight, 0.0f, 1.0f);
}

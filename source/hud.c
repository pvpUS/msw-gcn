#include <math.h>
#include <gccore.h>

#include "hud.h"
#include "world.h"
#include "inventory.h"
#include "atlas_gen.h"   /* ATLAS_COLS/CELL/PAD/TILE/TEX_W/TEX_H */

/* Dedicated 2D vertex format so we never disturb the world's GX_VTXFMT0.
 * POS as 2D floats + RGBA8 vertex colour + float texcoords, matching the
 * canonical GameCube sprite setup (see .gc-examples gxSprites). */
#define HUD_FMT GX_VTXFMT1

/* ---- GX state helpers -------------------------------------------------- */

/* Flat, vertex-colour-only stage (panels, hearts, digits, crosshair). */
static void tev_flat(void) {
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
}

/* Textured stage modulated by vertex colour (block icons from the atlas). */
static void tev_tex(void) {
	GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
}

/* ---- primitives (all in gui-space pixels; y grows downward) ------------ */

static void rect(float x, float y, float w, float h,
                 u8 r, u8 g, u8 b, u8 a) {
	GX_Begin(GX_QUADS, HUD_FMT, 4);
	GX_Position2f32(x,     y    ); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0, 0);
	GX_Position2f32(x + w, y    ); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0, 0);
	GX_Position2f32(x + w, y + h); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0, 0);
	GX_Position2f32(x,     y + h); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0, 0);
	GX_End();
}

/* Hollow border of thickness t drawn just inside (x,y,w,h). */
static void frame(float x, float y, float w, float h, float t,
                  u8 r, u8 g, u8 b, u8 a) {
	rect(x,         y,         w, t, r, g, b, a);   /* top    */
	rect(x,         y + h - t, w, t, r, g, b, a);   /* bottom */
	rect(x,         y,         t, h, r, g, b, a);   /* left   */
	rect(x + w - t, y,         t, h, r, g, b, a);   /* right  */
}

/* One atlas tile (a block's side face == its global id) as an s x s icon.
 * Requires tev_tex() + World_BindAtlas() first. */
static void tile_icon(float x, float y, float s, int tile) {
	int col = tile % ATLAS_COLS;
	int row = tile / ATLAS_COLS;
	float px0 = (float)(col * ATLAS_CELL + ATLAS_PAD);
	float py0 = (float)(row * ATLAS_CELL + ATLAS_PAD);
	float u0 = px0 / ATLAS_TEX_W, u1 = (px0 + ATLAS_TILE) / (float)ATLAS_TEX_W;
	float v0 = py0 / ATLAS_TEX_H, v1 = (py0 + ATLAS_TILE) / (float)ATLAS_TEX_H;
	GX_Begin(GX_QUADS, HUD_FMT, 4);
	GX_Position2f32(x,     y    ); GX_Color4u8(255,255,255,255); GX_TexCoord2f32(u0, v0);
	GX_Position2f32(x + s, y    ); GX_Color4u8(255,255,255,255); GX_TexCoord2f32(u1, v0);
	GX_Position2f32(x + s, y + s); GX_Color4u8(255,255,255,255); GX_TexCoord2f32(u1, v1);
	GX_Position2f32(x,     y + s); GX_Color4u8(255,255,255,255); GX_TexCoord2f32(u0, v1);
	GX_End();
}

/* ---- 3x5 digit font (stack counts) ------------------------------------ */
/* Each row is 3 bits, MSB = leftmost column. */
static const u8 DIGITS[10][5] = {
	{7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
	{7,4,7,1,7}, {7,4,7,5,7}, {7,1,2,2,2}, {7,5,7,5,7}, {7,5,7,1,7},
};

static void draw_digit(float x, float y, float px, int d,
                       u8 r, u8 g, u8 b, u8 a) {
	int row, col;
	for (row = 0; row < 5; row++)
		for (col = 0; col < 3; col++)
			if (DIGITS[d][row] & (1 << (2 - col)))
				rect(x + col * px, y + row * px, px, px, r, g, b, a);
}

static int num_digits(int v) {
	int n = 1;
	while (v >= 10) { v /= 10; n++; }
	return n;
}

/* Draw `value` with its MOST significant digit's top-left at (x,y). Columns are
 * px wide; a 1-column gap separates digits (advance 4*px per digit). */
static void draw_number(float x, float y, float px, int value,
                        u8 r, u8 g, u8 b, u8 a) {
	int n = num_digits(value);
	int i;
	for (i = n - 1; i >= 0; i--) {
		int place = 1, k;
		for (k = 0; k < i; k++) place *= 10;
		draw_digit(x, y, px, (value / place) % 10, r, g, b, a);
		x += 4 * px;
	}
}

/* Stack count, right-aligned to (rx, ...) with the digits' bottom at by, with a
 * 1px drop shadow (like the vanilla item overlay). */
static void draw_count(float rx, float by, float px, int value) {
	float w = (4 * num_digits(value) - 1) * px;
	float x = rx - w;
	float y = by - 5 * px;
	draw_number(x + px, y + px, px, value, 40, 40, 40, 255);   /* shadow */
	draw_number(x,      y,      px, value, 255, 255, 255, 255); /* white  */
}

/* ---- heart bitmap (7 wide x 6 tall), MSB = leftmost column ------------- */
static const u8 HEART[6] = { 0x36, 0x7F, 0x7F, 0x3E, 0x1C, 0x08 };

static void draw_heart(float x, float y, float px, int halfOnly,
                       u8 r, u8 g, u8 b, u8 a) {
	int row, col;
	for (row = 0; row < 6; row++)
		for (col = 0; col < 7; col++) {
			if (halfOnly && col >= 4) continue;   /* left ~half of the heart */
			if (HEART[row] & (1 << (6 - col)))
				rect(x + col * px, y + row * px, px, px, r, g, b, a);
		}
}

/* Health row: 10 dark containers with red hearts on top. Mirrors
 * GuiIngame.renderPlayerStats' container/full/half logic. */
static void draw_hearts(float left, float top, float px, float health) {
	int hp = (int)ceil((double)health);   /* MathHelper.ceiling_float_int(getHealth()) */
	int i;
	for (i = 0; i < 10; i++) {
		float x = left + i * 8.0f;
		draw_heart(x, top, px, 0, 48, 48, 48, 200);          /* container */
		if (i * 2 + 1 < hp)       draw_heart(x, top, px, 0, 220, 30, 40, 255);
		else if (i * 2 + 1 == hp) draw_heart(x, top, px, 1, 220, 30, 40, 255);
	}
}

/* ---- inventory-screen slot geometry (GuiInventory-ish, 176x166 panel) -- */
/* Cell top-left for a main-inventory index (0-8 hotbar row, 9-35 storage). */
static void inv_cell(int index, float gl, float gt, float *cx, float *cy) {
	if (index < 9) {                       /* hotbar row */
		*cx = gl + 8 + index * 18;
		*cy = gt + 142;
	} else {                               /* 3x9 storage grid */
		int s = index - 9;
		*cx = gl + 8 + (s % 9) * 18;
		*cy = gt + 84 + (s / 9) * 18;
	}
}

/* ---- public ------------------------------------------------------------ */

void Hud_InitGX(void) {
	GX_SetVtxAttrFmt(HUD_FMT, GX_VA_POS,  GX_POS_XY,   GX_F32,   0);
	GX_SetVtxAttrFmt(HUD_FMT, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(HUD_FMT, GX_VA_TEX0, GX_TEX_ST,   GX_F32,   0);
}

void Hud_Draw(Player *p, int fbWidth, int efbHeight, int invOpen, int cursorSlot) {
	Inventory *inv = &p->inventory;

	/* GUI scale a la ScaledResolution: work in a virtual space ~1/2..1/3 the
	 * framebuffer so HUD elements stay a sensible size across video modes. */
	int scale = efbHeight / 240;
	if (scale < 1) scale = 1;
	if (scale > 3) scale = 3;
	float gw = (float)fbWidth  / scale;
	float gh = (float)efbHeight / scale;

	/* --- enter 2D --- */
	Mtx44 proj;
	guOrtho(proj, 0, gh, 0, gw, 0, 300);
	GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);

	Mtx mv;
	guMtxIdentity(mv);
	guMtxTransApply(mv, mv, 0.0f, 0.0f, -5.0f);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);

	GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);          /* always on top */
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);

	/* hotbar geometry (GuiIngame.renderTooltip) */
	float hbLeft = gw / 2 - 91;
	float hbTop  = gh - 22;

	/* ===== pass A: flat backgrounds ===== */
	tev_flat();

	if (invOpen) {
		float gl = (gw - 176) / 2, gt = (gh - 166) / 2;
		rect(0, 0, gw, gh, 0, 0, 0, 160);                 /* dim the world */
		rect(gl, gt, 176, 166, 120, 120, 120, 245);       /* panel */
		frame(gl, gt, 176, 166, 2, 60, 60, 60, 255);
		int i;
		for (i = 0; i < INV_MAIN_SIZE; i++) {             /* 36 main cells */
			float cx, cy; inv_cell(i, gl, gt, &cx, &cy);
			rect(cx, cy, 18, 18, 40, 40, 40, 255);
			if (i == inv->currentItem)                    /* held-slot marker */
				frame(cx, cy, 18, 18, 1, 255, 255, 255, 220);
		}
		for (i = 0; i < INV_ARMOR_SIZE; i++)              /* 4 armor cells */
			rect(gl + 8, gt + 8 + i * 18, 18, 18, 40, 40, 40, 255);

		/* Slot cursor, standing in for the mouse pointer: a lit cell plus a
		 * bright border, like GuiContainer's hovered-slot highlight. */
		if (cursorSlot >= 0 && cursorSlot < INV_MAIN_SIZE) {
			float cx, cy; inv_cell(cursorSlot, gl, gt, &cx, &cy);
			rect(cx, cy, 18, 18, 255, 255, 255, 90);
			frame(cx - 1, cy - 1, 20, 20, 2, 255, 220, 60, 255);
		}
	} else {
		/* crosshair */
		float cx = gw / 2, cy = gh / 2;
		rect(cx - 4, cy - 1, 9, 2, 255, 255, 255, 200);
		rect(cx - 1, cy - 4, 2, 9, 255, 255, 255, 200);

		/* hotbar panel + slots + held-slot highlight */
		rect(hbLeft, hbTop, 182, 22, 0, 0, 0, 150);
		frame(hbLeft, hbTop, 182, 22, 1, 200, 200, 200, 200);
		int j;
		for (j = 0; j < INV_HOTBAR_SIZE; j++)
			rect(hbLeft + 2 + j * 20, hbTop + 2, 18, 18, 255, 255, 255, 25);
		frame(hbLeft - 1 + inv->currentItem * 20, hbTop - 1, 24, 24, 2,
		      255, 255, 255, 255);

		/* health hearts, sitting just above the hotbar */
		draw_hearts(hbLeft, gh - 39, 1.0f, p->health);
	}

	/* ===== pass B: textured block icons ===== */
	tev_tex();
	World_BindAtlas();
	if (!invOpen) {
		int j;
		for (j = 0; j < INV_HOTBAR_SIZE; j++) {
			ItemStack *s = &inv->main[j];
			if (!ItemStack_IsEmpty(s) && s->item >= 0)
				tile_icon(hbLeft + 3 + j * 20, hbTop + 3, 16, s->item);
		}
	} else {
		float gl = (gw - 176) / 2, gt = (gh - 166) / 2;
		int i;
		for (i = 0; i < INV_MAIN_SIZE; i++) {
			ItemStack *s = &inv->main[i];
			if (ItemStack_IsEmpty(s) || s->item < 0) continue;
			float cx, cy; inv_cell(i, gl, gt, &cx, &cy);
			tile_icon(cx + 1, cy + 1, 16, s->item);
		}
		/* The carried stack rides the cursor, drawn last so it sits on top of
		 * whatever is already in that slot (vanilla draws it under the mouse). */
		if (!ItemStack_IsEmpty(&inv->carried) && inv->carried.item >= 0 &&
		    cursorSlot >= 0 && cursorSlot < INV_MAIN_SIZE) {
			float cx, cy; inv_cell(cursorSlot, gl, gt, &cx, &cy);
			tile_icon(cx + 9, cy + 9, 16, inv->carried.item);
		}
	}

	/* ===== pass C: flat stack counts ===== */
	tev_flat();
	if (!invOpen) {
		int j;
		for (j = 0; j < INV_HOTBAR_SIZE; j++) {
			ItemStack *s = &inv->main[j];
			if (!ItemStack_IsEmpty(s) && s->count > 1)
				draw_count(hbLeft + 3 + j * 20 + 16, hbTop + 3 + 16, 1.0f, s->count);
		}
	} else {
		float gl = (gw - 176) / 2, gt = (gh - 166) / 2;
		int i;
		for (i = 0; i < INV_MAIN_SIZE; i++) {
			ItemStack *s = &inv->main[i];
			if (ItemStack_IsEmpty(s) || s->count <= 1) continue;
			float cx, cy; inv_cell(i, gl, gt, &cx, &cy);
			draw_count(cx + 1 + 16, cy + 1 + 16, 1.0f, s->count);
		}
		if (inv->carried.count > 1 && cursorSlot >= 0 && cursorSlot < INV_MAIN_SIZE) {
			float cx, cy; inv_cell(cursorSlot, gl, gt, &cx, &cy);
			draw_count(cx + 9 + 16, cy + 9 + 16, 1.0f, inv->carried.count);
		}
	}

	/* --- leave 2D: restore the world's render pipeline --- */
	World_SetupRenderState();
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
}

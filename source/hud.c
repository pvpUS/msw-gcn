#include <math.h>
#include <string.h>
#include <malloc.h>     /* mallinfo, for the heap-used reading */
#include <gccore.h>
#include <ogc/tpl.h>

#include "hud.h"
#include "world.h"
#include "net.h"
#include "inventory.h"
#include "atlas_gen.h"   /* ATLAS_COLS/CELL/PAD/TILE/TEX_W/TEX_H */
#include "font_gen.h"    /* generated: FONT_*, g_fontWidth[]     */
#include "font_tpl.h"    /* generated: font_tpl[], font_tpl_size  */

/* Dedicated 2D vertex format so we never disturb the world's GX_VTXFMT0.
 * POS as 2D floats + RGBA8 vertex colour + float texcoords, matching the
 * canonical GameCube sprite setup (see .gc-examples gxSprites). A glyph quad
 * is exactly this layout too, so text needs no format of its own -- only a
 * second GXTexObj. (Formats 0/1/2 are world/HUD/helditem.) */
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

/* ---- text (FontRenderer) ----------------------------------------------- */

static TPLFile  fontTPL;
static GXTexObj fontTex;

/* FontRenderer's 16 colour codes, built exactly as its constructor does:
 * bit3 = "bright" (+85 to every channel), bits 2/1/0 = R/G/B at 170, with
 * gold (6) getting an extra +85 red. Index = the hex digit after a section
 * sign. Stored 0xRRGGBB; alpha comes from the caller's colour. */
static u32 color_code(int i) {
	int j  = ((i >> 3) & 1) * 85;
	int r  = ((i >> 2) & 1) * 170 + j;
	int g  = ((i >> 1) & 1) * 170 + j;
	int b  = ((i >> 0) & 1) * 170 + j;
	if (i == 6) r += 85;
	return ((u32)(r & 255) << 16) | ((u32)(g & 255) << 8) | (u32)(b & 255);
}

/* A character's cell in ascii.png. For the printable range this engine draws,
 * FontRenderer's ordering string puts a character at its own code (see
 * tools/build_font.py); anything else is folded to '?' rather than sampling a
 * cell we have no width for. Chat text arrives already folded to printable
 * ASCII by the proxy, so this is a backstop, not the common path. */
static inline int glyph_index(unsigned char c) {
	if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) return '?' - FONT_FIRST_CHAR;
	return c - FONT_FIRST_CHAR;
}

/* Section sign (0xA7), the colour-code escape. */
#define FONT_ESC 0xA7

static inline int is_hex_digit(unsigned char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
	       (c >= 'A' && c <= 'F');
}

/* Consume a formatting code at `s` (which points at the character *after* the
 * section sign). Returns 1 if it was one, updating *rgb when it set a colour.
 * The style codes (k/l/m/n/o obfuscated..italic) and r (reset) are recognised
 * and swallowed; only colour is actually applied. */
static int format_code(unsigned char c, u32 base, u32 *rgb) {
	if (is_hex_digit(c)) {
		int v = (c <= '9') ? c - '0' : (c | 32) - 'a' + 10;
		*rgb = color_code(v);
		return 1;
	}
	if (c == 'r' || c == 'R') { *rgb = base >> 8; return 1; }
	return (c | 32) >= 'k' && (c | 32) <= 'o';
}

int Hud_StringWidth(const char *s) {
	int w = 0;
	const unsigned char *p = (const unsigned char *)s;
	for (; *p; p++) {
		if (*p == FONT_ESC && p[1]) {
			u32 dummy = 0;
			if (format_code(p[1], 0, &dummy)) { p++; continue; }
		}
		w += g_fontWidth[glyph_index(*p)];
	}
	return w;
}

/* One 8x8 glyph quad at the pen position. The quad is always FONT_CELL square
 * even though the advance is narrower, so glyphs that overhang their own
 * width (the tail of a 'j', say) are not clipped -- vanilla does the same. */
static void glyph_quad(float x, float y, int gi, u8 r, u8 g, u8 b, u8 a) {
	int cell = gi + FONT_FIRST_CHAR;
	float u0 = (float)(cell % FONT_GRID * FONT_CELL) / FONT_TEX_W;
	float v0 = (float)(cell / FONT_GRID * FONT_CELL) / FONT_TEX_H;
	/* 7.99 rather than 8, exactly as FontRenderer.renderDefaultChar: it keeps
	 * the sample off the shared edge with the next cell. */
	float u1 = u0 + 7.99f / FONT_TEX_W;
	float v1 = v0 + 7.99f / FONT_TEX_H;
	float s = (float)FONT_CELL;
	GX_Position2f32(x,     y    ); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u0, v0);
	GX_Position2f32(x + s, y    ); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u1, v0);
	GX_Position2f32(x + s, y + s); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u1, v1);
	GX_Position2f32(x,     y + s); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u0, v1);
}

/* Number of glyph quads `s` will emit, so the whole string goes into one
 * GX_Begin instead of one per character. */
static int glyph_count(const char *s) {
	int n = 0;
	const unsigned char *p = (const unsigned char *)s;
	for (; *p; p++) {
		if (*p == FONT_ESC && p[1]) {
			u32 dummy = 0;
			if (format_code(p[1], 0, &dummy)) { p++; continue; }
		}
		n++;
	}
	return n;
}

int Hud_DrawString(const char *s, int x, int y, u32 rgba) {
	int n = glyph_count(s);
	if (n <= 0) return x;

	tev_tex();
	GX_LoadTexObj(&fontTex, GX_TEXMAP0);

	u32 rgb = rgba >> 8;
	u8  a   = (u8)(rgba & 0xFF);
	float pen = (float)x;

	GX_Begin(GX_QUADS, HUD_FMT, (u16)(4 * n));
	const unsigned char *p = (const unsigned char *)s;
	for (; *p; p++) {
		if (*p == FONT_ESC && p[1] && format_code(p[1], rgba, &rgb)) { p++; continue; }
		int gi = glyph_index(*p);
		glyph_quad(pen, (float)y, gi,
		           (u8)(rgb >> 16), (u8)(rgb >> 8), (u8)rgb, a);
		pen += g_fontWidth[gi];
	}
	GX_End();
	return (int)pen;
}

/* Several strings in a single GX_Begin. One glyph quad is ~24 bytes through
 * the immediate-mode path and a GX_Begin costs a command header and a pipeline
 * sync either way, so a screen of nametags batched like this is one submission
 * instead of sixteen. Text is the predictable hot spot on this target -- see
 * the perf section of the plan -- and this is the cheapest of the mitigations.
 *
 * The runs must already be positioned; this only concatenates them. */
typedef struct { const char *s; int x, y; u32 rgba; } TextRun;

static void draw_runs(const TextRun *runs, int n) {
	int total = 0, i;
	for (i = 0; i < n; i++) total += glyph_count(runs[i].s);
	if (total <= 0) return;

	tev_tex();
	GX_LoadTexObj(&fontTex, GX_TEXMAP0);
	GX_Begin(GX_QUADS, HUD_FMT, (u16)(4 * total));
	for (i = 0; i < n; i++) {
		u32 rgb = runs[i].rgba >> 8;
		u8  a   = (u8)(runs[i].rgba & 0xFF);
		float pen = (float)runs[i].x;
		const unsigned char *p = (const unsigned char *)runs[i].s;
		for (; *p; p++) {
			if (*p == FONT_ESC && p[1] &&
			    format_code(p[1], runs[i].rgba, &rgb)) { p++; continue; }
			int gi = glyph_index(*p);
			glyph_quad(pen, (float)runs[i].y, gi,
			           (u8)(rgb >> 16), (u8)(rgb >> 8), (u8)rgb, a);
			pen += g_fontWidth[gi];
		}
	}
	GX_End();
}

int Hud_DrawStringShadow(const char *s, int x, int y, u32 rgba) {
	/* FontRenderer.drawString's drop shadow: the same text one pixel down-right
	 * at a quarter brightness, alpha unchanged. */
	u32 dark = ((rgba >> 2) & 0x3F3F3F00u) | (rgba & 0xFFu);
	Hud_DrawString(s, x + 1, y + 1, dark);
	return Hud_DrawString(s, x, y, rgba);
}

/* ---- small integer formatting (no stdio in the render path) ------------- */

/* Right-aligned decimal into `buf`; returns a pointer into it. Used for stack
 * counts and the perf overlay, both of which run every frame. */
static char *fmt_int(char *buf, int size, int v) {
	char *p = buf + size - 1;
	int neg = v < 0;
	unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
	*p = '\0';
	do { *--p = (char)('0' + u % 10); u /= 10; } while (u && p > buf + 1);
	if (neg && p > buf) *--p = '-';
	return p;
}

/* Append `s` to buf at *pos, stopping at the end. The overlays build their
 * lines every frame, so this stays away from stdio. */
static void app_str(char *buf, int cap, int *pos, const char *s) {
	while (*s && *pos < cap - 1) buf[(*pos)++] = *s++;
	buf[*pos] = '\0';
}

static void app_int(char *buf, int cap, int *pos, int v) {
	char tmp[12];
	app_str(buf, cap, pos, fmt_int(tmp, sizeof tmp, v));
}

/* One decimal place, which is the useful resolution for a 16.6 ms budget. */
static void app_ms(char *buf, int cap, int *pos, float ms) {
	if (ms < 0.0f) ms = 0.0f;
	int t = (int)(ms * 10.0f + 0.5f);
	app_int(buf, cap, pos, t / 10);
	app_str(buf, cap, pos, ".");
	app_int(buf, cap, pos, t % 10);
}

/* Bytes as KB, the unit every budget in the plan is quoted in. */
static void app_kb(char *buf, int cap, int *pos, u32 bytes) {
	app_int(buf, cap, pos, (int)(bytes / 1024));
	app_str(buf, cap, pos, "K");
}

/* Stack count over an item icon: right-aligned to rx with its baseline where
 * GuiIngame puts it (the icon's bottom edge less 7), with the font's own drop
 * shadow. */
static void draw_count(float rx, float by, int value) {
	char buf[12];
	char *s = fmt_int(buf, sizeof buf, value);
	Hud_DrawStringShadow(s, (int)rx - Hud_StringWidth(s), (int)by - 7,
	                     0xFFFFFFFFu);
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

	/* The glyph sheet is its own small I4 texture rather than atlas tiles --
	 * 256 cells would have eaten most of the atlas's remaining space, and the
	 * atlas's padded 16x16 cell grid can't express an 8x8 glyph layout anyway.
	 * Point sampling and no mipmaps: text is drawn at an integer GUI scale and
	 * any filtering just blurs it. */
	TPL_OpenTPLFromMemory(&fontTPL, (void *)font_tpl, font_tpl_size);
	TPL_GetTexture(&fontTPL, 0, &fontTex);
	GX_InitTexObjFilterMode(&fontTex, GX_NEAR, GX_NEAR);
	GX_InitTexObjWrapMode(&fontTex, GX_CLAMP, GX_CLAMP);
}

HudScreen Hud_Screen(int fbWidth, int efbHeight) {
	/* GUI scale a la ScaledResolution: work in a virtual space ~1/2..1/3 the
	 * framebuffer so HUD elements stay a sensible size across video modes. */
	HudScreen sc;
	sc.scale = efbHeight / 240;
	if (sc.scale < 1) sc.scale = 1;
	if (sc.scale > 3) sc.scale = 3;
	sc.w = (float)fbWidth  / sc.scale;
	sc.h = (float)efbHeight / sc.scale;
	return sc;
}

HudScreen Hud_Begin2D(int fbWidth, int efbHeight) {
	HudScreen sc = Hud_Screen(fbWidth, efbHeight);

	Mtx44 proj;
	guOrtho(proj, 0, sc.h, 0, sc.w, 0, 300);
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
	return sc;
}

void Hud_End2D(void) {
	World_SetupRenderState();
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
}

void Hud_DrawPanel(float x, float y, float w, float h, u32 rgba) {
	tev_flat();
	rect(x, y, w, h, (u8)(rgba >> 24), (u8)(rgba >> 16), (u8)(rgba >> 8),
	     (u8)rgba);
}

void Hud_Draw(Player *p, int fbWidth, int efbHeight, int invOpen, int cursorSlot) {
	Inventory *inv = &p->inventory;

	HudScreen sc = Hud_Begin2D(fbWidth, efbHeight);
	float gw = sc.w, gh = sc.h;

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

	/* ===== pass C: stack counts, in the real font ===== */
	if (!invOpen) {
		int j;
		for (j = 0; j < INV_HOTBAR_SIZE; j++) {
			ItemStack *s = &inv->main[j];
			if (!ItemStack_IsEmpty(s) && s->count > 1)
				draw_count(hbLeft + 3 + j * 20 + 16, hbTop + 3 + 16, s->count);
		}
	} else {
		float gl = (gw - 176) / 2, gt = (gh - 166) / 2;
		int i;
		for (i = 0; i < INV_MAIN_SIZE; i++) {
			ItemStack *s = &inv->main[i];
			if (ItemStack_IsEmpty(s) || s->count <= 1) continue;
			float cx, cy; inv_cell(i, gl, gt, &cx, &cy);
			draw_count(cx + 1 + 16, cy + 1 + 16, s->count);
		}
		if (inv->carried.count > 1 && cursorSlot >= 0 && cursorSlot < INV_MAIN_SIZE) {
			float cx, cy; inv_cell(cursorSlot, gl, gt, &cx, &cy);
			draw_count(cx + 9 + 16, cy + 9 + 16, inv->carried.count);
		}
	}

	Hud_End2D();
}

/* ---- nametags (T10) ----------------------------------------------------- */

void Hud_DrawTags(const HudTag *tags, int n, int fbWidth, int efbHeight) {
	if (n <= 0) return;
	if (n > HUD_TAG_MAX) n = HUD_TAG_MAX;

	Hud_Begin2D(fbWidth, efbHeight);

	/* Plates first, in one flat pass, then every name in one textured one.
	 * Vanilla puts the same translucent plate behind a nametag and for the
	 * same reason: 8-pixel glyphs over a bright skywars map are otherwise
	 * unreadable at 480p. The plate also means no drop shadow is needed,
	 * which halves the glyph count. */
	TextRun run[HUD_TAG_MAX];
	int i;

	tev_flat();
	for (i = 0; i < n; i++) {
		int w = Hud_StringWidth(tags[i].text);
		int x = tags[i].x - w / 2;
		int y = tags[i].y - 8;
		rect((float)(x - 1), (float)(y - 1), (float)(w + 2), 10.0f, 0, 0, 0, 110);
		run[i].s = tags[i].text;
		run[i].x = x;
		run[i].y = y;
		run[i].rgba = tags[i].colour;
	}
	draw_runs(run, n);

	Hud_End2D();
}

/* ---- network HUD: chat, action bar, XP (T10) ---------------------------- */

void Hud_NetInit(HudNet *n) {
	memset(n, 0, sizeof(*n));
	n->barColour = 0xFF;
}

static void chat_push(HudNet *n, u8 colour, const char *s, int len) {
	if (len >= HUD_CHAT_WIDTH) len = HUD_CHAT_WIDTH - 1;
	if (len < 0) len = 0;
	memcpy(n->line[n->head], s, len);
	n->line[n->head][len] = '\0';
	n->lineColour[n->head] = colour;
	n->head = (n->head + 1) % HUD_CHAT_LINES;
	if (n->count < HUD_CHAT_LINES) n->count++;
	/* Only counted as unread if nobody was looking. Without this the indicator
	 * would tick up while the log is open, which is the one time it is wrong. */
	if (!n->show && n->unread < 999) n->unread++;
}

void Hud_NetChat(HudNet *n, u8 colour, const char *text, int len) {
	int i = 0;
	if (len <= 0) return;
	/* Wrapped at a fixed character count rather than a measured width: the
	 * ring is written when a line arrives and drawn at whatever GUI scale is
	 * current, and HUD_CHAT_WIDTH is sized for the narrowest of them. */
	while (i < len) {
		int room = HUD_CHAT_WIDTH - 1;
		int take = (len - i < room) ? (len - i) : room;
		if (i + take < len) {
			int b = take;
			while (b > 1 && text[i + b] != ' ') b--;
			if (b > room / 3) take = b;      /* break on a word if there is one */
		}
		chat_push(n, colour, text + i, take);
		i += take;
		while (i < len && text[i] == ' ') i++;
	}
}

void Hud_NetActionBar(HudNet *n, u8 colour, const char *text, int len) {
	if (len < 0) len = 0;
	if (len > HUD_BAR_TEXT - 1) len = HUD_BAR_TEXT - 1;
	memcpy(n->bar, text, len);
	n->bar[len] = '\0';
	n->barColour = colour;
	n->barTicks  = len ? HUD_BAR_TICKS : 0;
}

void Hud_NetTick(HudNet *n) {
	if (n->barTicks > 0) n->barTicks--;
	if (n->show) n->unread = 0;
}

/* An MC colour-code index as 0xRRGGBBAA, or white for 0xFF (no code). */
static u32 code_rgba(u8 code, u8 alpha) {
	u32 rgb = (code > 15) ? 0xFFFFFFu : color_code(code);
	return (rgb << 8) | alpha;
}

void Hud_DrawNetOverlay(const HudNet *n, int fbWidth, int efbHeight) {
	HudScreen sc = Hud_Begin2D(fbWidth, efbHeight);
	float gw = sc.w, gh = sc.h;

	TextRun run[HUD_CHAT_SHOWN + 4];
	int nr = 0;
	char xpBuf[24], unreadBuf[24];

	/* ===== pass A: the panels ===== */
	tev_flat();

	int shown = 0;
	float chatTop = 0.0f;
	if (n->show && n->count) {
		shown = (n->count < HUD_CHAT_SHOWN) ? n->count : HUD_CHAT_SHOWN;
		chatTop = gh - 46.0f - shown * 9.0f;
		rect(1.0f, chatTop - 2.0f, gw * 0.66f, shown * 9.0f + 4.0f, 0, 0, 0, 160);
	}

	/* ===== pass B: the text, all of it in one batch ===== */
	int i;
	for (i = 0; i < shown; i++) {
		/* Oldest of the visible lines first. head is one past the newest, so
		 * the window starts `shown` entries back from it. */
		int idx = (n->head - shown + i + HUD_CHAT_LINES) % HUD_CHAT_LINES;
		run[nr].s = n->line[idx];
		run[nr].x = 3;
		run[nr].y = (int)chatTop + i * 9;
		run[nr].rgba = code_rgba(n->lineColour[idx], 255);
		nr++;
	}

	/* The action bar: one centred line, always on, fading out over its last
	 * 20 ticks. This is where anything the player must not miss belongs --
	 * the chat log is hidden, so it cannot be. */
	if (n->barTicks > 0 && n->bar[0]) {
		int a = (n->barTicks >= HUD_BAR_FADE)
		      ? 255 : (n->barTicks * 255 / HUD_BAR_FADE);
		run[nr].s = n->bar;
		run[nr].x = (int)(gw / 2) - Hud_StringWidth(n->bar) / 2;
		run[nr].y = (int)gh - 60;
		run[nr].rgba = code_rgba(n->barColour, (u8)a);
		nr++;
	}

	/* XP level is the ranked elo on this server, not experience. */
	if (n->xpLevel > 0) {
		int p = 0;
		app_str(xpBuf, sizeof xpBuf, &p, "elo ");
		app_int(xpBuf, sizeof xpBuf, &p, n->xpLevel);
		run[nr].s = xpBuf;
		run[nr].x = 3;
		run[nr].y = (int)gh - 11;
		run[nr].rgba = 0x55FF55FFu;
		nr++;
	}

	/* Unread indicator. One glyph's worth of screen, and without it hidden
	 * chat means silently missing every game announcement. */
	if (!n->show && n->unread > 0) {
		int p = 0;
		app_str(unreadBuf, sizeof unreadBuf, &p, "\xA7" "e* ");
		app_int(unreadBuf, sizeof unreadBuf, &p,
		        n->unread > 99 ? 99 : n->unread);
		app_str(unreadBuf, sizeof unreadBuf, &p, " chat");
		run[nr].s = unreadBuf;
		run[nr].x = 3;
		run[nr].y = (int)gh - 21;
		run[nr].rgba = 0xFFFFFFC0u;
		nr++;
	}

	/* Spectator banner. The plugin cancels lethal damage and drops you into
	 * spectator instead of killing you (T26), so this is the only signal that
	 * anything happened -- there is no death screen to wait for.
	 *
	 * Drawn where the hotbar was, which in spectator is empty screen: at the
	 * top it would sit under the perf overlay's nine lines, and the one line
	 * that says "you are dead" is a poor thing to have to read around. */
	if (n->gameMode == 3) {
		static const char *SPEC = "SPECTATING";
		run[nr].s = SPEC;
		run[nr].x = (int)(gw / 2) - Hud_StringWidth(SPEC) / 2;
		run[nr].y = (int)gh - 24;
		run[nr].rgba = 0xAAAAAAFFu;
		nr++;
	}

	draw_runs(run, nr);
	Hud_End2D();
}

/* ---- perf overlay (T27) ------------------------------------------------- */

/* Exponential mean over roughly the last second at 60 Hz. A plain mean over
 * the whole session goes flat and stops reporting; the maxima below are what
 * catch a one-frame spike. */
#define PERF_SMOOTH 0.05f

void Hud_PerfInit(HudPerf *pf) {
	memset(pf, 0, sizeof(*pf));
	pf->heapFree = Hud_HeapFree();
	pf->heapUsed = pf->heapUsedMax = Hud_HeapUsed();
}

void Hud_PerfSample(HudPerf *pf, double frameUs, double tickUs,
                    const World *w, u32 entities) {
	pf->frameMs = (float)(frameUs / 1000.0);
	pf->tickMs  = (float)(tickUs / 1000.0);
	pf->frameMsAvg += (pf->frameMs - pf->frameMsAvg) * PERF_SMOOTH;
	if (pf->frameMs > pf->frameMsMax) pf->frameMsMax = pf->frameMs;
	if (pf->tickMs  > pf->tickMsMax)  pf->tickMsMax  = pf->tickMs;

	pf->heapFree = Hud_HeapFree();
	pf->heapUsed = Hud_HeapUsed();
	if (pf->heapUsed > pf->heapUsedMax) pf->heapUsedMax = pf->heapUsed;
	pf->entities = entities;
	if (w) World_GetStats(w, &pf->w);
}

u32 Hud_HeapFree(void) {
	return (u32)((char *)SYS_GetArena1Hi() - (char *)SYS_GetArena1Lo());
}

u32 Hud_HeapUsed(void) {
	return (u32)mallinfo().uordblks;
}

void Hud_DrawPerf(const HudPerf *pf, int fbWidth, int efbHeight) {
	HudScreen sc = Hud_Begin2D(fbWidth, efbHeight);

	char line[80];
	int  n;
	int  x = 2, y = 2;
	const int lh = 9;                    /* 8px glyph + 1px leading */

	/* Panel behind the text: 480p is a noisy background for 8px glyphs. */
	tev_flat();
	rect(0, 0, 132, lh * 8 + 4, 0, 0, 0, 150);

	/* Free heap first: it is the number that governs everything else. Amber
	 * under 4 MB (the entity/network/font reserve the plan budgets for), red
	 * under 2 MB (the end-to-end floor). */
	u32 kbFree = pf->heapFree / 1024;
	u32 heapColor = kbFree < 2048 ? 0xFF5555FFu
	              : kbFree < 4096 ? 0xFFAA00FFu : 0x55FF55FFu;
	n = 0;
	app_str(line, sizeof line, &n, "free ");
	app_kb(line, sizeof line, &n, pf->heapFree);
	app_str(line, sizeof line, &n, " used ");
	app_kb(line, sizeof line, &n, pf->heapUsed);
	app_str(line, sizeof line, &n, " pk ");
	app_kb(line, sizeof line, &n, pf->heapUsedMax);
	Hud_DrawStringShadow(line, x, y, heapColor); y += lh;

	/* A vsync-locked frame is 16.67 ms, so that is the healthy reading, not the
	 * failure one -- the next step up is a dropped field at 33.3 ms. Flag past
	 * 18 ms, which can only mean fields are starting to slip. */
	n = 0;
	app_str(line, sizeof line, &n, "frame ");
	app_ms(line, sizeof line, &n, pf->frameMsAvg);
	app_str(line, sizeof line, &n, " max ");
	app_ms(line, sizeof line, &n, pf->frameMsMax);
	Hud_DrawStringShadow(line, x, y,
	                     pf->frameMsAvg > 18.0f ? 0xFF5555FFu : 0xFFFFFFFFu);
	y += lh;

	n = 0;
	app_str(line, sizeof line, &n, "tick ");
	app_ms(line, sizeof line, &n, pf->tickMs);
	app_str(line, sizeof line, &n, " max ");
	app_ms(line, sizeof line, &n, pf->tickMsMax);
	Hud_DrawStringShadow(line, x, y, 0xFFFFFFFFu); y += lh;

	n = 0;
	app_str(line, sizeof line, &n, "remesh ");
	app_ms(line, sizeof line, &n, pf->w.remeshMs);
	app_str(line, sizeof line, &n, " max ");
	app_ms(line, sizeof line, &n, pf->w.remeshMsMax);
	Hud_DrawStringShadow(line, x, y, 0xFFFFFFFFu); y += lh;

	/* The deferred re-mesh queue (T24): `dirty` is the meshes still owed after
	 * this frame's World_FlushRemesh -- non-zero for the few frames after a
	 * network block batch, and stuck non-zero means the flush is not keeping
	 * up. `chunk` is the worst single chunk, which is the number T24's 4 ms
	 * budget is actually stated against (the line above is a whole call, and a
	 * call re-meshes several). */
	n = 0;
	app_str(line, sizeof line, &n, "dirty ");
	app_int(line, sizeof line, &n, (int)pf->w.dirtyChunks);
	app_str(line, sizeof line, &n, " chunk ");
	app_ms(line, sizeof line, &n, pf->w.remeshChunkMsMax);
	Hud_DrawStringShadow(line, x, y,
	                     pf->w.remeshChunkMsMax > 4.0f ? 0xFFAA00FFu : 0xFFFFFFFFu);
	y += lh;

	/* Display lists: drawn / total chunks, and what they cost. `dl` is the
	 * allocation; `use` is what was actually recorded into it, so the gap is
	 * the over-allocation T4 trims. */
	n = 0;
	app_str(line, sizeof line, &n, "chunk ");
	app_int(line, sizeof line, &n, (int)pf->w.chunksDrawn);
	app_str(line, sizeof line, &n, "/");
	app_int(line, sizeof line, &n, (int)pf->w.chunks);
	app_str(line, sizeof line, &n, " dl ");
	app_kb(line, sizeof line, &n, pf->w.dlBytes);
	app_str(line, sizeof line, &n, " use ");
	app_kb(line, sizeof line, &n, pf->w.dlUsed);
	Hud_DrawStringShadow(line, x, y, 0xFFFFFFFFu); y += lh;

	/* Both indexed arrays are GX_INDEX16-addressed and append-only: past
	 * 65535 entries a new tile would silently alias an existing one. */
	n = 0;
	app_str(line, sizeof line, &n, "clr ");
	app_int(line, sizeof line, &n, (int)pf->w.clrCount);
	app_str(line, sizeof line, &n, " tex ");
	app_int(line, sizeof line, &n, (int)pf->w.texCount);
	app_str(line, sizeof line, &n, "/65535");
	Hud_DrawStringShadow(line, x, y,
	                     pf->w.texCount > 60000 ? 0xFF5555FFu : 0xFFFFFFFFu);
	y += lh;

	n = 0;
	app_str(line, sizeof line, &n, "face ");
	app_int(line, sizeof line, &n, (int)pf->w.faces);
	app_str(line, sizeof line, &n, " ent ");
	app_int(line, sizeof line, &n, (int)pf->entities);
	app_str(line, sizeof line, &n, " edit ");
	app_int(line, sizeof line, &n, (int)pf->w.edits);
	Hud_DrawStringShadow(line, x, y, 0xFFFFFFFFu);

	(void)sc;
	Hud_End2D();
}

/* ---- connection indicator (T3) ------------------------------------------ */

void Hud_DrawNetStatus(int fbWidth, int efbHeight) {
	NetState st = Net_GetState();
	if (st == NET_DOWN && Net_LastError()[0] == '\0') return;

	HudScreen sc = Hud_Begin2D(fbWidth, efbHeight);

	char line[64];
	int n = 0;
	app_str(line, sizeof line, &n, "net ");
	app_str(line, sizeof line, &n, Net_StateText());
	if (st == NET_READY) {
		app_str(line, sizeof line, &n, " ");
		app_int(line, sizeof line, &n, (int)Net_RttMs());
		app_str(line, sizeof line, &n, "ms");
	} else if (Net_LastError()[0]) {
		app_str(line, sizeof line, &n, ": ");
		app_str(line, sizeof line, &n, Net_LastError());
	}

	u32 color = (st == NET_READY)  ? 0x55FF55FFu
	          : (st == NET_DOWN)   ? 0xFF5555FFu
	          : (st == NET_IDLE)   ? 0xFFAA00FFu : 0xFFFF55FFu;

	int w = Hud_StringWidth(line);
	int x = (int)sc.w - w - 3;
	tev_flat();
	rect((float)(x - 2), 1.0f, (float)(w + 4), 10.0f, 0, 0, 0, 150);
	Hud_DrawStringShadow(line, x, 2, color);

	Hud_End2D();
}

#ifndef MSW_HUD_H
#define MSW_HUD_H

#include <gccore.h>
#include "player.h"
#include "world.h"   /* WorldStats, for the perf overlay */

/* First-person HUD: hotbar, held-slot highlight, health hearts, a crosshair,
 * and a toggleable full inventory screen -- modeled on GuiIngame /
 * GuiInventory (1.8.9). Drawn as an orthographic 2D overlay on top of the 3D
 * world; item icons reuse the block-texture atlas (a block's global id is its
 * atlas side tile).
 *
 * The inventory screen is interactive: a slot cursor stands in for the mouse
 * pointer (there isn't one on a GameCube pad), and the stack being carried
 * between slots -- InventoryPlayer.itemStack, what the mouse would be dragging
 * -- is drawn following that cursor. See Inventory_SlotClick for the actual
 * move/merge/swap rules. */

/* One-time GX setup: a dedicated 2D vertex format, and the glyph sheet. Call
 * once after World_InitGX. */
void Hud_InitGX(void);

/* ---- the 2D overlay pass ------------------------------------------------
 * Anything drawn on top of the world -- this HUD, the perf overlay, the
 * command palette -- shares one orthographic pass in a virtual "gui space"
 * (ScaledResolution): ~1/2 to 1/3 the framebuffer, so elements keep a sensible
 * size across video modes. Gui-space y grows downward. */
typedef struct {
	float w, h;   /* gui-space extent                                       */
	int   scale;  /* framebuffer pixels per gui pixel (1..3)                 */
} HudScreen;

/* Load the ortho projection and 2D pipeline state, and report the gui-space
 * geometry. fbWidth/efbHeight are the current EFB dimensions (the same values
 * passed to GX_SetViewport). */
HudScreen Hud_Begin2D(int fbWidth, int efbHeight);

/* Restore the world's render state (World_SetupRenderState + z-test). The
 * caller must reload the perspective projection before the next World_Draw. */
void Hud_End2D(void);

/* ---- text (FontRenderer) ------------------------------------------------
 * Minecraft 1.8.9's ascii.png, baked to an I4 texture (data/font.tpl) with
 * vanilla's variable glyph advances (source/font_gen.h). Draws in gui space
 * between Hud_Begin2D and Hud_End2D; glyphs are 8 gui-pixels tall.
 *
 * Colours are 0xRRGGBBAA. A section sign (0xA7) followed by 0-9/a-f switches
 * colour mid-string exactly as vanilla does, 'r' resets to the passed colour,
 * and k-o (the style codes) are swallowed rather than drawn -- the proxy
 * strips section signs before sending chat, so this is for locally built
 * strings and for anything that slips through.
 *
 * A whole string is one GX_Begin. Both draw calls leave the font bound and the
 * TEV stage in modulate mode, so re-bind the atlas (World_BindAtlas) before
 * drawing block icons again. */
int Hud_DrawString(const char *s, int x, int y, u32 rgba);

/* Same, with vanilla's drop shadow: the string again at +1,+1 and a quarter
 * brightness, drawn first. */
int Hud_DrawStringShadow(const char *s, int x, int y, u32 rgba);

/* FontRenderer.getStringWidth: the pen advance for the whole string, with
 * formatting codes excluded. */
int Hud_StringWidth(const char *s);

/* ---- perf overlay (T27) -------------------------------------------------
 * Every budget in the BBA plan -- peak heap, frame time, remesh cost per
 * chunk, the 65535 GX_INDEX16 vertex-array ceiling -- is stated against one of
 * these numbers, so they are read live off the screen rather than inferred.
 * Free heap is the one that governs the rest: this is a 24 MB machine with no
 * headroom to buy later.
 *
 * main.c owns the timing; hud.c owns the layout. Draws its own 2D pass, so it
 * composes with (or without) Hud_Draw. */
typedef struct {
	float frameMs, frameMsAvg, frameMsMax;  /* wall time for a whole frame  */
	float tickMs,  tickMsMax;               /* the 20 Hz sim inside it      */
	u32   heapFree;      /* SYS_GetArena1Hi - Lo, bytes                     */
	u32   heapLow;       /* the least free it has ever been                 */
	u32   entities;      /* live entities (item drops today; T9 grows this) */
	WorldStats w;        /* World_GetStats                                  */
} HudPerf;

/* Zero the struct and stamp the heap baseline. */
void Hud_PerfInit(HudPerf *pf);

/* Fold one frame's measurements in: `frameUs`/`tickUs` are that frame's wall
 * and simulation time, `w` the world it drew. Keeps the rolling mean and the
 * maxima, and re-reads the heap. */
void Hud_PerfSample(HudPerf *pf, double frameUs, double tickUs,
                    const World *w, u32 entities);

/* Draw the overlay top-left, in its own 2D pass. */
void Hud_DrawPerf(const HudPerf *pf, int fbWidth, int efbHeight);

/* Draw the HUD for `p`. fbWidth/efbHeight are the current EFB dimensions (the
 * same values passed to GX_SetViewport). `invOpen` shows the full inventory
 * screen, with `cursorSlot` (a main-inventory index 0-35) highlighted. Leaves
 * the GX pipeline restored to the world's render state (via
 * World_SetupRenderState); the caller must reload the perspective projection
 * before the next World_Draw. */
void Hud_Draw(Player *p, int fbWidth, int efbHeight, int invOpen, int cursorSlot);

#endif

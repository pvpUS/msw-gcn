#ifndef MSW_HUD_H
#define MSW_HUD_H

#include <gccore.h>
#include "player.h"

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

/* One-time GX setup (a dedicated 2D vertex format). Call once after
 * World_InitGX. */
void Hud_InitGX(void);

/* Draw the HUD for `p`. fbWidth/efbHeight are the current EFB dimensions (the
 * same values passed to GX_SetViewport). `invOpen` shows the full inventory
 * screen, with `cursorSlot` (a main-inventory index 0-35) highlighted. Leaves
 * the GX pipeline restored to the world's render state (via
 * World_SetupRenderState); the caller must reload the perspective projection
 * before the next World_Draw. */
void Hud_Draw(Player *p, int fbWidth, int efbHeight, int invOpen, int cursorSlot);

#endif

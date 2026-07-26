#ifndef MSW_HELDITEM_H
#define MSW_HELDITEM_H

#include <gccore.h>
#include "player.h"

/* First-person "held item": the block or item in the currently selected hotbar
 * slot, drawn as a small 3D overlay in the lower-right of the screen the way
 * Minecraft renders the item in your hand. Blocks render as a shaded 3D cube
 * (per-face atlas tiles + Minecraft's directional face shading); flat items
 * (e.g. the diamond sword, whose atlas tile index is >= NUM_BLOCK_IDS) render
 * as a single upright textured card.
 *
 * Drawn between World_Draw and Hud_Draw so it sits on top of the world but
 * under the 2D HUD, matching vanilla layering. It reuses the perspective
 * projection main.c already has loaded and the block-texture atlas; it never
 * touches the world mesh state, so World_Draw/Hud_Draw are unaffected. */

/* One-time GX setup (a dedicated vertex format). Call once after Hud_InitGX. */
void HeldItem_InitGX(void);

/* Draw the held item for `p`. fbWidth/efbHeight are the current EFB dimensions
 * (the same values passed to GX_SetViewport). No-op when the held slot is
 * empty. Leaves the GX viewport depth range restored to [0,1]; the following
 * Hud_Draw sets up all of its own remaining state. */
void HeldItem_Draw(Player *p, int fbWidth, int efbHeight);

/* ---- shared item geometry (also used for dropped items in the world) -----
 * entityitem.c renders EntityItems out of the same two meshes, just with a
 * world model-view instead of the first-person one, so they live here rather
 * than being duplicated. Both assume HeldItem_SetupGX() has run this pass and
 * that a model-view matrix is loaded in GX_PNMTX0; the mesh is a unit shape
 * centred on the origin. */

/* Vertex descriptor/format, TEV, alpha-cutout, blend and cull state the two
 * meshes below need. Does not touch the projection or viewport. */
void HeldItem_SetupGX(void);

/* Shaded, per-face-textured unit cube for a block id. */
void HeldItem_DrawBlockMesh(int item);

/* Single upright textured card for a flat item's atlas tile. */
void HeldItem_DrawFlatMesh(int tile);

#endif

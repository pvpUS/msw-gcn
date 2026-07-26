#ifndef MSW_ITEMS_H
#define MSW_ITEMS_H

#include <gccore.h>

/* net.minecraft.item.Item, for the handful of non-block items this engine has.
 *
 * An ItemStack.item below NUM_BLOCK_IDS is a placeable block (an ItemBlock in
 * vanilla terms); at or above it, the value is an appended atlas tile index
 * (ITEM_*_TILE from the generated block_book_gen.h) and names one of the items
 * described here -- the tools you mine with and the loose drops blocks turn
 * into (coal, flint, clay balls, ...). See tools/build_atlas.py's
 * ITEM_TEXTURES for the tile list.
 *
 * Only the properties the mining path actually reads are modelled: the tool
 * class and material tier that drive ItemTool.getStrVsBlock /
 * canHarvestBlock, and the max stack size. No durability -- nothing here wears
 * out, so ItemStack.onBlockDestroyed is a no-op and tools are never consumed. */

/* Tool material tiers (Item.ToolMaterial), for the tools that exist. */
#define TOOL_TIER_WOOD    0
#define TOOL_TIER_STONE   1
#define TOOL_TIER_IRON    2
#define TOOL_TIER_DIAMOND 3

typedef struct {
	s16   item;         /* the item id == its atlas tile index          */
	u8    toolClass;    /* TOOL_* from block_props_gen.h (TOOL_NONE = not a tool) */
	u8    tier;         /* ToolMaterial.getHarvestLevel()               */
	float efficiency;   /* ToolMaterial.getEfficiencyOnProperMaterial() */
	u8    maxStack;     /* Item.getItemStackLimit()                     */
} ItemDef;

/* Item definition for an ItemStack.item, or NULL when it is a block id (which
 * behaves as vanilla's plain Item: strVsBlock 1.0, harvests nothing). */
const ItemDef *Item_Def(int item);

/* Item.getItemStackLimit(): 1 for tools, 64 for everything else (including
 * blocks). */
int Item_MaxStack(int item);

/* ItemStack.getStrVsBlock(block): the dig-speed multiplier the held item
 * contributes, i.e. ItemTool/ItemSword.getStrVsBlock for the block's material.
 * `item` < 0 (empty hand) gives 1.0. */
float Item_StrVsBlock(int item, int blockId);

/* ItemStack.canHarvestBlock(block): whether holding this item lets the block
 * drop at all. Only consulted for blocks whose material setRequiresTool()
 * (rock/iron/anvil/web/snow) -- see Block_CanHarvest. */
int Item_CanHarvestBlock(int item, int blockId);

/* ---- the block side of the same question -------------------------------- */

/* Material.isLiquid(): true for water and lava, in any metadata state.
 *
 * Vanilla's liquids are *not* solid -- they have no collision box, they don't
 * occlude, and an entity inside one takes a different branch of
 * moveEntityWithHeading (swim/lava drag) instead of the ground/air one. A
 * block id outside the palette is not a liquid, so this is safe to call with
 * an unvalidated id. */
int Block_IsLiquid(int blockId);

/* EntityPlayer.canHarvestBlock -> InventoryPlayer.canHeldItemHarvest:
 * true when the block needs no tool, or the held item can harvest it. */
int Block_CanHarvest(int blockId, int heldItem);

/* Block.getPlayerRelativeBlockHardness(): the fraction of the block that one
 * tick of mining removes. Returns 0 for unbreakable blocks. `onGround` is the
 * player's state (EntityPlayer.getToolDigEfficiency divides by 5 in mid-air). */
float Block_PlayerRelativeHardness(int blockId, int heldItem, int onGround);

#endif

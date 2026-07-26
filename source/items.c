#include <stddef.h>

#include "items.h"
#include "block_props_gen.h"   /* g_blockProps[], TOOL_*, MAT_*, NUM_BLOCK_IDS */
#include "block_book_gen.h"    /* ITEM_*_TILE                                  */

/* Every non-block item, keyed by its atlas tile index. Diamond is the only
 * tool material present (Item.ToolMaterial.EMERALD in MCP names: harvest level
 * 3, efficiency 8.0); the rest are inert drops that only ever sit in a slot.
 * Anything not listed falls back to the plain-item defaults in Item_Def. */
static const ItemDef g_items[] = {
	{ ITEM_DIAMOND_SWORD_TILE,   TOOL_SWORD,   TOOL_TIER_DIAMOND, 8.0f, 1 },
	{ ITEM_DIAMOND_PICKAXE_TILE, TOOL_PICKAXE, TOOL_TIER_DIAMOND, 8.0f, 1 },
	{ ITEM_DIAMOND_AXE_TILE,     TOOL_AXE,     TOOL_TIER_DIAMOND, 8.0f, 1 },
	{ ITEM_DIAMOND_SHOVEL_TILE,  TOOL_SHOVEL,  TOOL_TIER_DIAMOND, 8.0f, 1 },
};
#define NUM_ITEM_DEFS ((int)(sizeof(g_items) / sizeof(g_items[0])))

/* Plain (non-tool) item: no dig bonus, stacks to 64 -- vanilla's Item base
 * class. Blocks take this path too, which is exactly right: ItemBlock doesn't
 * override getStrVsBlock or canHarvestBlock. */
static const ItemDef g_plainItem = { -1, TOOL_NONE, 0, 1.0f, 64 };

const ItemDef *Item_Def(int item) {
	if (item < NUM_BLOCK_IDS) return NULL;
	int i;
	for (i = 0; i < NUM_ITEM_DEFS; i++)
		if (g_items[i].item == item) return &g_items[i];
	return &g_plainItem;
}

int Item_MaxStack(int item) {
	const ItemDef *d = Item_Def(item);
	return d ? d->maxStack : 64;
}

float Item_StrVsBlock(int item, int blockId) {
	if (item < 0 || blockId < 0 || blockId >= NUM_BLOCK_IDS) return 1.0f;
	const ItemDef *d = Item_Def(item);
	if (!d || d->toolClass == TOOL_NONE) return 1.0f;   /* Item.getStrVsBlock */

	const BlockProps *b = &g_blockProps[blockId];
	if (d->toolClass == TOOL_SWORD) {
		/* ItemSword.getStrVsBlock: cobwebs come apart fast, and it snips
		 * plants/vines/leaves/gourds slightly quicker than a bare hand. */
		if (b->material == MAT_WEB) return 15.0f;
		if (b->material == MAT_PLANTS || b->material == MAT_VINE ||
		    b->material == MAT_LEAVES || b->material == MAT_GOURD) return 1.5f;
		return 1.0f;
	}
	/* ItemPickaxe/ItemAxe/ItemSpade: the efficiency bonus applies on the
	 * materials (and effectiveBlocks) folded into BlockProps.speedTool. */
	return (b->speedTool == d->toolClass) ? d->efficiency : 1.0f;
}

int Item_CanHarvestBlock(int item, int blockId) {
	if (item < 0 || blockId < 0 || blockId >= NUM_BLOCK_IDS) return 0;
	const ItemDef *d = Item_Def(item);
	if (!d) return 0;                        /* holding a block: Item.canHarvestBlock */

	const BlockProps *b = &g_blockProps[blockId];
	if (b->harvestTool == TOOL_NONE) return 1;
	if (d->toolClass != b->harvestTool) return 0;
	/* ItemPickaxe.canHarvestBlock's tier gates (obsidian needs diamond,
	 * diamond/gold need iron, iron/lapis need stone, ...) live in
	 * BlockProps.harvestLevel. */
	return d->tier >= b->harvestLevel;
}

int Block_CanHarvest(int blockId, int heldItem) {
	if (blockId < 0 || blockId >= NUM_BLOCK_IDS) return 0;
	/* InventoryPlayer.canHeldItemHarvest: material.isToolNotRequired() short-
	 * circuits before the held item is even looked at. */
	if (g_blockProps[blockId].harvestTool == TOOL_NONE) return 1;
	return Item_CanHarvestBlock(heldItem, blockId);
}

float Block_PlayerRelativeHardness(int blockId, int heldItem, int onGround) {
	if (blockId < 0 || blockId >= NUM_BLOCK_IDS) return 0.0f;
	float f = g_blockProps[blockId].hardness;
	if (f < 0.0f) return 0.0f;                       /* setBlockUnbreakable() */

	/* EntityPlayer.getToolDigEfficiency, minus the enchantment/potion/water
	 * terms this engine has no state for. */
	float dig = Item_StrVsBlock(heldItem, blockId);
	if (!onGround) dig /= 5.0f;

	/* Block.getPlayerRelativeBlockHardness: /30 when the drop is allowed,
	 * /100 (a bare-hands slog with nothing to show for it) when it isn't. */
	return Block_CanHarvest(blockId, heldItem) ? dig / f / 30.0f
	                                           : dig / f / 100.0f;
}

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

/* EnumAction: what holding the use button on this item does. Instant items
 * (a thrown pearl, a snowball, a bucket) are USE_NONE -- they act on the press
 * and there is nothing to hold. */
enum {
	USE_NONE = 0,
	USE_BOW,     /* draw; release fires. 20 ticks is a full charge   */
	USE_EAT,     /* 32 ticks, then the server applies the effect     */
	USE_DRINK,   /* 32 ticks                                         */
};

/* ItemBow.getMaxItemUseDuration is 72000, but the charge saturates at 20
 * ticks and nothing here needs to model the rest. */
#define USE_TICKS_BOW  20
#define USE_TICKS_FOOD 32

typedef struct {
	s16   item;         /* the item id == its atlas tile index          */
	u8    toolClass;    /* TOOL_* from block_props_gen.h (TOOL_NONE = not a tool) */
	u8    tier;         /* ToolMaterial.getHarvestLevel()               */
	float efficiency;   /* ToolMaterial.getEfficiencyOnProperMaterial() */
	u8    maxStack;     /* Item.getItemStackLimit()                     */

	/* ---- combat / use (T19) ----------------------------------------------
	 * `attackDamage` is ItemTool/ItemSword.attackDamage plus the player's 1.0
	 * base, i.e. what a hit with this in hand is worth. It is *not* used to
	 * predict anything: 1.8's attackEntityFrom returns early on the client, so
	 * vanilla predicts no damage, no knockback and no sprint reset, and adding
	 * any of that here would only desync. It exists so the HUD and the tooltip
	 * can say what the kit is holding.
	 *
	 * `maxDamage` is durability, 0 for indestructible. Nothing wears out here
	 * either -- the server owns the stack -- but INV_SET carries the damage
	 * value and a bow at 3/384 should not look like a fresh one. */
	float attackDamage;
	u16   maxDamage;
	u8    useAction;    /* USE_*                                        */
	u8    useDuration;  /* ticks to complete a hold, 0 = instant        */
} ItemDef;

/* Item definition for an ItemStack.item, or NULL when it is a block id (which
 * behaves as vanilla's plain Item: strVsBlock 1.0, harvests nothing). */
const ItemDef *Item_Def(int item);

/* Item.getItemStackLimit(): 1 for tools, 64 for everything else (including
 * blocks). */
int Item_MaxStack(int item);

/* EnumAction for the held item, and how many ticks a full hold takes. Both are
 * USE_NONE/0 for a block or an unknown item. */
int Item_UseAction(int item);
int Item_UseDuration(int item);

/* The melee damage a hit with this in hand is worth, in half-hearts. Display
 * only -- see the note on ItemDef.attackDamage. */
float Item_AttackDamage(int item);

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

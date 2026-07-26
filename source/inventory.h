#ifndef MSW_INVENTORY_H
#define MSW_INVENTORY_H

#include <gccore.h>

/* Player inventory, ported from net.minecraft.entity.player.InventoryPlayer
 * (Minecraft 1.8.9 / MCP-919). Same slot layout and store/merge/split logic;
 * the parts that need the full game (NBT, subtypes, damaged tools, the mouse-
 * held itemStack, armor damage, crafting) are dropped or reduced to the block-
 * item case this engine actually has.
 *
 * An "item" here is a world block's global id (the same index that world.c's
 * atlas/shape tables use), so an item stack maps 1:1 to a placeable block.
 * The world is currently unbreakable, so nothing populates the inventory yet --
 * this is the container the pickup path will fill once blocks can be mined. */

#define INV_MAIN_SIZE   36   /* mainInventory: 0-8 hotbar, 9-35 storage */
#define INV_HOTBAR_SIZE 9
#define INV_ARMOR_SIZE  4    /* armorInventory                          */
#define INV_STACK_LIMIT 64   /* getInventoryStackLimit()                */

/* One stack. stackSize (count) == 0 is the empty/"null ItemStack" sentinel --
 * item is then meaningless (kept as -1). item is a block global id; meta is the
 * block data value, retained for fidelity with ItemStack.itemDamage even though
 * block variants already have distinct global ids in this engine. */
typedef struct {
	s16 item;   /* block global id, -1 when empty          */
	s16 meta;   /* metadata / itemDamage                    */
	u8  count;  /* stackSize; 0 == empty slot              */
} ItemStack;

typedef struct {
	ItemStack main[INV_MAIN_SIZE];
	ItemStack armor[INV_ARMOR_SIZE];
	int currentItem;   /* selected hotbar slot, 0-8 (InventoryPlayer.currentItem) */

	/* InventoryPlayer.itemStack: the stack "on the cursor" while the inventory
	 * screen is open -- what a mouse would be dragging in vanilla. Empty when
	 * nothing is being carried. */
	ItemStack carried;
} Inventory;

/* True for an empty ("null") stack. */
static inline int ItemStack_IsEmpty(const ItemStack *s) { return s->count == 0; }

/* Clear all slots and reset the held-slot index (constructor + clear()). */
void Inventory_Init(Inventory *inv);

/* getStackInSlot(index): index 0..35 -> main, 36..39 -> armor. */
ItemStack *Inventory_GetStackInSlot(Inventory *inv, int index);

/* getCurrentItem(): the stack in the selected hotbar slot, or NULL if empty. */
ItemStack *Inventory_GetCurrentItem(Inventory *inv);

/* changeCurrentItem(direction): scroll the hotbar. direction > 0 selects the
 * slot to the left (decreasing index), < 0 to the right, wrapping 0..8. */
void Inventory_ChangeCurrentItem(Inventory *inv, int direction);

/* getFirstEmptyStack(): index of the first empty main slot, or -1. */
int Inventory_GetFirstEmptyStack(Inventory *inv);

/* addItemStackToInventory (undamaged path): merge `stack` into existing
 * matching stacks then empty slots. Mutates stack->count down to whatever
 * didn't fit and returns 1 if at least one item was stored, 0 otherwise. */
int Inventory_AddItemStack(Inventory *inv, ItemStack *stack);

/* Convenience wrapper: try to add `count` of (item,meta). Returns the leftover
 * count that did not fit (0 = all stored). */
int Inventory_AddItem(Inventory *inv, int item, int meta, int count);

/* decrStackSize(index, count): remove up to `count` from a slot, returning the
 * removed stack (count 0 if the slot was empty). Empties the slot if drained. */
ItemStack Inventory_DecrStackSize(Inventory *inv, int index, int count);

/* setInventorySlotContents(index, stack). */
void Inventory_SetSlot(Inventory *inv, int index, ItemStack stack);

/* Container.slotClick's PICKUP case (click type 0), the only one an inventory
 * with no crafting grid needs: exchange between slot `index` and the carried
 * stack. `rightClick` picks up half / puts down one, like the right mouse
 * button; otherwise the whole stack moves. Same-item stacks merge up to the
 * stack limit, different ones swap. Every slot here is a plain inventory slot,
 * so vanilla's isItemValid/canTakeStack tests are all trivially true. */
void Inventory_SlotClick(Inventory *inv, int index, int rightClick);

#endif

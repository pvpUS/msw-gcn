#include <string.h>
#include "inventory.h"
#include "items.h"     /* Item_MaxStack: tools don't stack (ItemStack.isStackable) */

/* Port of net.minecraft.entity.player.InventoryPlayer (MCP-919, 1.8.9). Method
 * names and control flow are kept close to the Java so they can be diffed
 * against it; see inventory.h for the block-item simplifications. */

static const ItemStack EMPTY = { -1, 0, 0 };

void Inventory_Init(Inventory *inv) {
	int i;
	for (i = 0; i < INV_MAIN_SIZE; i++)  inv->main[i]  = EMPTY;
	for (i = 0; i < INV_ARMOR_SIZE; i++) inv->armor[i] = EMPTY;
	inv->currentItem = 0;
	inv->carried = EMPTY;
}

/* getStackInSlot(index): main[0..35] then armor[0..3]. */
ItemStack *Inventory_GetStackInSlot(Inventory *inv, int index) {
	if (index < 0) return NULL;
	if (index >= INV_MAIN_SIZE) {
		index -= INV_MAIN_SIZE;
		if (index >= INV_ARMOR_SIZE) return NULL;
		return &inv->armor[index];
	}
	return &inv->main[index];
}

/* getCurrentItem(). */
ItemStack *Inventory_GetCurrentItem(Inventory *inv) {
	if (inv->currentItem < 0 || inv->currentItem >= INV_HOTBAR_SIZE) return NULL;
	ItemStack *s = &inv->main[inv->currentItem];
	return ItemStack_IsEmpty(s) ? NULL : s;
}

/* changeCurrentItem(direction). */
void Inventory_ChangeCurrentItem(Inventory *inv, int direction) {
	if (direction > 0) direction = 1;
	if (direction < 0) direction = -1;

	for (inv->currentItem -= direction; inv->currentItem < 0; inv->currentItem += 9)
		;
	while (inv->currentItem >= 9)
		inv->currentItem -= 9;
}

void Inventory_SetCurrentItem(Inventory *inv, int slot) {
	if (slot < 0 || slot >= INV_HOTBAR_SIZE) return;
	inv->currentItem = slot;
}

/* getInventoryStackLimit() capped by the item's own getMaxStackSize(): 64 for
 * blocks and loose items, 1 for tools (which is also what makes them fail
 * ItemStack.isStackable, so they never merge). */
static int stack_limit(int item) {
	int m = Item_MaxStack(item);
	return m < INV_STACK_LIMIT ? m : INV_STACK_LIMIT;
}

/* storeItemStack(itemStackIn): first main slot already holding the same item
 * with room to stack (isStackable + under the per-stack and inventory limits).
 * Blocks have no NBT/subtypes here, so the tag/subtype checks reduce to an
 * (item,meta) match. */
static int storeItemStack(Inventory *inv, const ItemStack *in) {
	int limit = stack_limit(in->item);
	if (limit <= 1) return -1;              /* !isStackable() */
	int i;
	for (i = 0; i < INV_MAIN_SIZE; i++) {
		ItemStack *s = &inv->main[i];
		if (!ItemStack_IsEmpty(s) && s->item == in->item && s->meta == in->meta &&
		    s->count < limit)
			return i;
	}
	return -1;
}

/* getFirstEmptyStack(). */
int Inventory_GetFirstEmptyStack(Inventory *inv) {
	int i;
	for (i = 0; i < INV_MAIN_SIZE; i++)
		if (ItemStack_IsEmpty(&inv->main[i])) return i;
	return -1;
}

/* storePartialItemStack(itemStackIn): store as much of `in` as fits in one
 * matching-or-empty slot; returns the left-over count. */
static int storePartialItemStack(Inventory *inv, const ItemStack *in) {
	int i = in->count;
	int j = storeItemStack(inv, in);

	if (j < 0) j = Inventory_GetFirstEmptyStack(inv);
	if (j < 0) return i;

	if (ItemStack_IsEmpty(&inv->main[j])) {
		inv->main[j].item = in->item;
		inv->main[j].meta = in->meta;
		inv->main[j].count = 0;
	}

	int k = i;
	int room = stack_limit(in->item) - inv->main[j].count;
	if (k > room) k = room;
	if (k == 0) return i;

	i -= k;
	inv->main[j].count += (u8)k;
	return i;
}

/* addItemStackToInventory (undamaged block path). */
int Inventory_AddItemStack(Inventory *inv, ItemStack *stack) {
	if (stack == NULL || stack->count == 0 || stack->item < 0) return 0;

	int start = stack->count;
	int prev;
	do {
		prev = stack->count;
		stack->count = (u8)storePartialItemStack(inv, stack);
	} while (stack->count > 0 && stack->count < prev);

	return stack->count < start;
}

int Inventory_AddItem(Inventory *inv, int item, int meta, int count) {
	ItemStack s = { (s16)item, (s16)meta, (u8)count };
	Inventory_AddItemStack(inv, &s);
	return s.count;   /* leftover */
}

/* decrStackSize(index, count) == splitStack. */
ItemStack Inventory_DecrStackSize(Inventory *inv, int index, int count) {
	ItemStack *slot = Inventory_GetStackInSlot(inv, index);
	ItemStack out = EMPTY;
	if (slot == NULL || ItemStack_IsEmpty(slot)) return out;

	if (slot->count <= count) {
		out = *slot;
		*slot = EMPTY;
	} else {
		out.item = slot->item;
		out.meta = slot->meta;
		out.count = (u8)count;
		slot->count -= (u8)count;
	}
	return out;
}

/* setInventorySlotContents(index, stack). */
void Inventory_SetSlot(Inventory *inv, int index, ItemStack stack) {
	ItemStack *slot = Inventory_GetStackInSlot(inv, index);
	if (slot) *slot = stack;
}

/* Container.slotClick, mode 0 (PICKUP). Follows the Java branch for branch:
 * empty slot -> put down, empty hand -> pick up, matching stacks -> merge,
 * anything else -> swap. */
void Inventory_SlotClick(Inventory *inv, int index, int rightClick) {
	ItemStack *slot = Inventory_GetStackInSlot(inv, index);
	if (!slot) return;
	ItemStack *carried = &inv->carried;

	if (ItemStack_IsEmpty(slot)) {
		/* itemstack11 == null: put down the whole carried stack, or one of it */
		if (ItemStack_IsEmpty(carried)) return;
		int n = rightClick ? 1 : carried->count;
		int limit = stack_limit(carried->item);
		if (n > limit) n = limit;
		slot->item  = carried->item;
		slot->meta  = carried->meta;
		slot->count = (u8)n;
		carried->count -= (u8)n;
		if (carried->count == 0) *carried = EMPTY;
		return;
	}

	if (ItemStack_IsEmpty(carried)) {
		/* itemstack15 == null: take the stack, or half of it rounded up */
		int n = rightClick ? (slot->count + 1) / 2 : slot->count;
		carried->item  = slot->item;
		carried->meta  = slot->meta;
		carried->count = (u8)n;
		slot->count -= (u8)n;
		if (slot->count == 0) *slot = EMPTY;
		return;
	}

	if (slot->item == carried->item && slot->meta == carried->meta) {
		/* same item: deposit as much as the slot has room for */
		int room = stack_limit(carried->item) - slot->count;
		int n = rightClick ? 1 : carried->count;
		if (n > room) n = room;
		if (n <= 0) return;
		slot->count += (u8)n;
		carried->count -= (u8)n;
		if (carried->count == 0) *carried = EMPTY;
		return;
	}

	/* different items: swap, provided the carried stack fits in one slot */
	if (carried->count <= stack_limit(carried->item)) {
		ItemStack tmp = *slot;
		*slot = *carried;
		*carried = tmp;
	}
}

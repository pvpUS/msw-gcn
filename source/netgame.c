#include <string.h>
#include <gccore.h>

#include "netgame.h"
#include "net.h"
#include "maps_gen.h"     /* MAP_COUNT, for the MAP_SELECT bounds check */

void NetGame_Init(NetGame *ng) {
	memset(ng, 0, sizeof(*ng));
	ng->selfEid  = -1;
	ng->mapIndex = -1;
	ng->wantMap  = -1;
	ng->health   = 20.0f;
	Entity_WorldInit(&ng->ents);
	Hud_NetInit(&ng->hud);
	Inventory_Init(&ng->inv);
}

void NetGame_Reset(NetGame *ng) {
	Entity_WorldClear(&ng->ents);
	Hud_NetInit(&ng->hud);
	ng->tpPending = 0;
}

void NetGame_MapLoaded(NetGame *ng, int index) {
	ng->mapIndex = index;
}

void NetGame_Tick(NetGame *ng) {
	Entity_TickAll(&ng->ents);
	Hud_NetTick(&ng->hud);
}

/* ---- message handlers ---------------------------------------------------
 * Every one of these is bounds-checked against the frame length before it
 * reads. The proxy and the console are written months and one language apart,
 * and a payload that is a byte short should be a dropped message rather than a
 * read past the receive buffer. */

static void on_map_select(NetGame *ng, const u8 *d, u16 len) {
	if (len < 17) return;
	int idx = gc_get_u8(d);
	if (idx < 0 || idx >= MAP_COUNT) return;   /* proxy and DOL disagree */
	ng->originX = gc_get_s32(d + 1);
	ng->originY = gc_get_s32(d + 5);
	ng->originZ = gc_get_s32(d + 9);
	ng->selfEid = gc_get_s32(d + 13);
	Entity_SetSelf(&ng->ents, ng->selfEid);
	ng->wantMap = idx;
}

static void on_block_set(NetGame *ng, World *w, const u8 *d, u16 len) {
	u16 n = len / 8, i;
	if (!w) return;   /* no world yet; the proxy re-sends the diff on attach */
	for (i = 0; i < n; i++) {
		const u8 *p = d + i * 8;
		int x  = gc_get_s16(p);
		int y  = gc_get_s16(p + 2);
		int z  = gc_get_s16(p + 4);
		u16 id = gc_get_u16(p + 6);
		/* Deferred, always. A join-time diff is hundreds of blocks in one
		 * frame and the synchronous World_SetBlock would re-mesh the same few
		 * chunks once per block; World_FlushRemesh drains them nearest-first
		 * over the following frames instead (T24). */
		World_SetBlockDeferred(w, x, y, z,
		                       (id == GCLINK_AIR) ? -1 : (int)id);
	}
	ng->blockSets += n;
}

static void on_teleport(NetGame *ng, const u8 *d, u16 len) {
	if (len < 33) return;
	ng->tpX     = gc_get_f64(d);
	ng->tpY     = gc_get_f64(d + 8);
	ng->tpZ     = gc_get_f64(d + 16);
	ng->tpYaw   = gc_get_f32(d + 24);
	ng->tpPitch = gc_get_f32(d + 28);
	/* Echoed back in every MOVE once T22 sends any, so the server can throw
	 * away the ones that were in flight when it moved us. */
	ng->teleportEpoch = gc_get_u8(d + 32);
	ng->tpPending = 1;
}

static void on_entity_add(NetGame *ng, const u8 *d, u16 len) {
	if (len < 24) return;
	s32 eid = gc_get_s32(d);
	u8  type = gc_get_u8(d + 4);
	Entity *e = Entity_Add(&ng->ents, eid, type);
	if (!e) return;

	e->flags = gc_get_u8(d + 5);
	double x = (double)gc_get_s32(d + 6)  / GCLINK_POS_SCALE;
	double y = (double)gc_get_s32(d + 10) / GCLINK_POS_SCALE;
	double z = (double)gc_get_s32(d + 14) / GCLINK_POS_SCALE;
	Entity_SetPos(e, x, y, z,
	              gclink_yaw(gc_get_u8(d + 18)),
	              gclink_pitch(gc_get_u8(d + 19)));

	u16 held = gc_get_u16(d + 20);
	e->held   = (held == GCLINK_AIR) ? -1 : (int)held;
	e->colour = gc_get_u8(d + 22);

	u32 nameLen = gc_get_u8(d + 23);
	if (nameLen > (u32)(len - 24))       nameLen = (u32)(len - 24);
	if (nameLen > ENTITY_NAME_MAX - 1)   nameLen = ENTITY_NAME_MAX - 1;
	if (nameLen) memcpy(e->name, d + 24, nameLen);
	e->name[nameLen] = '\0';

	ng->entityAdds++;
}

static void on_entity_move(NetGame *ng, const u8 *d, u16 len) {
	u16 n = len / 18, i;
	for (i = 0; i < n; i++) {
		const u8 *p = d + i * 18;
		/* An eid the console does not know is a stale update -- ignore it
		 * rather than synthesising an entity with no type or name. */
		Entity *e = Entity_Find(&ng->ents, gc_get_s32(p));
		if (!e) continue;
		Entity_MoveTo(e,
		              (double)gc_get_s32(p + 4)  / GCLINK_POS_SCALE,
		              (double)gc_get_s32(p + 8)  / GCLINK_POS_SCALE,
		              (double)gc_get_s32(p + 12) / GCLINK_POS_SCALE,
		              gclink_yaw(gc_get_u8(p + 16)),
		              gclink_pitch(gc_get_u8(p + 17)));
	}
}

static void on_entity_remove(NetGame *ng, const u8 *d, u16 len) {
	u16 n = len / 4, i;
	for (i = 0; i < n; i++) Entity_Remove(&ng->ents, gc_get_s32(d + i * 4));
}

static void on_entity_equip(NetGame *ng, const u8 *d, u16 len) {
	if (len < 7) return;
	Entity *e = Entity_Find(&ng->ents, gc_get_s32(d));
	if (!e) return;
	/* Slot 0 is the hand; 1-4 are armor, which this target does not render --
	 * everyone on MegaSkywars wears full enchanted diamond, so drawing it
	 * would tell the player nothing they could act on. See "Cut by decision". */
	if (gc_get_u8(d + 4) != 0) return;
	u16 item = gc_get_u16(d + 5);
	e->held = (item == GCLINK_AIR) ? -1 : (int)item;
}

static void on_entity_anim(NetGame *ng, const u8 *d, u16 len) {
	if (len < 5) return;
	Entity *e = Entity_Find(&ng->ents, gc_get_s32(d));
	if (e) Entity_Anim(e, gc_get_u8(d + 4));
}

static void on_inv_set(NetGame *ng, const u8 *d, u16 len) {
	u16 n = len / 6, i;
	for (i = 0; i < n; i++) {
		const u8 *p = d + i * 6;
		int slot = gc_get_u8(p);
		u16 item = gc_get_u16(p + 1);
		u8  count = gc_get_u8(p + 3);
		u16 meta = gc_get_u16(p + 4);
		if (slot < 0 || slot >= INV_TOTAL_SIZE) continue;
		ItemStack s;
		if (item == GCLINK_AIR || count == 0) {
			s.item = -1; s.meta = 0; s.count = 0;
		} else {
			s.item = (int)item; s.meta = (int)meta; s.count = (int)count;
		}
		Inventory_SetSlot(&ng->inv, slot, s);
	}
}

static void on_chat(NetGame *ng, const u8 *d, u16 len, int actionBar) {
	if (len < 1) return;
	u8 colour = gc_get_u8(d);
	const char *text = (const char *)(d + 1);
	int n = (int)len - 1;
	if (actionBar) Hud_NetActionBar(&ng->hud, colour, text, n);
	else           Hud_NetChat(&ng->hud, colour, text, n);
}

/* ---- the drain ---------------------------------------------------------- */

int NetGame_Poll(NetGame *ng, World *w) {
	NetMsg m;

	if (ng->wantMap >= 0 && ng->wantMap != ng->mapIndex) return 1;

	while (Net_Poll(&m)) {
		ng->msgs++;
		switch (m.type) {
		case GC_S_MAP_SELECT:
			on_map_select(ng, m.data, m.len);
			if (ng->wantMap != ng->mapIndex) {
				/* Stop here. Everything after this in the buffer is about the
				 * new map, and applying it against the old one -- or against
				 * none -- would be worse than waiting a frame. Net_Poll leaves
				 * the rest of the buffer intact for the next call. */
				NetGame_Reset(ng);
				return 1;
			}
			break;

		case GC_S_BLOCK_SET:     on_block_set(ng, w, m.data, m.len); break;
		case GC_S_TELEPORT:      on_teleport(ng, m.data, m.len); break;
		case GC_S_ENTITY_ADD:    on_entity_add(ng, m.data, m.len); break;
		case GC_S_ENTITY_MOVE:   on_entity_move(ng, m.data, m.len); break;
		case GC_S_ENTITY_REMOVE: on_entity_remove(ng, m.data, m.len); break;
		case GC_S_ENTITY_EQUIP:  on_entity_equip(ng, m.data, m.len); break;
		case GC_S_ENTITY_ANIM:   on_entity_anim(ng, m.data, m.len); break;
		case GC_S_INV_SET:       on_inv_set(ng, m.data, m.len); break;

		case GC_S_GAME_STATE:
			if (m.len >= 1) ng->gameState = ng->hud.gameState = gc_get_u8(m.data);
			break;
		case GC_S_GAME_MODE:
			if (m.len >= 1) ng->gameMode = ng->hud.gameMode = gc_get_u8(m.data);
			break;
		case GC_S_HEALTH:
			if (m.len >= 4) ng->health = gc_get_f32(m.data);
			break;
		case GC_S_HELD_SLOT:
			if (m.len >= 1) {
				ng->heldSlot = gc_get_u8(m.data) & 7;
				Inventory_SetCurrentItem(&ng->inv, ng->heldSlot);
			}
			break;
		case GC_S_XP:
			if (m.len >= 10) {
				ng->hud.xpBar   = gc_get_f32(m.data);
				ng->hud.xpLevel = gc_get_s16(m.data + 4);
			}
			break;

		case GC_S_CHAT:       on_chat(ng, m.data, m.len, 0); break;
		case GC_S_ACTION_BAR: on_chat(ng, m.data, m.len, 1); break;

		/* Sent by the proxy already, meaningful only once the console moves
		 * and fights: knockback (T19) and the item-use slowdown (T22). Named
		 * rather than defaulted so the "unhandled" log below stays about
		 * messages that really are a surprise. */
		case GC_S_SELF_VELOCITY:
		case GC_S_USE_STATE:
			break;

		default:
			break;
		}
	}
	return (ng->wantMap >= 0 && ng->wantMap != ng->mapIndex);
}

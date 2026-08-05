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
}

void NetGame_Reset(NetGame *ng) {
	Entity_WorldClear(&ng->ents);
	Hud_NetInit(&ng->hud);
	ng->tpPending = 0;
	ng->usingItem = 0;
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

/* S08 PlayerPosLook, already resolved and converted by the proxy.
 *
 * The epoch is the whole point of the round trip. The console has MOVEs in
 * flight when this arrives, describing where it was a moment ago; every one of
 * them is now wrong, and a server that acted on them would rubberband the
 * player straight back. So the epoch bumps, the console echoes the new one in
 * every MOVE from here on, and the proxy throws away anything still carrying
 * the old one. */
static void on_teleport(NetGame *ng, Player *p, const u8 *d, u16 len) {
	if (len < 33) return;
	ng->tpX     = gc_get_f64(d);
	ng->tpY     = gc_get_f64(d + 8);
	ng->tpZ     = gc_get_f64(d + 16);
	ng->tpYaw   = gc_get_f32(d + 24);
	ng->tpPitch = gc_get_f32(d + 28);
	ng->teleportEpoch = gc_get_u8(d + 32);
	ng->tpPending = 1;
	if (p) Player_Teleport(p, ng->tpX, ng->tpY, ng->tpZ, ng->tpYaw, ng->tpPitch);
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

/* S2F/S30 -> the player's own inventory, which the server owns outright once
 * there is a game on: the kit arrives this way, every block placed comes back
 * decremented this way, and death empties it this way. */
static void on_inv_set(NetGame *ng, Player *p, const u8 *d, u16 len) {
	u16 n = len / 6, i;
	if (!p) return;
	for (i = 0; i < n; i++) {
		const u8 *q = d + i * 6;
		int slot = gc_get_u8(q);
		u16 item = gc_get_u16(q + 1);
		u8  count = gc_get_u8(q + 3);
		u16 meta = gc_get_u16(q + 4);
		if (slot < 0 || slot > GCLINK_INV_CURSOR) continue;
		ItemStack s;
		if (item == GCLINK_AIR || count == 0) {
			s.item = -1; s.meta = 0; s.count = 0;
		} else {
			s.item = (s16)item; s.meta = (s16)meta; s.count = (u8)count;
		}
		/* The cursor is not a slot in the array -- it is the stack being
		 * carried between them -- but it arrives the same way and for the same
		 * reason, so it rides the same message. */
		if (slot == GCLINK_INV_CURSOR) p->inventory.carried = s;
		else                           Inventory_SetSlot(&p->inventory, slot, s);
	}
	(void)ng;
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

int NetGame_Poll(NetGame *ng, World *w, Player *p) {
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
		case GC_S_TELEPORT:      on_teleport(ng, p, m.data, m.len); break;
		case GC_S_ENTITY_ADD:    on_entity_add(ng, m.data, m.len); break;
		case GC_S_ENTITY_MOVE:   on_entity_move(ng, m.data, m.len); break;
		case GC_S_ENTITY_REMOVE: on_entity_remove(ng, m.data, m.len); break;
		case GC_S_ENTITY_EQUIP:  on_entity_equip(ng, m.data, m.len); break;
		case GC_S_ENTITY_ANIM:   on_entity_anim(ng, m.data, m.len); break;
		case GC_S_INV_SET:       on_inv_set(ng, p, m.data, m.len); break;

		case GC_S_GAME_STATE:
			if (m.len >= 1) ng->gameState = ng->hud.gameState = gc_get_u8(m.data);
			break;
		case GC_S_GAME_MODE:
			/* Death on this server is a game-mode change and nothing else --
			 * the plugin cancels lethal damage, so there is no S07 Respawn and
			 * no death screen to wait for. Spectator hides the hotbar, the
			 * hearts and the crosshair (T26). */
			if (m.len >= 1) {
				ng->gameMode = ng->hud.gameMode = gc_get_u8(m.data);
				if (p) p->gameMode = ng->gameMode;
			}
			break;
		case GC_S_HEALTH:
			/* S06 UpdateHealth. The local fall-damage prediction is gated off
			 * (Player.serverDriven) so the two cannot fight. */
			if (m.len >= 4) {
				ng->health = gc_get_f32(m.data);
				if (p) p->health = ng->health;
			}
			break;
		case GC_S_SELF_VELOCITY:
			/* Knockback. Applying this is mandatory -- ignore it and the
			 * server moves you while you do not, which is permanent rubberband
			 * for as long as someone is hitting you. */
			if (m.len >= 24 && p)
				Player_SetVelocity(p, gc_get_f64(m.data),
				                      gc_get_f64(m.data + 8),
				                      gc_get_f64(m.data + 16));
			break;
		case GC_S_USE_STATE:
			if (m.len >= 1) {
				ng->usingItem = gc_get_u8(m.data) != 0;
				if (p) p->itemInUse = ng->usingItem;
			}
			break;
		case GC_S_HELD_SLOT:
			if (m.len >= 1) {
				ng->heldSlot = gc_get_u8(m.data) & 7;
				if (p) Inventory_SetCurrentItem(&p->inventory, ng->heldSlot);
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

		default:
			break;
		}
	}
	return (ng->wantMap >= 0 && ng->wantMap != ng->mapIndex);
}

/* ---- console -> proxy (T22) --------------------------------------------- */

void NetGame_SendMove(NetGame *ng, const Player *p) {
	u8 buf[34];
	u8 flags = 0;

	if (!ng->sendMovement) return;
	/* Nothing before the first teleport. The server's hasMoved stays false
	 * until it has had its own position echoed back (which the proxy does the
	 * instant S08 arrives), and everything sent before that lands in a bit
	 * bucket -- including, later, the first block placement, which is the
	 * symptom this is actually preventing. */
	if (!ng->tpPending) return;

	if (p->onGround)  flags |= GCLINK_MOVE_ONGROUND;
	if (p->sprinting) flags |= GCLINK_MOVE_SPRINTING;
	if (p->sneaking)  flags |= GCLINK_MOVE_SNEAKING;

	gc_put_f64(buf,      p->x);
	gc_put_f64(buf + 8,  p->y);      /* the AABB's minY, which is the feet */
	gc_put_f64(buf + 16, p->z);
	gc_put_f32(buf + 24, p->yaw);
	gc_put_f32(buf + 28, p->pitch);
	gc_put_u8 (buf + 32, flags);
	gc_put_u8 (buf + 33, ng->teleportEpoch);

	Net_Send(GC_C_MOVE, buf, sizeof buf);
}

void NetGame_SendInteract(NetGame *ng, const Interact *it) {
	int i;
	(void)ng;
	for (i = 0; i < it->evCount; i++) {
		const InteractEvent *e = &it->ev[i];
		u8 buf[10];
		switch (e->kind) {
		case INTERACT_EV_DIG:
			gc_put_u8 (buf,     e->status);
			gc_put_s16(buf + 1, e->bx);
			gc_put_s16(buf + 3, e->by);
			gc_put_s16(buf + 5, e->bz);
			gc_put_u8 (buf + 7, e->face);
			Net_Send(GC_C_DIG, buf, 8);
			break;
		case INTERACT_EV_PLACE:
			gc_put_s16(buf,     e->bx);
			gc_put_s16(buf + 2, e->by);
			gc_put_s16(buf + 4, e->bz);
			gc_put_u8 (buf + 6, e->face);
			gc_put_u8 (buf + 7, e->curX);
			gc_put_u8 (buf + 8, e->curY);
			gc_put_u8 (buf + 9, e->curZ);
			Net_Send(GC_C_PLACE, buf, 10);
			break;
		case INTERACT_EV_USE:
			gc_put_u8(buf, e->status == USE_RELEASE ? GCLINK_ITEM_RELEASE
			                                        : GCLINK_ITEM_START);
			Net_Send(GC_C_USE_ITEM, buf, 1);
			break;
		default:
			break;
		}
	}
}

void NetGame_SendCombat(NetGame *ng, Combat *c) {
	(void)ng;
	/* Order matters and it is the proxy's to enforce -- C09 held slot, then
	 * C0A ArmSwing, then C02 UseEntity -- but the swing still has to leave
	 * here first, because a swing with no attack behind it (a miss) is the
	 * only thing other players see of it. */
	if (Combat_TakeSwing(c)) Net_Send(GC_C_SWING, NULL, 0);

	s32 eid = Combat_TakeAttack(c);
	if (eid >= 0) {
		u8 buf[5];
		gc_put_s32(buf, eid);
		gc_put_u8(buf + 4, GCLINK_USE_ATTACK);
		Net_Send(GC_C_USE_ENTITY, buf, 5);
	}
}

/* One inventory click. See GC_C_WINDOW_CLICK: the proxy fills in the stack the
 * server believes is in the slot and the action number, because both are its to
 * know -- the console's copy of the window is a mirror, not the record. */
void NetGame_SendWindowClick(NetGame *ng, int slot, int button) {
	u8 buf[2];
	(void)ng;
	if (slot < 0 || slot >= INV_TOTAL_SIZE) return;
	gc_put_u8(buf,     (u8)slot);
	gc_put_u8(buf + 1, (u8)(button ? GCLINK_CLICK_RIGHT : GCLINK_CLICK_LEFT));
	Net_Send(GC_C_WINDOW_CLICK, buf, sizeof buf);
}

void NetGame_SendHeldSlot(NetGame *ng, int slot) {
	u8 b = (u8)(slot & 7);
	(void)ng;
	Net_Send(GC_C_HELD_SLOT, &b, 1);
}

void NetGame_SendChat(NetGame *ng, const char *text) {
	int n = 0;
	(void)ng;
	if (!text) return;
	while (text[n] && n < 100) n++;
	if (!n) return;
	Net_Send(GC_C_CHAT, text, (u16)n);
}

void NetGame_SendAction(NetGame *ng, int action) {
	u8 b = (u8)action;
	(void)ng;
	Net_Send(GC_C_ACTION, &b, 1);
}

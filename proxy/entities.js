'use strict';
/*
 * entities.js -- the server's entity stream, reduced to what a GameCube can
 * draw and afford.
 *
 * Two filters do most of the work here, and both exist because of how
 * MegaSkywars actually plays rather than because of the protocol:
 *
 *  - **Type.** Every kill spawns real mobs, armor stands and flying skulls
 *    tagged as cosmetics (Cosmetics.java), and every projectile drags a
 *    per-tick particle trail behind it. Only the eight types in ENT reach the
 *    console; everything else is dropped before it ever takes a slot.
 *  - **Count.** 128 slots, nearest-first. Sixteen players, their arrows and
 *    snowballs, and one 192-stone death drop overflow 64 comfortably, which is
 *    why the cap is not 64 -- but it is still a cap, and the thing to evict is
 *    whatever is furthest away and least likely to be looked at.
 *
 * Positions stay in the wire's own fixed-point (x32) all the way to the
 * console, converted only from absolute to map-local. Relative moves are
 * accumulated here rather than forwarded: the console gets absolute positions,
 * so one dropped frame is one stale tick instead of a permanent offset.
 */

const { S, Writer, ENT, EFLAG, ANIM, AIR, POS_SCALE, ENTITY_MOVE_MAX,
        angleToByte } = require('./gclink');

// ---- 1.8 type numbers -------------------------------------------------------
/** S0E spawn_entity object types worth drawing. */
const OBJECT_TYPES = new Map([
    [2, ENT.ITEM],        // dropped item
    [60, ENT.ARROW],
    [61, ENT.SNOWBALL],
    [65, ENT.PEARL],      // thrown ender pearl
    [73, ENT.POTION],     // splash potion
    [90, ENT.BOBBER],     // fishing float -- the Knockback III rod
]);

/** S0F spawn_entity_living: the plugin spawns exactly one mob per game, an
 *  ender dragon over the map centre (MapStatus:1076). Everything else that
 *  appears as a living entity is a cosmetic. */
const MOB_TYPES = new Map([
    [63, ENT.DRAGON],
]);

/** 1.8 metadata index 0, the shared entity flag byte. */
const META_ON_FIRE = 0x01;
const META_SNEAKING = 0x02;
const META_SPRINTING = 0x08;
const META_USING = 0x10;
const META_INVISIBLE = 0x20;

/** EntityItem's DataWatcher index for the ItemStack it is carrying. */
const META_ITEM_STACK = 10;

const COLOUR_CODES = '0123456789abcdef';

class Entity {
    constructor(eid, type) {
        this.eid = eid;
        this.type = type;
        this.x = 0; this.y = 0; this.z = 0;   // absolute, fixed-point x32
        this.yaw = 0; this.pitch = 0;         // byte turns
        this.flags = 0;
        this.held = AIR;
        this.name = '';
        this.colour = 0xff;
        this.added = false;                   // has the console been told?
        this.moved = false;                   // changed since the last flush?
    }
}

class EntityTranslator {
    constructor({ link, blockmap, config, log = console }) {
        this.link = link;
        this.bm = blockmap;
        this.log = log;
        this.cap = config.cap ?? 128;
        this.minDelta = Math.round((config.minMoveDelta ?? 0.03) * POS_SCALE) || 1;

        this.map = null;         // origin for the absolute -> local conversion
        this.selfEid = -1;
        this.entities = new Map();   // eid -> Entity
        this.names = new Map();      // uuid -> name, from S38 player_info
        this.uuidOf = new Map();     // eid -> uuid
        this.teamColour = new Map(); // player name -> MC colour code index
        this.playerTeam = new Map(); // player name -> team name
        this.evicted = new Set();    // eids the cap pushed out; not re-added blind

        this.pendingAnims = [];
        this.pendingEquip = [];
        this.stats = { added: 0, dropped: 0, evicted: 0, moves: 0 };
    }

    setMap(m) { this.map = m; this.clear(); }
    setSelfEid(eid) { this.selfEid = eid; }

    clear() {
        if (this.entities.size && this.link.attached) {
            this.sendRemovals([...this.entities.keys()]);
        }
        this.entities.clear();
        this.uuidOf.clear();
        this.evicted.clear();
        this.pendingAnims.length = 0;
        this.pendingEquip.length = 0;
    }

    /** A console that just attached knows about nothing; make every live
     *  entity look new again so the next flush introduces them all. */
    onConsoleAttached() {
        for (const e of this.entities.values()) { e.added = false; e.moved = true; }
    }

    // ---- spawn -------------------------------------------------------------

    /** S0C named_entity_spawn -- players, and only players. */
    onPlayerSpawn(pkt) {
        const e = this.track(pkt.entityId, ENT.PLAYER);
        if (!e) return;
        e.x = pkt.x; e.y = pkt.y; e.z = pkt.z;
        e.yaw = pkt.yaw & 0xff; e.pitch = pkt.pitch & 0xff;
        e.held = this.itemId(pkt.currentItem);
        this.uuidOf.set(e.eid, pkt.playerUUID);
        e.name = this.names.get(pkt.playerUUID) || '';
        e.colour = this.colourFor(e.name);
        this.applyMetadata(e, pkt.metadata);
        e.moved = true;
    }

    /** S0E spawn_entity -- objects and projectiles. */
    onObjectSpawn(pkt) {
        const type = OBJECT_TYPES.get(pkt.type);
        if (type === undefined) { this.stats.dropped++; return; }
        const e = this.track(pkt.entityId, type);
        if (!e) return;
        e.x = pkt.x; e.y = pkt.y; e.z = pkt.z;
        e.yaw = pkt.yaw & 0xff; e.pitch = pkt.pitch & 0xff;
        e.moved = true;
    }

    /** S0F spawn_entity_living -- the ender dragon, and nothing else. */
    onMobSpawn(pkt) {
        const type = MOB_TYPES.get(pkt.type);
        if (type === undefined) { this.stats.dropped++; return; }
        const e = this.track(pkt.entityId, type);
        if (!e) return;
        e.x = pkt.x; e.y = pkt.y; e.z = pkt.z;
        e.yaw = pkt.yaw & 0xff; e.pitch = pkt.pitch & 0xff;
        this.applyMetadata(e, pkt.metadata);
        e.moved = true;
    }

    track(eid, type) {
        if (eid === this.selfEid) return null;   // the console is not an entity
        let e = this.entities.get(eid);
        if (e) { e.type = type; return e; }
        e = new Entity(eid, type);
        this.entities.set(eid, e);
        this.evicted.delete(eid);
        this.stats.added++;
        return e;
    }

    // ---- movement ----------------------------------------------------------

    /** S15 rel_entity_move and S17 entity_move_look: deltas are int8 x32, so a
     *  little under four blocks. Accumulated into the absolute position. */
    onRelMove(pkt, hasLook) {
        const e = this.entities.get(pkt.entityId);
        if (!e) return;
        e.x += pkt.dX; e.y += pkt.dY; e.z += pkt.dZ;
        if (hasLook) { e.yaw = pkt.yaw & 0xff; e.pitch = pkt.pitch & 0xff; }
        e.moved = true;
    }

    /** S16 entity_look: rotation only, no position. */
    onLook(pkt) {
        const e = this.entities.get(pkt.entityId);
        if (!e) return;
        e.yaw = pkt.yaw & 0xff; e.pitch = pkt.pitch & 0xff;
        e.moved = true;
    }

    /** S18 entity_teleport: absolute, and authoritative over any accumulated
     *  drift -- which is exactly why the console is sent absolutes too. */
    onTeleport(pkt) {
        const e = this.entities.get(pkt.entityId);
        if (!e) return;
        e.x = pkt.x; e.y = pkt.y; e.z = pkt.z;
        e.yaw = pkt.yaw & 0xff; e.pitch = pkt.pitch & 0xff;
        e.moved = true;
    }

    /** S13 entity_destroy. */
    onDestroy(pkt) {
        const gone = [];
        for (const eid of pkt.entityIds) {
            if (this.entities.delete(eid)) gone.push(eid);
            this.evicted.delete(eid);
            this.uuidOf.delete(eid);
        }
        if (gone.length) this.sendRemovals(gone);
    }

    // ---- appearance --------------------------------------------------------

    /** S04 entity_equipment. Slot 0 is the held item, 1-4 the armor. */
    onEquipment(pkt) {
        const e = this.entities.get(pkt.entityId);
        if (!e) return;
        const item = this.itemId(pkt.item);
        if (pkt.slot === 0) {
            if (e.held === item) return;
            e.held = item;
        }
        this.pendingEquip.push([e.eid, pkt.slot & 0xff, item]);
    }

    /** S0B animation: 0 swing arm, 1 take damage, 3 eat/critical-effect. */
    onAnimation(pkt) {
        if (!this.entities.has(pkt.entityId)) return;
        const map = { 0: ANIM.SWING, 1: ANIM.HURT, 3: ANIM.EAT };
        const a = map[pkt.animation];
        if (a !== undefined) this.pendingAnims.push([pkt.entityId, a]);
    }

    /** S19 entity_status: 2 is the hurt flash, 3 is death. */
    onStatus(pkt) {
        if (!this.entities.has(pkt.entityId)) return;
        if (pkt.entityStatus === 2) this.pendingAnims.push([pkt.entityId, ANIM.HURT]);
        else if (pkt.entityStatus === 3) this.pendingAnims.push([pkt.entityId, ANIM.DEATH]);
    }

    /** S1C entity_metadata: only the shared flag byte at index 0 is of use. */
    onMetadata(pkt) {
        const e = this.entities.get(pkt.entityId);
        if (!e) return;
        this.applyMetadata(e, pkt.metadata);
    }

    applyMetadata(e, metadata) {
        if (!Array.isArray(metadata)) return;
        for (const entry of metadata) {
            if (entry.key === 0 && typeof entry.value === 'number') {
                let f = 0;
                if (entry.value & META_SNEAKING) f |= EFLAG.SNEAKING;
                if (entry.value & META_SPRINTING) f |= EFLAG.SPRINTING;
                if (entry.value & META_INVISIBLE) f |= EFLAG.INVISIBLE;
                if (entry.value & META_USING) f |= EFLAG.USING;
                e.flags = f;
                continue;
            }
            // A dropped item's *contents* travel as DataWatcher index 10, not
            // in the S0E that spawns it -- S0E only says "object type 2". So
            // without this the console knows a drop exists and not what it is,
            // and draws nothing. Reuses the held-item slot because that is
            // exactly what it means for an item entity.
            if (entry.key === META_ITEM_STACK && e.type === ENT.ITEM) {
                const item = this.itemId(entry.value);
                if (item === e.held) continue;
                e.held = item;
                this.pendingEquip.push([e.eid, 0, item]);
            }
        }
    }

    // ---- names and teams ----------------------------------------------------

    /** S38 player_info. Names arrive here, usually before the entity does.
     *  `action` decodes to a name, not the wire's number. */
    onPlayerInfo(pkt) {
        const add = pkt.action === 'add_player' || pkt.action === 0;
        const remove = pkt.action === 'remove_player' || pkt.action === 4;
        for (const d of pkt.data || []) {
            if (add && d.name) this.names.set(d.UUID, stripCodes(d.name));
            else if (remove) this.names.delete(d.UUID);
        }
        // Fill in anyone who spawned before their name did.
        for (const e of this.entities.values()) {
            if (e.type !== ENT.PLAYER || e.name) continue;
            const n = this.names.get(this.uuidOf.get(e.eid));
            if (!n) continue;
            e.name = n;
            e.colour = this.colourFor(n);
        }
    }

    /**
     * S3E teams. The plugin puts each skywars team in a scoreboard team whose
     * prefix is its colour, which is the only signal for who is on whose side
     * -- there is no sidebar scoreboard to read it off.
     */
    onTeam(pkt) {
        const mode = pkt.mode;
        if (mode === 1) {                       // team removed
            for (const [p, t] of this.playerTeam) {
                if (t === pkt.team) { this.playerTeam.delete(p); this.teamColour.delete(p); }
            }
        }
        if (mode === 0 || mode === 2) {         // created / updated
            const c = firstColourCode(pkt.prefix || '');
            this._teamColours = this._teamColours || new Map();
            this._teamColours.set(pkt.team, c);
            for (const [p, t] of this.playerTeam) {
                if (t === pkt.team) this.teamColour.set(p, c);
            }
        }
        if (mode === 0 || mode === 3) {         // created / players added
            const c = (this._teamColours && this._teamColours.get(pkt.team)) ?? 0xff;
            for (const p of pkt.players || []) {
                this.playerTeam.set(p, pkt.team);
                this.teamColour.set(p, c);
            }
        }
        if (mode === 4) {                       // players removed
            for (const p of pkt.players || []) {
                this.playerTeam.delete(p);
                this.teamColour.delete(p);
            }
        }
        for (const e of this.entities.values()) {
            if (e.type !== ENT.PLAYER || !e.name) continue;
            const c = this.teamColour.get(e.name);
            if (c !== undefined && c !== e.colour) { e.colour = c; e.added = false; }
        }
    }

    colourFor(name) {
        const c = this.teamColour.get(name);
        return c === undefined ? 0xff : c;
    }

    /** Engine item id from either a full slot (S04 equipment) or a bare item
     *  id (S0C's currentItem, which carries no damage value). */
    itemId(item) {
        let id, damage = 0;
        if (typeof item === 'number') { id = item; }
        else if (item && item.blockId !== undefined) { id = item.blockId; damage = item.itemDamage || 0; }
        else return AIR;
        if (id === null || id < 0) return AIR;
        const engine = this.bm.toItem(id, damage);
        return engine === null || engine === undefined ? AIR : engine;
    }

    // ---- flush --------------------------------------------------------------

    /**
     * Called at 20 Hz. Introduces anything new, sends one batched ENTITY_MOVE
     * for everything that actually moved, and applies the cap by distance from
     * the player -- who is `selfX/Y/Z` in absolute coordinates.
     */
    flush(selfX, selfY, selfZ) {
        if (!this.link.attached || !this.map) return;

        this.enforceCap(selfX, selfY, selfZ);

        for (const e of this.entities.values()) {
            if (!e.added) { this.sendAdd(e); e.added = true; e.moved = false; }
        }

        // Batch the moves. A frame's worth of ENTITY_MOVE for 128 entities is
        // 2.3 KB, which is nothing on a LAN and one console-side read instead
        // of 128 -- the console drains this inside a 16 ms frame.
        const moving = [];
        for (const e of this.entities.values()) {
            if (!e.moved) continue;
            e.moved = false;
            const dx = e.x - (e.sentX ?? Infinity);
            if (e.sentX !== undefined &&
                Math.abs(dx) < this.minDelta &&
                Math.abs(e.y - e.sentY) < this.minDelta &&
                Math.abs(e.z - e.sentZ) < this.minDelta &&
                e.yaw === e.sentYaw && e.pitch === e.sentPitch) continue;
            e.sentX = e.x; e.sentY = e.y; e.sentZ = e.z;
            e.sentYaw = e.yaw; e.sentPitch = e.pitch;
            moving.push(e);
        }
        for (let i = 0; i < moving.length; i += ENTITY_MOVE_MAX) {
            const chunk = moving.slice(i, i + ENTITY_MOVE_MAX);
            const w = new Writer(chunk.length * 18);
            for (const e of chunk) {
                w.i32(e.eid)
                 .i32(e.x - this.map.originX * POS_SCALE)
                 .i32(e.y - this.map.originY * POS_SCALE)
                 .i32(e.z - this.map.originZ * POS_SCALE)
                 .u8(e.yaw).u8(e.pitch);
            }
            this.link.send(S.ENTITY_MOVE, w.done());
            this.stats.moves += chunk.length;
        }

        for (const [eid, slot, item] of this.pendingEquip) {
            if (!this.entities.has(eid)) continue;
            this.link.send(S.ENTITY_EQUIP, new Writer(7).i32(eid).u8(slot).u16(item).done());
        }
        this.pendingEquip.length = 0;

        for (const [eid, anim] of this.pendingAnims) {
            if (!this.entities.has(eid)) continue;
            this.link.send(S.ENTITY_ANIM, new Writer(5).i32(eid).u8(anim).done());
        }
        this.pendingAnims.length = 0;
    }

    sendAdd(e) {
        const w = new Writer(32);
        w.i32(e.eid).u8(e.type).u8(e.flags)
         .i32(e.x - this.map.originX * POS_SCALE)
         .i32(e.y - this.map.originY * POS_SCALE)
         .i32(e.z - this.map.originZ * POS_SCALE)
         .u8(e.yaw).u8(e.pitch)
         .u16(e.held)
         .u8(e.colour)
         .str8(e.name, 24);
        this.link.send(S.ENTITY_ADD, w.done());
        e.sentX = e.x; e.sentY = e.y; e.sentZ = e.z;
        e.sentYaw = e.yaw; e.sentPitch = e.pitch;
    }

    sendRemovals(eids) {
        for (let i = 0; i < eids.length; i += 2048) {
            const chunk = eids.slice(i, i + 2048);
            const w = new Writer(chunk.length * 4);
            for (const eid of chunk) w.i32(eid);
            this.link.send(S.ENTITY_REMOVE, w.done());
        }
    }

    /**
     * Nearest-N, but players and the dragon rank ahead of everything else
     * outright rather than by a weighted distance. Losing sight of an opponent
     * because a burst of arrows filled the table is a gameplay bug; losing an
     * arrow is not, and no distance weighting survives the case it has to --
     * a fight across the map while snowballs land at your feet. A game caps at
     * sixteen players and one dragon, so this tier can never exhaust 128 by
     * itself and the cap stays a cap.
     */
    enforceCap(selfX, selfY, selfZ) {
        if (this.entities.size <= this.cap) return;
        const sx = selfX * POS_SCALE, sy = selfY * POS_SCALE, sz = selfZ * POS_SCALE;
        const ranked = [...this.entities.values()].map((e) => {
            const dx = e.x - sx, dy = e.y - sy, dz = e.z - sz;
            const tier = (e.type === ENT.PLAYER || e.type === ENT.DRAGON) ? 0 : 1;
            return { e, tier, d: dx * dx + dy * dy + dz * dz };
        }).sort((a, b) => (a.tier - b.tier) || (a.d - b.d));

        const drop = ranked.slice(this.cap);
        const gone = [];
        for (const { e } of drop) {
            this.entities.delete(e.eid);
            if (e.added) gone.push(e.eid);
            this.evicted.add(e.eid);
            this.stats.evicted++;
        }
        if (gone.length) this.sendRemovals(gone);
    }

    get count() { return this.entities.size; }

    report() {
        const s = this.stats;
        return `entities ${this.entities.size} live, ${s.added} added, ` +
               `${s.dropped} filtered out, ${s.evicted} evicted, ${s.moves} moves sent`;
    }
}

/** The first section-sign colour code in a string, as an index into the 16, or
 *  0xff when there is none. Formatting codes (k-o, r) are not colours. */
function firstColourCode(s) {
    for (let i = 0; i + 1 < s.length; i++) {
        if (s[i] !== '§') continue;
        const idx = COLOUR_CODES.indexOf(s[i + 1].toLowerCase());
        if (idx >= 0) return idx;
    }
    return 0xff;
}

function stripCodes(s) { return String(s).replace(/§./g, ''); }

module.exports = { EntityTranslator, OBJECT_TYPES, MOB_TYPES, firstColourCode, stripCodes };

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

/**
 * Flight physics, per type, straight out of the 1.8 client's own onUpdate.
 *
 * The server barely tracks a projectile. EntityTracker.trackEntity gives an
 * arrow `64, 20, false` -- a position every **twenty ticks**, and its velocity
 * never again after the spawn packet. A snowball, pearl and potion get 10, a
 * bobber 5; a player gets 3. Nothing about a flying arrow trips the exemptions
 * either: isAirBorne is only set by addVelocity and jump (Entity.java:1429),
 * and its DataWatcher does not change in flight.
 *
 * A vanilla client does not paper over that with interpolation -- it *runs the
 * projectile*. EntityArrow.onUpdate integrates motion every single tick and the
 * once-a-second packet is a correction to a simulation that is already right.
 * So do the same here: given a spawn velocity, twenty ticks of this is what
 * vanilla itself would have drawn.
 *
 * Order matters and is not the obvious one: position first, then drag, then
 * gravity (EntityArrow.java:402,450-453; EntityThrowable.java:282-285).
 *
 * No bobber. EntityFishHook's motion is threaded with rand.nextFloat()
 * (EntityFishHook.java:239-241,471), so it cannot be reproduced without the
 * server's RNG and simulating it would diverge immediately -- and at 5 ticks it
 * is the mildest case anyway. No dropped item either: it lies still.
 */
const FLIGHT = new Map([
    [ENT.ARROW,    { drag: 0.99, gravity: 0.05 }],
    [ENT.SNOWBALL, { drag: 0.99, gravity: 0.03 }],
    [ENT.PEARL,    { drag: 0.99, gravity: 0.03 }],
    [ENT.POTION,   { drag: 0.99, gravity: 0.05 }],
]);

/** Velocity on the wire is blocks per tick x8000. */
const VEL_SCALE = 8000;

/** How far the flight simulation may advance between collision samples. */
const COLLIDE_STEP = 0.25;

/**
 * How long an arrow must hold still before it counts as stuck in a block.
 *
 * A 1.8 arrow that hits something sets inGround and simply stops. The server
 * keeps the entity -- it can still be picked up, and it lingers for 1200 ticks
 * -- but sends nothing further about it, so there is no landed packet to key
 * on. The silence is the signal. Ten flushes is half a second; an arrow in
 * flight moves every single tick, so nothing airborne can reach it.
 */
const LANDED_TICKS = 10;

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
        this.hidden = false;                  // dropped from the console, still tracked here
        this.stillTicks = 0;                  // flushes since this last changed position

        // Flight simulation, for the types in FLIGHT. Position is carried in
        // blocks here rather than in the wire's fixed point: twenty ticks of
        // drag compounding is not something to round at every step.
        this.flying = false;
        this.mx = 0; this.my = 0; this.mz = 0;      // blocks per tick
        this.simX = 0; this.simY = 0; this.simZ = 0; // blocks, absolute
        this.srvX = 0; this.srvY = 0; this.srvZ = 0; // last position the server stated
        this.hasSrv = false;                         // ...has it stated one yet
    }
}

class EntityTranslator {
    constructor({ link, blockmap, config, log = console, isSolid = null }) {
        this.link = link;
        this.bm = blockmap;
        this.log = log;
        // (localX, localY, localZ) -> is there a block there. Optional: without
        // it projectiles still fly, they just do not stop on impact until the
        // server says so.
        this.isSolid = isSolid;
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
        this.stats = { added: 0, dropped: 0, evicted: 0, moves: 0, landed: 0, stepped: 0 };
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
        this.serverPos(e, pkt.x, pkt.y, pkt.z);
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
        this.serverPos(e, pkt.x, pkt.y, pkt.z);
        e.yaw = pkt.yaw & 0xff; e.pitch = pkt.pitch & 0xff;
        // objectData is the shooter's id + 1 for a projectile, so it is never
        // zero and the velocity fields are present. This is the only velocity
        // an arrow will ever be given.
        if (pkt.velocity) this.setVelocity(e, pkt.velocity);
    }

    /** S12 entity_velocity for someone else's projectile. Arrows never get one
     *  (trackEntity passes sendVelocityUpdates false); the throwables do. */
    onVelocity(pkt) {
        const e = this.entities.get(pkt.entityId);
        if (e && pkt.velocity) this.setVelocity(e, pkt.velocity);
    }

    setVelocity(e, v) {
        if (!FLIGHT.has(e.type)) return;
        e.mx = v.x / VEL_SCALE;
        e.my = v.y / VEL_SCALE;
        e.mz = v.z / VEL_SCALE;
        // A projectile spawned with no velocity at all is one already at rest.
        e.flying = (e.mx !== 0 || e.my !== 0 || e.mz !== 0);
    }

    /**
     * A position the server has stated, and the only way one gets in.
     *
     * srvX/Y/Z is the server's own track, kept apart from e.x because e.x is
     * where the *simulation* has a projectile -- and S15's deltas are relative
     * to what the server last said, not to what we extrapolated since. Adding
     * them to a simulated position would compound the difference every twenty
     * ticks instead of correcting it.
     */
    serverPos(e, x, y, z) {
        const restated = e.hasSrv && x === e.srvX && y === e.srvY && z === e.srvZ;
        e.srvX = x; e.srvY = y; e.srvZ = z;
        e.hasSrv = true;
        e.x = x; e.y = y; e.z = z;
        e.moved = true;

        if (!FLIGHT.has(e.type)) return;
        // The same position twice running means it has stopped -- an arrow in a
        // wall. Stop flying it; the reaper takes it from there.
        if (restated) e.flying = false;
        e.simX = x / POS_SCALE; e.simY = y / POS_SCALE; e.simZ = z / POS_SCALE;
    }

    /** S0F spawn_entity_living -- the ender dragon, and nothing else. */
    onMobSpawn(pkt) {
        const type = MOB_TYPES.get(pkt.type);
        if (type === undefined) { this.stats.dropped++; return; }
        const e = this.track(pkt.entityId, type);
        if (!e) return;
        this.serverPos(e, pkt.x, pkt.y, pkt.z);
        e.yaw = pkt.yaw & 0xff; e.pitch = pkt.pitch & 0xff;
        this.applyMetadata(e, pkt.metadata);
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
     *  little under four blocks. Accumulated onto the server's own track, not
     *  onto where the simulation has got to -- see serverPos. */
    onRelMove(pkt, hasLook) {
        const e = this.entities.get(pkt.entityId);
        if (!e) return;
        this.serverPos(e, e.srvX + pkt.dX, e.srvY + pkt.dY, e.srvZ + pkt.dZ);
        if (hasLook) { e.yaw = pkt.yaw & 0xff; e.pitch = pkt.pitch & 0xff; }
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
        this.serverPos(e, pkt.x, pkt.y, pkt.z);
        e.yaw = pkt.yaw & 0xff; e.pitch = pkt.pitch & 0xff;
    }

    /** S13 entity_destroy. */
    onDestroy(pkt) {
        const gone = [];
        for (const eid of pkt.entityIds) {
            const e = this.entities.get(eid);
            // A landed arrow was already taken off the console; do not spend a
            // second removal telling it about something it has forgotten.
            if (this.entities.delete(eid) && !e.hidden) gone.push(eid);
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

        this.stepFlight();
        this.enforceCap(selfX, selfY, selfZ);

        for (const e of this.entities.values()) {
            // `hidden` outranks `added`, so a console attaching mid-game is not
            // handed every arrow already stuck in the map.
            if (!e.added && !e.hidden) { this.sendAdd(e); e.added = true; e.moved = false; }
        }

        // Batch the moves. A frame's worth of ENTITY_MOVE for 128 entities is
        // 2.3 KB, which is nothing on a LAN and one console-side read instead
        // of 128 -- the console drains this inside a 16 ms frame.
        const moving = [];
        for (const e of this.entities.values()) {
            if (!e.moved || e.hidden) continue;
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

        this.reapLanded();
    }

    /**
     * Advance every projectile one tick, exactly as the client that shot it is
     * already doing. See FLIGHT for why this is not optional.
     *
     * Collision is checked against the proxy's own copy of the map, which
     * applyState keeps current with every block the game breaks or places -- so
     * an arrow stops in the wall it hit rather than sailing through it for the
     * up-to-a-second before the server gets around to mentioning it. Any
     * non-air block stops it: erring toward stopping early costs a few
     * centimetres, and the next server position corrects it regardless.
     */
    stepFlight() {
        for (const e of this.entities.values()) {
            if (!e.flying) continue;
            const f = FLIGHT.get(e.type);
            if (!f) continue;

            // Walk the tick's motion in short steps rather than sampling only
            // where it ends up. A drawn bow puts an arrow at about three blocks
            // a tick, so one sample per tick tests points three blocks apart
            // and misses every wall thinner than that -- the arrow would pass
            // clean through the map. Sub-stepping also leaves it resting
            // against the block it hit instead of a tick's flight short of it.
            const n = Math.max(1, Math.ceil(
                Math.sqrt(e.mx * e.mx + e.my * e.my + e.mz * e.mz) / COLLIDE_STEP));
            const sx = e.mx / n, sy = e.my / n, sz = e.mz / n;

            for (let i = 0; i < n; i++) {
                const nx = e.simX + sx, ny = e.simY + sy, nz = e.simZ + sz;
                if (this.blocked(nx, ny, nz)) { e.flying = false; break; }
                e.simX = nx; e.simY = ny; e.simZ = nz;
            }

            if (e.flying) {
                e.mx *= f.drag; e.my *= f.drag; e.mz *= f.drag;
                e.my -= f.gravity;
            }

            e.x = Math.round(e.simX * POS_SCALE);
            e.y = Math.round(e.simY * POS_SCALE);
            e.z = Math.round(e.simZ * POS_SCALE);

            // An arrow points along its flight (EntityArrow.java:406-408).
            // Vanilla eases this by 0.2 per tick; straight from the motion is
            // indistinguishable at arrow speeds and carries no extra state.
            const flat = Math.sqrt(e.mx * e.mx + e.mz * e.mz);
            e.yaw = angleToByte(Math.atan2(e.mx, e.mz) * 180 / Math.PI);
            e.pitch = angleToByte(Math.atan2(e.my, flat) * 180 / Math.PI);

            e.moved = true;
            this.stats.stepped++;
        }
    }

    /** Is the simulation's next point inside a block. Absolute coordinates in,
     *  because that is what the simulation carries. */
    blocked(x, y, z) {
        if (!this.isSolid || !this.map) return false;
        return this.isSolid(Math.floor(x) - this.map.originX,
                            Math.floor(y) - this.map.originY,
                            Math.floor(z) - this.map.originZ);
    }

    /**
     * Take arrows off the console once they have stuck in a block.
     *
     * Over a game these accumulate without bound -- a wall someone shot at ends
     * up studded with them -- and each costs a slot in a 128-entity table and a
     * model on screen for something that will never move again. They stay in
     * this table, so the eventual entity_destroy still matches and the flush
     * loop above never re-adds them.
     *
     * Only arrows. Snowballs, pearls and potions die on impact rather than
     * landing, and a dropped item lying still is exactly the thing you want to
     * see. A bobber does sit motionless, but it is one per rod and it is how
     * you read where someone cast.
     */
    reapLanded() {
        const gone = [];
        for (const e of this.entities.values()) {
            if (e.type !== ENT.ARROW) continue;

            if (e.x === e.stillX && e.y === e.stillY && e.z === e.stillZ) {
                if (++e.stillTicks >= LANDED_TICKS && !e.hidden) {
                    e.hidden = true;
                    this.stats.landed++;
                    if (e.added) gone.push(e.eid);
                }
                continue;
            }

            e.stillX = e.x; e.stillY = e.y; e.stillZ = e.z;
            e.stillTicks = 0;
            // Nothing in 1.8 dislodges a stuck arrow, but a type that can move
            // again should come back rather than stay invisible for good.
            if (e.hidden) { e.hidden = false; this.sendAdd(e); e.added = true; }
        }
        if (gone.length) this.sendRemovals(gone);
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
            // A landed arrow is not on the console at all, so it is the first
            // thing to give up its slot however close it happens to be.
            const tier = e.hidden ? 2
                       : (e.type === ENT.PLAYER || e.type === ENT.DRAGON) ? 0 : 1;
            return { e, tier, d: dx * dx + dy * dy + dz * dz };
        }).sort((a, b) => (a.tier - b.tier) || (a.d - b.d));

        const drop = ranked.slice(this.cap);
        const gone = [];
        for (const { e } of drop) {
            this.entities.delete(e.eid);
            if (e.added && !e.hidden) gone.push(e.eid);
            this.evicted.add(e.eid);
            this.stats.evicted++;
        }
        if (gone.length) this.sendRemovals(gone);
    }

    get count() { return this.entities.size; }

    report() {
        const s = this.stats;
        return `entities ${this.entities.size} live, ${s.added} added, ` +
               `${s.dropped} filtered out, ${s.stepped} flight steps, ` +
               `${s.landed} arrows landed, ` +
               `${s.evicted} evicted, ${s.moves} moves sent`;
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

module.exports = { EntityTranslator, OBJECT_TYPES, MOB_TYPES, LANDED_TICKS,
                   firstColourCode, stripCodes };

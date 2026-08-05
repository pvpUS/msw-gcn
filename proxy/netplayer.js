'use strict';
/*
 * netplayer.js -- console intent in, correct 1.8 packets out.
 *
 * This is the highest-risk file in the proxy, and the risk is not that it
 * crashes. Every item below, left out, produces either a kick or a *silent
 * drop*: the console goes on playing, the server ignores it, and nothing
 * anywhere says so. The list, and what each omission costs:
 *
 *   C0B sprint/sneak edges     server-side sprint is driven entirely by these.
 *                              Sprint locally without them and you get "moved
 *                              too quickly" *and* no sprint knockback.
 *   the sprint-reset asymmetry after an attack the server silently clears its
 *                              own sprint flag and never says so, and vanilla
 *                              never re-sends START_SPRINTING. Re-sending it
 *                              is the classic custom-client tell.
 *   the teleport epoch         MOVEs in flight across an S08 describe a place
 *                              the player no longer is.
 *   packet selection           C03/C04/C05/C06 by what actually changed, with
 *                              Y as the AABB's minY -- not posY.
 *   attack ordering            C09 before C0A before C02: the server computes
 *                              damage from *its* idea of the held item.
 *   the C02 target filter      attacking an item or an arrow is a kick, not a
 *                              no-op.
 *   C0F auto-ack               miss one and the server ignores every later
 *                              window click.
 *
 * The console side of the same contract is source/netgame.c. Between them
 * there is exactly one movement path and one attack path, and both are
 * vanilla's.
 */

const { C, S, Writer, clamp } = require('./gclink');
const { gcYawToMc, engineSlotToWindow } = require('./state');

/** C0BPacketEntityAction.Action, in its wire order. */
const ACTION = {
    START_SNEAKING: 0,
    STOP_SNEAKING: 1,
    STOP_SLEEPING: 2,
    START_SPRINTING: 3,
    STOP_SPRINTING: 4,
};

/** C07PacketPlayerDigging.Action. 0-2 are the dig states the console sends;
 *  3-5 are the ones only this file produces. */
const DIG = {
    START: 0,
    ABORT: 1,
    STOP: 2,
    DROP_ALL: 3,
    DROP_ONE: 4,
    RELEASE_USE_ITEM: 5,
};

/** C02PacketUseEntity.Action. */
const USE_ENTITY = { INTERACT: 0, ATTACK: 1 };

/**
 * "No face", for the C07 statuses that carry no block and for a C08 aimed at
 * nothing. Vanilla writes 255 here; the byte on the wire is 0xFF either way.
 *
 * It has to be spelled -1 rather than 255 because minecraft-data types both
 * fields `i8`, and node's Buffer.writeInt8 *throws* on 255 -- it does not
 * truncate. That throw used to escape the console dispatch and take the whole
 * proxy process down with it, which from the console looks like the server
 * dropping you the instant you release a bow, finish a golden apple or drop an
 * item. Same byte, no exception.
 */
const NO_FACE = -1;

/** C08's cursor for a use-on-nothing. Vanilla passes 0.0F for all three and
 *  writes `(int)(facing * 16.0F)`, so these are zero, not -1. */
const NO_CURSOR = 0;

/** EntityPlayerSP.onUpdateWalkingPlayer's thresholds, verbatim: 9.0E-4 is
 *  0.03 blocks squared, and twenty ticks is how long a perfectly still player
 *  may go without sending a full position. */
const MOVE_EPSILON_SQ = 9.0e-4;
const POSITION_FORCE_TICKS = 20;

/** The three kit items whose use is a *hold* rather than an instant action.
 *  Everything else -- pearls, snowballs, splash potions, both buckets -- fires
 *  on the press and needs no USE_STATE. */
const ITEM_BOW = 261;
const ITEM_GOLDEN_APPLE = 322;
const ITEM_POTION = 373;
const POTION_SPLASH_BIT = 0x4000;

/** Entity types the server accepts a C02 ATTACK against. Anything else is
 *  "Attempting to attack an invalid entity" and an immediate kick, so the
 *  filter is here as well as in the console's ray-trace (T18) -- one of them
 *  is a convenience and the other is the guard, and this is the guard. */
const ATTACKABLE = new Set([0 /* ENT.PLAYER */, 7 /* ENT.DRAGON */]);

class NetPlayer {
    constructor({ link, state, entities, log = console }) {
        this.link = link;
        this.state = state;
        this.entities = entities;
        this.log = log;

        this.client = null;
        this.map = null;

        // What the server currently believes, which is the only thing the
        // diffs below may be taken against.
        this.sentSprint = false;
        this.sentSneak = false;
        this.lastX = 0; this.lastY = 0; this.lastZ = 0;
        this.lastYaw = 0; this.lastPitch = 0;
        this.positionTicks = 0;
        this.haveLast = false;

        this.usingItem = false;
        // C0E's action number: unique per click, echoed back in S32. Starts at
        // 0 and stays inside a signed short, which is what the field is.
        this.clickAction = 0;
        this.stats = { moves: 0, stale: 0, digs: 0, places: 0, attacks: 0,
                       rejected: 0, swings: 0, clicks: 0 };
    }

    attach(client) {
        this.client = client;
        this.sentSprint = false;
        this.sentSneak = false;
        this.haveLast = false;
        this.setUsing(false);
    }

    setMap(m) {
        this.map = m;
        this.haveLast = false;
        this.setUsing(false);
    }

    /**
     * The console went away mid-game. The account is still standing in the
     * world, so leave it in a state nothing is holding down: stop sprinting,
     * stop sneaking, and release any draw. A reconnect then starts from the
     * same blank slate the server has.
     */
    onConsoleDetached() {
        if (this.client && this.state.selfEid >= 0) {
            this.syncActionState(false, false);
            if (this.usingItem) {
                this.client.write('block_dig', {
                    status: DIG.RELEASE_USE_ITEM,
                    location: { x: 0, y: 0, z: 0 }, face: NO_FACE,
                });
            }
        }
        this.usingItem = false;
        this.haveLast = false;
    }

    /** A teleport resets what the server believes about us, so the next MOVE
     *  is measured from there rather than from wherever we were before it. */
    onTeleported() {
        this.lastX = this.state.x;
        this.lastY = this.state.y;
        this.lastZ = this.state.z;
        this.lastYaw = this.state.yaw;
        this.lastPitch = this.state.pitch;
        this.positionTicks = 0;
        this.haveLast = true;
        this.setUsing(false);
    }

    // ---- dispatch ----------------------------------------------------------

    onMessage(type, payload) {
        switch (type) {
            case C.MOVE:       return this.onMove(payload);
            case C.DIG:        return this.onDig(payload);
            case C.PLACE:      return this.onPlace(payload);
            case C.USE_ENTITY: return this.onUseEntity(payload);
            case C.USE_ITEM:   return this.onUseItem(payload);
            case C.SWING:      return this.onSwing();
            case C.HELD_SLOT:  return this.onHeldSlot(payload);
            case C.CHAT:       return this.state.say(payload.toString('ascii'));
            case C.ACTION:     return this.onAction(payload);
            case C.WINDOW_CLICK: return this.onWindowClick(payload);
            default:           return false;
        }
    }

    /** S32 ConfirmTransaction. Ack every one, or the server quietly ignores
     *  all further window clicks -- including the ones the plugin's own GUIs
     *  make on our behalf. */
    onTransaction(pkt) {
        if (!this.client) return;
        this.client.write('transaction', {
            windowId: pkt.windowId, action: pkt.action, accepted: true,
        });
    }

    // ---- movement ----------------------------------------------------------

    /**
     * One 20 Hz MOVE becomes exactly what EntityPlayerSP.onUpdateWalkingPlayer
     * would have sent: the C0B edges, then one of C03/C04/C05/C06 chosen by
     * what actually changed.
     *
     * Layout: double x,y,z; float yaw,pitch; u8 flags; u8 epoch. [34 B]
     */
    onMove(buf) {
        if (buf.length < 34 || !this.client || !this.map) return;

        const epoch = buf.readUInt8(33);
        if (epoch !== (this.state.teleportEpoch & 0xff)) {
            // In flight when the server moved us. Acting on it would report a
            // position the server has already overruled, and the server's
            // answer to that is a teleport back.
            this.stats.stale++;
            return;
        }

        const x = buf.readDoubleBE(0) + this.map.originX;
        const y = buf.readDoubleBE(8) + this.map.originY;   // the AABB's minY
        const z = buf.readDoubleBE(16) + this.map.originZ;
        const yaw = gcYawToMc(buf.readFloatBE(24));
        const pitch = -buf.readFloatBE(28);
        const flags = buf.readUInt8(32);
        const onGround = (flags & 1) !== 0;
        const sprinting = (flags & 2) !== 0;
        const sneaking = (flags & 4) !== 0;

        this.syncActionState(sprinting, sneaking);

        if (!this.haveLast) {
            // Nothing to diff against yet: send the whole thing once so the
            // server has a baseline, then start diffing.
            this.lastX = x; this.lastY = y; this.lastZ = z;
            this.lastYaw = yaw; this.lastPitch = pitch;
            this.positionTicks = 0;
            this.haveLast = true;
            this.client.write('position_look', { x, y, z, yaw, pitch, onGround });
            this.stats.moves++;
            return;
        }

        const dx = x - this.lastX, dy = y - this.lastY, dz = z - this.lastZ;
        const movedFar = dx * dx + dy * dy + dz * dz > MOVE_EPSILON_SQ;
        const moved = movedFar || this.positionTicks >= POSITION_FORCE_TICKS;
        const looked = yaw !== this.lastYaw || pitch !== this.lastPitch;

        if (moved && looked) {
            this.client.write('position_look', { x, y, z, yaw, pitch, onGround });
        } else if (moved) {
            this.client.write('position', { x, y, z, onGround });
        } else if (looked) {
            this.client.write('look', { yaw, pitch, onGround });
        } else {
            this.client.write('flying', { onGround });
        }
        this.stats.moves++;

        this.positionTicks++;
        if (moved) {
            this.lastX = x; this.lastY = y; this.lastZ = z;
            this.positionTicks = 0;
        }
        if (looked) { this.lastYaw = yaw; this.lastPitch = pitch; }

        // The server tracks our position for the entity cap and the map
        // lookup; without this it would only ever hear about a teleport.
        this.state.x = x; this.state.y = y; this.state.z = z;
        this.state.yaw = yaw; this.state.pitch = pitch;
    }

    /**
     * The sprint and sneak edges. Server-side sprint exists *only* because of
     * these -- there is no flag on a movement packet that says "sprinting" --
     * so a client that moves at sprint speed without sending START_SPRINTING
     * is a client the server thinks is walking too fast.
     */
    syncActionState(sprinting, sneaking) {
        const eid = this.state.selfEid;
        if (eid < 0) return;
        if (sprinting !== this.sentSprint) {
            this.client.write('entity_action', {
                entityId: eid, jumpBoost: 0,
                actionId: sprinting ? ACTION.START_SPRINTING : ACTION.STOP_SPRINTING,
            });
            this.sentSprint = sprinting;
        }
        if (sneaking !== this.sentSneak) {
            this.client.write('entity_action', {
                entityId: eid, jumpBoost: 0,
                actionId: sneaking ? ACTION.START_SNEAKING : ACTION.STOP_SNEAKING,
            });
            this.sentSneak = sneaking;
        }
    }

    // ---- blocks ------------------------------------------------------------

    /** u8 status, s16 x, y, z, u8 face. [8 B]
     *
     *  The face is clamped to the six that exist rather than forwarded. Both
     *  ends read it out of a u8 and minecraft-data writes it as an i8, so
     *  anything above 127 is not a wrong face -- it is a throw, and the two
     *  should not be the same failure. */
    onDig(buf) {
        if (buf.length < 8 || !this.client || !this.map) return;
        const status = buf.readUInt8(0);
        if (status > DIG.STOP) return;          // 3-5 are ours to send, not the console's
        this.client.write('block_dig', {
            status,
            location: {
                x: buf.readInt16BE(1) + this.map.originX,
                y: buf.readInt16BE(3) + this.map.originY,
                z: buf.readInt16BE(5) + this.map.originZ,
            },
            face: clamp(buf.readUInt8(7), 0, 5),
        });
        this.stats.digs++;
    }

    /** s16 x, y, z, u8 face, u8 curX, curY, curZ. [10 B]
     *
     *  The held stack is filled in here rather than sent over the link: the
     *  console mirrors the inventory but the server's own copy is what it
     *  validates against, and the two are only equal between updates. */
    onPlace(buf) {
        if (buf.length < 10 || !this.client || !this.map) return;
        this.client.write('block_place', {
            location: {
                x: buf.readInt16BE(0) + this.map.originX,
                y: buf.readInt16BE(2) + this.map.originY,
                z: buf.readInt16BE(4) + this.map.originZ,
            },
            direction: clamp(buf.readUInt8(6), 0, 5),
            heldItem: this.state.heldItem(),
            // Sixteenths of a block, and the console already clamps them --
            // but these are i8 on the wire, so an out-of-range one would throw
            // rather than land in the wrong corner of the block.
            cursorX: clamp(buf.readUInt8(7), 0, 15),
            cursorY: clamp(buf.readUInt8(8), 0, 15),
            cursorZ: clamp(buf.readUInt8(9), 0, 15),
        });
        this.stats.places++;
    }

    // ---- items -------------------------------------------------------------

    /** u8 action: 0 start, 1 release. */
    onUseItem(buf) {
        if (!buf.length || !this.client) return;
        const release = buf.readUInt8(0) === 1;

        if (release) {
            // C07 RELEASE_USE_ITEM. Position and face are ignored for this
            // status but the packet still carries them.
            this.client.write('block_dig', {
                status: DIG.RELEASE_USE_ITEM,
                location: { x: 0, y: 0, z: 0 },
                face: NO_FACE,
            });
            this.setUsing(false);
            return;
        }

        // "Use the held item on nothing", which is what a bow draw, a bite of
        // a golden apple and a thrown pearl all are: C08 at (-1,-1,-1) with
        // face 255 and a zero cursor.
        this.client.write('block_place', {
            location: { x: -1, y: -1, z: -1 },
            direction: NO_FACE,
            heldItem: this.state.heldItem(),
            cursorX: NO_CURSOR, cursorY: NO_CURSOR, cursorZ: NO_CURSOR,
        });
        this.setUsing(this.heldItemIsHeldDown());
    }

    /** Does the held stack start a hold (bow, food, a drinkable potion) rather
     *  than acting instantly? Only the answer matters -- what the item *does*
     *  is the server's business. */
    heldItemIsHeldDown() {
        const it = this.state.heldItem();
        if (!it || it.blockId === undefined || it.blockId < 0) return false;
        const id = it.blockId;
        if (id === ITEM_BOW || id === ITEM_GOLDEN_APPLE) return true;
        if (id === ITEM_POTION) return ((it.itemDamage || 0) & POTION_SPLASH_BIT) === 0;
        return false;
    }

    /** Tell the console to apply vanilla's 0.2x movement while a use is
     *  running. Predicting this console-side would be closer to vanilla, but
     *  the server is the one that decides whether the bow was ever drawn, and
     *  a disagreement about that is thirty-two ticks of divergent position. */
    setUsing(active) {
        active = !!active;
        if (active === this.usingItem) return;
        this.usingItem = active;
        this.link.send(S.USE_STATE, new Writer(1).u8(active ? 1 : 0).done());
    }

    /** u8 slot (0-8). */
    onHeldSlot(buf) {
        if (!buf.length || !this.client) return;
        const slot = buf.readUInt8(0) & 7;
        if (slot === this.state.heldSlot) return;
        this.state.heldSlot = slot;
        this.client.write('held_item_slot', { slotId: slot });
        // Changing weapons cancels a draw, on the server and therefore here.
        this.setUsing(false);
    }

    /** u8 GCLINK_ACTION_*. */
    onAction(buf) {
        if (!buf.length || !this.client) return;
        const a = buf.readUInt8(0);
        if (a === 0 || a === 1) {
            this.client.write('block_dig', {
                status: a === 1 ? DIG.DROP_ALL : DIG.DROP_ONE,
                location: { x: 0, y: 0, z: 0 },
                face: NO_FACE,
            });
        } else if (a === 3) {
            this.state.sendAllSlots();
            this.state.sendHealth();
            this.state.sendGameMode();
        }
    }

    // ---- the inventory window ------------------------------------------------

    /**
     * u8 engine slot, u8 button. [2 B] -> C0E ClickWindow.
     *
     * Three of C0E's six fields are the proxy's rather than the console's, and
     * each for its own reason:
     *
     *   slot        the console counts slots the engine's way (hotbar first);
     *               the server counts them the window's way (hotbar last, at
     *               36-44). engineSlotToWindow is the only place that converts.
     *   item        the stack the client believed was there. The server checks
     *               it, and the proxy's mirror is what the server itself last
     *               said -- so it agrees by construction where the console's
     *               copy only agrees between updates.
     *   action      a per-window counter the server echoes in S32. It has to be
     *               unique and it has to come from whoever is sending, which is
     *               here.
     *
     * A rejected click is not an error to handle: the server answers with S2F
     * or S30 and the window snaps back, which is exactly what the console's
     * local prediction needs to hear.
     */
    onWindowClick(buf) {
        if (buf.length < 2 || !this.client) return;
        const engine = buf.readUInt8(0);
        const slot = engineSlotToWindow(engine);
        if (slot < 0) { this.stats.rejected++; return; }

        this.clickAction = (this.clickAction + 1) & 0x7fff;
        this.client.write('window_click', {
            windowId: 0,
            slot,
            mouseButton: buf.readUInt8(1) === 1 ? 1 : 0,
            action: this.clickAction,
            mode: 0,                       // a plain click; no shift, no drag
            item: this.state.slotAt(engine),
        });
        this.stats.clicks++;
    }

    // ---- combat ------------------------------------------------------------

    onSwing() {
        if (!this.client) return;
        this.client.write('arm_animation', {});
        this.stats.swings++;
    }

    /** s32 eid, u8 action. [5 B] */
    onUseEntity(buf) {
        if (buf.length < 5 || !this.client) return;
        const eid = buf.readInt32BE(0);
        const attack = buf.readUInt8(4) === 1;

        if (eid === this.state.selfEid) { this.stats.rejected++; return; }
        const type = this.entities ? this.entities.typeOf(eid) : undefined;
        if (attack && !ATTACKABLE.has(type)) {
            // Either a projectile/item (a kick) or an entity we have already
            // forgotten (a no-op the server would answer with a kick anyway,
            // since it is gone there too).
            this.stats.rejected++;
            return;
        }

        // The server computes damage from *its* currentItem, so a slot change
        // that has not landed yet would swing the wrong weapon. C09, then the
        // swing, then the attack -- vanilla's order in EntityPlayerSP.
        this.client.write('use_entity', {
            target: eid, mouse: attack ? USE_ENTITY.ATTACK : USE_ENTITY.INTERACT,
        });
        this.stats.attacks++;

        // Deliberately *not* touching sentSprint. The server has just cleared
        // its own sprint flag as part of applying knockback and will never say
        // so; vanilla's serverSprintState stays true, so no START_SPRINTING is
        // re-sent and the player has to release W to sprint again. That
        // asymmetry is the whole of the 1.8 W-tap, and a client that "fixes"
        // it is visibly not vanilla.
    }

    report() {
        const s = this.stats;
        return `player ${s.moves} moves (${s.stale} stale), ${s.digs} digs, ` +
               `${s.places} places, ${s.swings} swings, ${s.attacks} attacks ` +
               `(${s.rejected} filtered), ${s.clicks} window clicks`;
    }
}

module.exports = { NetPlayer, ACTION, DIG, USE_ENTITY };

'use strict';
/*
 * state.js -- everything about the player and the game that is not blocks and
 * not entities: health, inventory, held slot, XP, game mode, teleports, chat
 * and the action bar.
 *
 * Ownership note. The 1.8 protocol state machine belongs to T22, and almost all
 * of it lives elsewhere -- the C0B sprint/sneak edges, the C03/C04/C05/C06
 * movement synthesis, attack ordering, the C02 target filter. Three things are
 * here anyway, because a proxy that cannot hold a session cannot be tested at
 * all and none of them is movement:
 *
 *   - the C06 reply to S08. Until it lands within 0.25 blocks the server's
 *     hasMoved stays false and *every* subsequent packet is silently dropped.
 *     It echoes the server's coordinates, never ours, which is also why it is
 *     safe to send before any movement code exists.
 *   - the teleport epoch. Bumped here, echoed by the console in every MOVE, and
 *     used by T22 to throw away MOVEs that were in flight across a teleport.
 *   - the outbound chat queue, truncated to 100 characters and rate-limited,
 *     because Spigot kicks on chat spam and /bordersize goes through it.
 */

const { EventEmitter } = require('events');
const { S, Writer, GAME, AIR } = require('./gclink');
const chat = require('./chat');

/** 1.8 window 0 slot -> the engine's InventoryPlayer index (inventory.h):
 *  0-8 hotbar, 9-35 storage, 36-39 armor as boots, legs, chest, helmet. The
 *  crafting grid and its output have no console counterpart. */
function windowSlotToEngine(slot) {
    if (slot >= 36 && slot <= 44) return slot - 36;      // hotbar
    if (slot >= 9 && slot <= 35) return slot;            // storage
    switch (slot) {
        case 5: return 39;   // helmet
        case 6: return 38;   // chestplate
        case 7: return 37;   // leggings
        case 8: return 36;   // boots
        default: return -1;  // 0 crafting output, 1-4 crafting grid
    }
}

/** The inverse, for the C0E the console's inventory clicks become. A click has
 *  to name the slot in the server's numbering, and getting this backwards would
 *  move a real item somewhere neither end expected. Round-tripped in
 *  selftest.js against every slot windowSlotToEngine accepts. */
function engineSlotToWindow(e) {
    if (e >= 0 && e <= 8) return e + 36;                 // hotbar
    if (e >= 9 && e <= 35) return e;                     // storage
    switch (e) {
        case 39: return 5;   // helmet
        case 38: return 6;   // chestplate
        case 37: return 7;   // leggings
        case 36: return 8;   // boots
        default: return -1;
    }
}

/** GC_S_INV_SET's slot index for the stack on the cursor -- one past the 40
 *  real ones. Mirrors GCLINK_INV_CURSOR in source/gclink.h. */
const INV_CURSOR = 40;

/** Spigot's chat spam kick is generous, but the plugin's own commands go
 *  through this queue too, so keep well clear of it. */
const CHAT_MIN_INTERVAL_MS = 1100;
const CHAT_MAX_CHARS = 100;

class StateTranslator extends EventEmitter {
    constructor({ link, blockmap, config, log = console }) {
        super();
        this.link = link;
        this.bm = blockmap;
        this.log = log;
        this.config = config;

        this.client = null;
        this.selfEid = -1;
        this.gameMode = 0;
        this.health = 20;
        this.teleportEpoch = 0;
        this.game = GAME.LOBBY;
        this.map = null;

        // Absolute position, as the server last told us. The entity cap ranks
        // by distance from here, so it has to be tracked even in a build that
        // sends no movement of its own.
        this.x = 0; this.y = 0; this.z = 0;
        this.yaw = 0; this.pitch = 0;

        this.slots = new Array(40).fill(null);
        // The stack on the cursor. The server owns it exactly as it owns the
        // slots, and it arrives the same way -- S2F with windowId -1.
        this.cursor = null;
        // Which hotbar slot the server believes is held. C08 BlockPlacement
        // carries the held stack and the server validates it, so this has to
        // track S09 *and* the console's own C09 -- see netplayer.onHeldSlot.
        this.heldSlot = 0;
        this.borderOptedIn = false;
        this._chatQueue = [];
        this._chatAt = 0;
        this._chatTimer = null;
    }

    attach(client) { this.client = client; }

    setMap(m) {
        this.map = m;
        // The hub is shipped and streamed exactly like a map -- it is scanned
        // the same way and the console loads it the same way -- but standing in
        // it is not being in a game, so it keeps the LOBBY state and none of
        // the per-game chatter that follows one.
        this.setGameState(m && !m.hub ? GAME.WAITING : GAME.LOBBY);
        if (m && !m.hub && this.config.sendBorderSize && !this.borderOptedIn) {
            this.say('/bordersize');
        }
    }

    /** A fresh console knows none of this; state it all again.
     *
     *  The teleport is not optional here even though nothing has moved: the
     *  console refuses to send any MOVE until it has had one, because before
     *  that it does not know the epoch to echo. A console that reconnects
     *  mid-game and never hears one stands perfectly still until the server's
     *  next S08 -- which, in a game where nothing is teleporting you, may be
     *  never. */
    onConsoleAttached() {
        this.sendHealth();
        this.sendGameMode();
        this.sendGameState();
        this.sendAllSlots();
        this.sendTeleport();
    }

    // ---- identity and position ---------------------------------------------

    /** S01 login. */
    onLogin(pkt) {
        this.selfEid = pkt.entityId;
        this.gameMode = pkt.gameMode & 0x7;
        this.log.info(`joined as entity ${this.selfEid}, game mode ${this.gameMode}`);
        this.sendGameMode();
    }

    /**
     * S08 PlayerPosLook. In 1.8 the `flags` byte marks each field as relative
     * to the client's current value; the server sends absolutes for a real
     * teleport and relatives for a nudge, and the confirmation has to echo
     * whatever the *server* meant, resolved.
     */
    onPosition(pkt) {
        const f = pkt.flags || 0;
        this.x = (f & 0x01) ? this.x + pkt.x : pkt.x;
        this.y = (f & 0x02) ? this.y + pkt.y : pkt.y;
        this.z = (f & 0x04) ? this.z + pkt.z : pkt.z;
        this.yaw = (f & 0x08) ? this.yaw + pkt.yaw : pkt.yaw;
        this.pitch = (f & 0x10) ? this.pitch + pkt.pitch : pkt.pitch;

        // C06. Echo the resolved server position verbatim -- sending our own
        // would leave hasMoved false and silently void everything after it.
        if (this.client) {
            this.client.write('position_look', {
                x: this.x, y: this.y, z: this.z,
                yaw: this.yaw, pitch: this.pitch, onGround: true,
            });
        }

        this.teleportEpoch = (this.teleportEpoch + 1) & 0xff;
        this.emit('position', this.x, this.y, this.z);
        this.sendTeleport();
        // Everything the outbound movement path believes about where we are is
        // now wrong, including the MOVEs already in flight from the console.
        // The epoch above discards those; this re-baselines the rest (T22).
        this.emit('teleport');
    }

    /** Local (map-relative) feet position, in the console's coordinate space. */
    sendTeleport() {
        if (!this.map) return;
        // MC yaw 0 faces +Z and positive pitch looks down; the engine's yaw 0
        // faces -Z and positive pitch looks up. Converted once, here, at the
        // boundary -- a sign error surfaces as "moved wrongly", not as a
        // visual bug, so it is worth having in exactly one place.
        this.link.send(S.TELEPORT, new Writer(33)
            .f64(this.x - this.map.originX)
            .f64(this.y - this.map.originY)
            .f64(this.z - this.map.originZ)
            .f32(mcYawToGc(this.yaw))
            .f32(-this.pitch)
            .u8(this.teleportEpoch)
            .done());
    }

    // ---- vitals -------------------------------------------------------------

    /** S06 update_health. Hunger is deliberately ignored: the plugin pins food
     *  and saturation at 20, so there is nothing for the console to show. */
    onHealth(pkt) {
        this.health = pkt.health;
        this.sendHealth();
    }

    sendHealth() { this.link.send(S.HEALTH, new Writer(4).f32(this.health).done()); }

    /** S1F set_experience. On this server the level is the ranked elo. */
    onExperience(pkt) {
        this.link.send(S.XP, new Writer(10)
            .f32(pkt.experienceBar).i16(pkt.level).i32(pkt.totalExperience).done());
    }

    /** S2B game_state_change. Reason 3 is a game-mode change, which is how
     *  death works here -- the plugin cancels lethal damage and drops you into
     *  spectator instead, so there is no S07 Respawn to wait for (T26). */
    onGameStateChange(pkt) {
        if (pkt.reason !== 3) return;
        const mode = pkt.gameMode | 0;
        if (mode === this.gameMode) return;
        this.gameMode = mode;
        this.log.info(`game mode -> ${mode}${mode === 3 ? ' (spectator)' : ''}`);
        this.sendGameMode();
        this.emit('gamemode', mode);
    }

    sendGameMode() { this.link.send(S.GAME_MODE, new Writer(1).u8(this.gameMode).done()); }

    /** S12 entity_velocity aimed at us: knockback, and applying it is not
     *  optional -- ignore it and the server moves you while you do not. The
     *  wire unit is 1/8000 of a block per tick. */
    onSelfVelocity(pkt) {
        const v = pkt.velocity || { x: 0, y: 0, z: 0 };
        this.link.send(S.SELF_VELOCITY, new Writer(24)
            .f64(v.x / 8000).f64(v.y / 8000).f64(v.z / 8000)
            .done());
    }

    // ---- inventory ----------------------------------------------------------

    /**
     * S2F set_slot.
     *
     * Window -1 slot -1 is the stack on the cursor. That used to be dropped,
     * which was fine while the console could not click: nothing could put an
     * item on the cursor, so nothing needed to take it off. Now that a click
     * goes out as C0E and is predicted locally, this is the only packet that
     * can put the prediction right -- without it, a click the server resolved
     * differently leaves a stack stuck to the cursor with nothing able to
     * clear it.
     */
    onSetSlot(pkt) {
        if (pkt.windowId === -1 && pkt.slot === -1) {
            this.cursor = pkt.item;
            this.sendCursor();
            return;
        }
        if (pkt.windowId !== 0) return;
        const e = windowSlotToEngine(pkt.slot);
        if (e < 0) return;
        this.slots[e] = pkt.item;
        this.sendSlots([e]);
    }

    /** S30 window_items: the whole of window 0 at once, on join and on respawn. */
    onWindowItems(pkt) {
        if (pkt.windowId !== 0) return;
        const changed = [];
        (pkt.items || []).forEach((item, slot) => {
            const e = windowSlotToEngine(slot);
            if (e < 0) return;
            this.slots[e] = item;
            changed.push(e);
        });
        this.sendSlots(changed);
    }

    sendAllSlots() {
        this.sendSlots(this.slots.map((_, i) => i));
        this.sendCursor();
    }

    /** The carried stack, as INV_SET's one out-of-range slot index. */
    sendCursor() {
        this.link.send(S.INV_SET, this.slotRecords([INV_CURSOR]).done());
    }

    sendSlots(indices) {
        if (!indices.length) return;
        this.link.send(S.INV_SET, this.slotRecords(indices).done());
    }

    /** { u8 slot, u16 itemId, u8 count, u16 meta } per index, in engine ids.
     *  INV_CURSOR reads from `cursor` rather than the slot array -- it is the
     *  stack between slots, not one of them. */
    slotRecords(indices) {
        const w = new Writer(indices.length * 6);
        for (const e of indices) {
            const item = e === INV_CURSOR ? this.cursor : this.slots[e];
            const empty = !item || item.blockId === undefined || item.blockId < 0
                       || !item.itemCount;
            if (empty) {
                w.u8(e).u16(AIR).u8(0).u16(0);
            } else {
                const engine = this.bm.toItem(item.blockId, item.itemDamage || 0);
                w.u8(e)
                 .u16(engine === null || engine === undefined ? AIR : engine)
                 .u8(Math.min(item.itemCount, 255))
                 .u16(item.itemDamage || 0);
            }
        }
        return w;
    }

    /** S09 held_item_slot -- absolute, which is why the console needs an
     *  absolute setter rather than InventoryPlayer's relative scroll. */
    onHeldItemSlot(pkt) {
        this.heldSlot = pkt.slot & 7;
        this.link.send(S.HELD_SLOT, new Writer(1).u8(this.heldSlot).done());
    }

    /** The stack the server thinks is in our hand, in its own wire form, ready
     *  to go straight back out in a C08. An empty hand is blockId -1, which is
     *  how the 1.8 slot format spells "nothing". */
    heldItem() {
        return this.slotAt(this.heldSlot);
    }

    /** The same, for any engine slot -- what C0E has to claim was there. */
    slotAt(engine) {
        const it = this.slots[engine];
        if (!it || it.blockId === undefined || it.blockId < 0 || !it.itemCount) {
            return { blockId: -1 };
        }
        return it;
    }

    // ---- chat ---------------------------------------------------------------

    /**
     * S02. Position byte 2 is the action bar -- the plugin's real UI surface,
     * where the border radius and every must-not-miss line goes -- and the
     * console draws that always-on while the chat log stays hidden until asked
     * for. Sending it as chat would hide the only thing that had to be seen.
     */
    onChat(pkt) {
        const parsed = chat.parse(pkt.message);
        if (!parsed.text.trim()) return;

        if (pkt.position === 2) {
            this.link.send(S.ACTION_BAR, new Writer(parsed.text.length + 1)
                .u8(parsed.colour).raw(parsed.text.slice(0, 120)).done());
            return;
        }

        for (const line of parsed.lines) {
            this.link.send(S.CHAT, new Writer(line.text.length + 1)
                .u8(line.colour).raw(line.text.slice(0, 200)).done());
        }
        this.watchGameState(parsed.text);
        if (this.config.log) this.log.chat(parsed.text);
    }

    /**
     * The plugin sends no title, no boss bar and no sidebar scoreboard, so its
     * broadcasts are the only signal for where a game is in its life cycle.
     * Matched against the exact strings in MapStatus.startGame.
     */
    watchGameState(text) {
        if (text.includes('Border Analysis Enabled')) this.borderOptedIn = true;
        else if (text.includes('Border Analysis Disabled')) this.borderOptedIn = false;

        // Broadcasts reach the hub too, and a game starting somewhere else is
        // not this session entering one.
        if (!this.map || this.map.hub) return;
        if (text.includes('Game has started')) this.setGameState(GAME.PLAYING);
        else if (text.includes('Game starting in')) this.setGameState(GAME.WAITING);
        else if (/\bwins!/.test(text)) this.setGameState(GAME.ENDED);
    }

    setGameState(g) {
        if (g === this.game) return;
        this.game = g;
        this.sendGameState();
        this.emit('gamestate', g);
    }

    sendGameState() { this.link.send(S.GAME_STATE, new Writer(1).u8(this.game).done()); }

    /**
     * Outbound chat and commands. Everything the console can make the account
     * say goes through one queue so the rate limit cannot be bypassed by a
     * second call site.
     */
    say(text) {
        const s = chat.fold(String(text)).replace(/§./g, '').slice(0, CHAT_MAX_CHARS).trim();
        if (!s) return;
        this._chatQueue.push(s);
        this._pumpChat();
    }

    _pumpChat() {
        if (this._chatTimer || !this._chatQueue.length) return;
        const wait = Math.max(0, this._chatAt + CHAT_MIN_INTERVAL_MS - Date.now());
        this._chatTimer = setTimeout(() => {
            this._chatTimer = null;
            const s = this._chatQueue.shift();
            if (s && this.client) {
                this._chatAt = Date.now();
                this.client.write('chat', { message: s });
                this.log.info(`say: ${s}`);
            }
            this._pumpChat();
        }, wait);
    }

    stop() {
        if (this._chatTimer) { clearTimeout(this._chatTimer); this._chatTimer = null; }
        this._chatQueue.length = 0;
    }
}

/**
 * MC yaw 0 faces +Z; the engine's faces -Z. Normalised to (-180, 180].
 *
 * **A reflection, not an offset.** The tempting `yaw - 180` is right at 0 and
 * 180 and wrong everywhere else: Minecraft's yaw 90 faces west (-X) and the
 * engine's forward is `(-sin y, -cos y)`, which faces west at *+*90, so the
 * two conventions run in opposite directions. Positions are not mirrored --
 * local is a plain subtraction of the map origin -- so the yaw cannot be
 * either, and `180 - yaw` is the conversion that agrees with them.
 *
 * Getting this backwards mirrors every entity's facing on the console (a
 * player walking west appears to moonwalk east) and, once T22 sends movement,
 * mirrors the yaw the server is told about as well.
 */
function mcYawToGc(yaw) {
    let g = 180 - yaw;
    while (g <= -180) g += 360;
    while (g > 180) g -= 360;
    return g;
}

/** The inverse, for T22's outbound movement -- which is the same reflection,
 *  so this is its own inverse up to normalisation. Round-trip tested in
 *  selftest.js: a sign error here reads as "moved wrongly", not as anything
 *  visible. */
function gcYawToMc(yaw) {
    let m = 180 - yaw;
    while (m < 0) m += 360;
    while (m >= 360) m -= 360;
    return m;
}

module.exports = { StateTranslator, windowSlotToEngine, engineSlotToWindow,
                   INV_CURSOR, mcYawToGc, gcYawToMc };

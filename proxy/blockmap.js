'use strict';
/*
 * blockmap.js -- the 1.8 wire <-> engine translation, from blockmap.json.
 *
 * Both sides of the link are generated from data/blockids.txt by
 * tools/gen_blockmap.py: the console's atlas, shape and property tables are
 * baked into the DOL, and this JSON is the proxy's copy. The palette hash goes
 * in HELLO so a stale one fails at the handshake rather than rendering a whole
 * map as the wrong blocks.
 *
 * The engine's palette is the union of what the 31 map scans contained, plus
 * the handful of blocks only gameplay produces, so it is *not* all of 1.8. Most
 * of what is missing is decoration the plugin never places, but Lights Out mode
 * rewrites an ENDER_PORTAL block above every player every tick, and that state
 * is deliberately unmapped. Unmapped states are dropped, never substituted:
 * a magenta placeholder blinking 20 times a second would be worse than a map
 * that quietly stays as it was.
 */

const fs = require('fs');
const path = require('path');

class BlockMap {
    constructor(doc) {
        this.paletteHash = doc.paletteHash >>> 0;
        this.blockCount = doc.blockCount;
        this.sentinel = doc.sentinel;
        this.unsupported = doc.unsupported || {};

        // A flat table over every 1.8 state id (4096 blocks x 16 metas). -1 is
        // "no engine block", which for state 0 means air and everywhere else
        // means drop. One typed array beats a 568-key object in the diff's
        // inner loop, where this is read millions of times on join.
        this.states = new Int16Array(65536).fill(-1);
        for (const [k, v] of Object.entries(doc.states)) this.states[k | 0] = v;

        this.items = new Map();          // itemId -> {name, engine}
        for (const [k, v] of Object.entries(doc.items || {})) this.items.set(k | 0, v);
        this.itemDamage = new Map();     // itemId -> {mask, values}
        for (const [k, v] of Object.entries(doc.itemDamage || {})) this.itemDamage.set(k | 0, v);

        this.missing = new Map();        // state id -> times seen, for the log
    }

    /** Engine global block id for a 1.8 state (id << 4 | meta). -1 = air or
     *  unmappable; `isAir` separates the two when it matters. */
    toGlobal(stateId) {
        const g = this.states[stateId & 0xffff];
        if (g < 0 && stateId >= 16) {
            this.missing.set(stateId, (this.missing.get(stateId) || 0) + 1);
        }
        return g;
    }

    /** State ids 0..15 are all air: block id 0, any meta. */
    isAir(stateId) { return stateId < 16; }

    /**
     * Engine item id for a 1.8 item stack. Item ids <= 255 are placeable
     * blocks and resolve through the block states instead; anything else is an
     * atlas tile index past blockCount, or null where the art does not exist
     * yet (T13 draws the rest of the kit).
     */
    toItem(itemId, damage = 0) {
        if (itemId === null || itemId === undefined || itemId < 0) return null;
        const dmg = this.itemDamage.get(itemId);
        if (dmg) {
            const hit = dmg.values[String(damage & dmg.mask)];
            if (hit) return hit.engine;
        }
        if (itemId <= 255) {
            const g = this.states[((itemId << 4) | (damage & 15)) & 0xffff];
            return g >= 0 ? g : null;
        }
        const it = this.items.get(itemId);
        return it ? it.engine : null;
    }

    /** The states seen on the wire that this palette cannot express, worst
     *  first. Worth printing once at shutdown: a long tail here is how a real
     *  palette gap shows up, and it is invisible from the console. */
    missingReport(limit = 12) {
        return [...this.missing.entries()]
            .sort((a, b) => b[1] - a[1])
            .slice(0, limit)
            .map(([s, n]) => `${s >> 4}:${s & 15} x${n}`);
    }
}

function load(file = path.join(__dirname, 'blockmap.json')) {
    try {
        return new BlockMap(JSON.parse(fs.readFileSync(file, 'utf8')));
    } catch (e) {
        throw new Error(`cannot read ${file} (run: python tools/gen_blockmap.py)` +
                        `\n  ${e.message}`);
    }
}

module.exports = { load, BlockMap };

'use strict';
/*
 * world.js -- make the console's local map match the server's live one.
 *
 * The console never decodes a chunk. It loads the same .mworld the proxy has,
 * and the proxy sends only the difference: on join, the server's chunks diffed
 * against the decoded scan (which is what makes joining a game two minutes in
 * work at all), and from then on, S23 / S22 block changes as they land.
 *
 * Three coordinate spaces meet here, and mixing them is the failure this file
 * is mostly written to prevent:
 *   absolute  - what the server sends. One world, 31 maps at fixed origins.
 *   local     - absolute minus the map's origin. What the .mworld stores, what
 *               World_GetBlock takes, and everything on the GCLink wire.
 *   cell      - a linear index into the tracking array. Never leaves this file.
 */

const { EventEmitter } = require('events');
const mworld = require('./mworld');
const { MapDB } = require('./mapdb');
const { S, Writer, AIR, BLOCK_SET_MAX } = require('./gclink');

const SECTION_BLOCK_BYTES = 4096 * 2;   // 4096 blocks, u16 LE, id << 4 | meta
const SECTION_LIGHT_BYTES = 2048;       // half a byte per block
const BIOME_BYTES = 256;

/** Chunk columns held while no map is selected yet. The server usually sends
 *  S08 before the chunks it triggered, but not reliably, and a column arriving
 *  a tick early would otherwise leave a permanent hole in the join diff. */
const PREBUFFER_COLUMNS = 96;

class WorldTranslator extends EventEmitter {
    constructor({ mapdb, blockmap, link, config, log = console, getSelfEid }) {
        super();
        this.mapdb = mapdb;
        this.bm = blockmap;
        this.link = link;
        this.log = log;
        this.getSelfEid = getSelfEid || (() => -1);
        this.matchRadius = config.mapMatchRadius ?? 250;
        this.maxPerFlush = config.maxBlocksPerFrame ?? 4096;

        this.map = null;        // mapdb entry, or null in the hub
        this.world = null;      // the console's map, as the proxy believes it
        this.deltas = new Map();  // cell -> id, everything changed since load
        this.pending = new Map(); // cell -> id, not yet on the wire
        this.prebuf = [];       // columns that arrived before the map was known
        this._xyz = [0, 0, 0];

        this.stats = { sent: 0, frames: 0, diffed: 0, outside: 0, unmapped: 0 };
    }

    // ---- map selection -----------------------------------------------------

    /** From S08 PlayerPosLook, and from wherever the player first appears. */
    onPosition(x, y, z) {
        const m = this.mapdb.findByPosition(x, y, z, this.matchRadius);
        if (!m) return this.toLobby();
        if (this.map && this.map.name === m.name) return;
        this.selectMap(m);
    }

    selectMap(m) {
        const t0 = Date.now();
        this.map = m;
        this.world = mworld.load(m.name, {
            marginXZ: this.mapdb.margin.xz, marginY: this.mapdb.margin.y,
        });
        this.deltas.clear();
        this.pending.clear();
        this.log.info(`map ${m.name} (g_maps[${m.index}]), origin ` +
            `${m.origin.join(', ')}, ${m.blocks} blocks decoded in ${Date.now() - t0} ms`);
        this.sendMapSelect();
        this.emit('map', m);
        this.drainPrebuffer();
    }

    toLobby() {
        if (!this.map) return;
        this.log.info('left the map area -- lobby');
        this.map = null;
        this.world = null;
        this.deltas.clear();
        this.pending.clear();
        this.prebuf.length = 0;
        this.emit('lobby');
    }

    sendMapSelect() {
        if (!this.map) return;
        const m = this.map;
        this.link.send(S.MAP_SELECT, new Writer(17)
            .u8(m.index)
            .i32(m.originX).i32(m.originY).i32(m.originZ)
            .i32(this.getSelfEid())
            .done());
    }

    /**
     * A console that just attached has the pristine map from its own DOL, so
     * everything the game has changed since has to be replayed. `deltas` is
     * kept for exactly this: it is every cell that differs from the scan, and
     * re-sending one that has since been changed back is idempotent.
     */
    onConsoleAttached() {
        if (!this.map) return;
        this.sendMapSelect();
        this.pending = new Map(this.deltas);
        if (this.pending.size) {
            this.log.info(`replaying ${this.pending.size} block deltas to the console`);
        }
    }

    // ---- chunks ------------------------------------------------------------

    /** S21. `chunkData` starts with the block data for every section in
     *  `bitMap`; the light and biome arrays that follow are of no interest. */
    onMapChunk(pkt) {
        if (!pkt.bitMap) return;            // bitMap 0 with groundUp = unload
        this.column(pkt.x, pkt.z, pkt.bitMap, pkt.chunkData, 0);
    }

    /** S26. One buffer holding every column back to back, so each one's total
     *  size -- light and biomes included -- is needed to find the next. */
    onMapChunkBulk(pkt) {
        let off = 0;
        for (const meta of pkt.meta) {
            const n = popcount16(meta.bitMap);
            this.column(meta.x, meta.z, meta.bitMap, pkt.data, off);
            off += n * SECTION_BLOCK_BYTES
                 + n * SECTION_LIGHT_BYTES
                 + (pkt.skyLightSent ? n * SECTION_LIGHT_BYTES : 0)
                 + BIOME_BYTES;           // bulk is always a ground-up send
        }
    }

    column(cx, cz, bitMap, data, offset) {
        if (!this.map) {
            // Hub chunks are the overwhelming majority of what arrives before a
            // map is picked; keep only what could possibly matter.
            if (!this.couldBeAMap(cx, cz)) return;
            if (this.prebuf.length < PREBUFFER_COLUMNS) {
                const n = popcount16(bitMap);
                this.prebuf.push({
                    cx, cz, bitMap,
                    // Copy: `data` is the packet's buffer and will not outlive it.
                    data: Buffer.from(data.subarray(offset, offset + n * SECTION_BLOCK_BYTES)),
                });
            }
            return;
        }
        this.diffColumn(cx, cz, bitMap, data, offset);
    }

    drainPrebuffer() {
        if (!this.prebuf.length) return;
        const held = this.prebuf;
        this.prebuf = [];
        let used = 0;
        for (const c of held) {
            if (this.columnTouchesMap(c.cx, c.cz)) { this.diffColumn(c.cx, c.cz, c.bitMap, c.data, 0); used++; }
        }
        if (used) this.log.info(`applied ${used} chunk column(s) buffered before MAP_SELECT`);
    }

    couldBeAMap(cx, cz) {
        const x = cx * 16, z = cz * 16;
        for (const m of this.mapdb.maps) {
            if (x + 15 >= m.absMin[0] && x <= m.absMax[0] &&
                z + 15 >= m.absMin[2] && z <= m.absMax[2]) return true;
        }
        return false;
    }

    columnTouchesMap(cx, cz) {
        const m = this.map, x = cx * 16, z = cz * 16;
        return x + 15 >= m.absMin[0] && x <= m.absMax[0] &&
               z + 15 >= m.absMin[2] && z <= m.absMax[2];
    }

    /**
     * The join diff. Walks only the sections and rows that overlap the map's
     * padded box -- on a typical map that is six or seven of sixteen sections,
     * and the bounds tests are what keep a full join under a frame's work
     * rather than several seconds of it.
     */
    diffColumn(cx, cz, bitMap, data, offset) {
        const m = this.map;
        if (!this.columnTouchesMap(cx, cz)) { this.stats.outside++; return; }

        const [minX, minY, minZ] = m.absMin;
        const [maxX, maxY, maxZ] = m.absMax;
        let secOff = offset;

        for (let sy = 0; sy < 16; sy++) {
            if (!(bitMap & (1 << sy))) continue;
            const here = secOff;
            secOff += SECTION_BLOCK_BYTES;

            const y0 = sy * 16;
            if (y0 + 15 < minY || y0 > maxY) continue;
            if (here + SECTION_BLOCK_BYTES > data.length) {
                this.log.warn(`chunk ${cx},${cz} section ${sy}: short buffer, stopping`);
                return;
            }

            for (let ly = 0; ly < 16; ly++) {
                const ay = y0 + ly;
                if (ay < minY || ay > maxY) continue;
                for (let lz = 0; lz < 16; lz++) {
                    const az = cz * 16 + lz;
                    if (az < minZ || az > maxZ) continue;
                    let p = here + ((ly << 8) | (lz << 4)) * 2;
                    for (let lx = 0; lx < 16; lx++, p += 2) {
                        const ax = cx * 16 + lx;
                        if (ax < minX || ax > maxX) continue;
                        this.applyState(ax - m.originX, ay - m.originY, az - m.originZ,
                                        data[p] | (data[p + 1] << 8));
                        this.stats.diffed++;
                    }
                }
            }
        }
    }

    /** Is there a block at this local coordinate. Used by the projectile
     *  simulation in entities.js, against the live copy rather than the one the
     *  map shipped with -- players break and place all game. */
    solidAt(x, y, z) {
        return !!this.world && this.world.get(x, y, z) !== mworld.AIR;
    }

    // ---- block changes -----------------------------------------------------

    /** S23 block_change. */
    onBlockChange(pkt) {
        const l = pkt.location;
        this.setAbsolute(l.x, l.y, l.z, pkt.type);
    }

    /** S22 multi_block_change: records are packed relative to one chunk. */
    onMultiBlockChange(pkt) {
        const bx = pkt.chunkX * 16, bz = pkt.chunkZ * 16;
        for (const r of pkt.records) {
            this.setAbsolute(bx + ((r.horizontalPos >> 4) & 15), r.y,
                             bz + (r.horizontalPos & 15), r.blockId);
        }
    }

    setAbsolute(x, y, z, stateId) {
        const m = this.map;
        if (!m) return;
        if (!MapDB.contains(m, x, y, z)) { this.stats.outside++; return; }
        this.applyState(x - m.originX, y - m.originY, z - m.originZ, stateId);
    }

    /**
     * One block, in local coordinates. Everything funnels through here, so the
     * two rules that matter are in one place: an unmapped state is dropped
     * outright rather than substituted, and a block already believed correct
     * costs nothing.
     */
    applyState(x, y, z, stateId) {
        let want;
        if (this.bm.isAir(stateId)) {
            want = mworld.AIR;
        } else {
            want = this.bm.toGlobal(stateId);
            if (want < 0) { this.stats.unmapped++; return; }
        }

        const w = this.world;
        if (!w.contains(x, y, z)) { this.stats.outside++; return; }
        const cell = w.index(x, y, z);
        if (w.voxels[cell] === want) return;

        w.voxels[cell] = want;
        this.deltas.set(cell, want);
        this.pending.set(cell, want);
    }

    // ---- flush -------------------------------------------------------------

    /**
     * Drain the queue into BLOCK_SET frames, up to the per-flush budget. Called
     * once per 20 Hz tick: a join diff runs to tens of thousands of blocks and
     * the console re-meshes a bounded number of chunks per frame, so the flood
     * is paced here rather than handed to TCP all at once. Returns how many
     * blocks went out.
     */
    flush() {
        if (!this.pending.size || !this.link.attached) return 0;
        let budget = this.maxPerFlush;
        let total = 0;

        while (this.pending.size && budget > 0) {
            const n = Math.min(BLOCK_SET_MAX, budget, this.pending.size);
            const w = new Writer(n * 8);
            let k = 0;
            for (const [cell, id] of this.pending) {
                if (k >= n) break;
                const c = this.world.coords(cell, this._xyz);
                w.i16(c[0]).i16(c[1]).i16(c[2]).u16(id < 0 ? AIR : id);
                this.pending.delete(cell);
                k++;
            }
            this.link.send(S.BLOCK_SET, w.done());
            this.stats.frames++;
            budget -= k;
            total += k;
        }
        this.stats.sent += total;
        return total;
    }

    get backlog() { return this.pending.size; }

    report() {
        const s = this.stats;
        return `blocks ${s.sent} sent in ${s.frames} frames, ${s.diffed} compared, ` +
               `${s.unmapped} unmapped dropped, ${s.outside} outside the map` +
               (this.pending.size ? `, ${this.pending.size} queued` : '');
    }
}

function popcount16(v) {
    v = v - ((v >> 1) & 0x5555);
    v = (v & 0x3333) + ((v >> 2) & 0x3333);
    return (((v + (v >> 4)) & 0x0f0f) * 0x0101) >> 8 & 0x1f;
}

module.exports = { WorldTranslator, popcount16 };

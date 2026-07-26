'use strict';
/*
 * mworld.js -- the .mworld decoder, in JS.
 *
 * The proxy needs the console's own copy of the map so it can diff the server's
 * live chunks against it and send only what differs. Both other decoders --
 * tools/compress_worlds.py (which is also the encoder, and therefore the spec)
 * and world.c's World_Load -- are already round-trip verified, so this one only
 * has to agree with them. selftest.js checks that it does, against every
 * shipped map.
 *
 * Two layers, in this order on the way out:
 *
 *   1. LZSS -- 64 KiB window, flag-byte grouped, 16-bit distance, 8-bit length,
 *      minimum match 3.
 *   2. a structural stream -- the point cloud bucketed into vertical columns,
 *      each column a series of air-gap / solid-run segments over a per-map
 *      palette of global block ids.
 *
 * Coordinates that come out are *local*: spawn-relative, which is the same
 * space the console's World_GetBlock takes, and absolute minus the map's
 * teleportLocation. See tools/gen_mapdb.py.
 */

const fs = require('fs');
const path = require('path');

const MAGIC = 0x4d574c31;      // "MWL1"
const MIN_MATCH = 3;
const HEADER_BYTES = 38;

/** Local block space stores air as -1, matching the engine. */
const AIR = -1;

// ---- LZSS ------------------------------------------------------------------
/**
 * Matches can overlap the output cursor (distance < length is how a run of one
 * byte is encoded), so the copy is byte-at-a-time on purpose -- copyWithin
 * would read the pre-copy bytes and produce a different, wrong answer.
 */
function lzDecompress(comp, expected) {
    const out = Buffer.alloc(expected);
    let o = 0, p = 0;
    while (o < expected) {
        if (p >= comp.length) throw new Error('mworld: LZSS stream ran out');
        const flags = comp[p++];
        for (let bit = 0; bit < 8 && o < expected; bit++) {
            if (flags & (1 << bit)) {
                const dist = ((comp[p] << 8) | comp[p + 1]) + 1;
                const len = comp[p + 2] + MIN_MATCH;
                p += 3;
                let s = o - dist;
                if (s < 0) throw new Error('mworld: LZSS distance before the start');
                for (let k = 0; k < len; k++) out[o++] = out[s++];
            } else {
                out[o++] = comp[p++];
            }
        }
    }
    return out;
}

// ---- unsigned LEB128 --------------------------------------------------------
function uvarint(s, st) {
    let r = 0, sh = 0, b;
    do { b = s[st.p++]; r |= (b & 0x7f) << sh; sh += 7; } while (b & 0x80);
    return r >>> 0;
}

// ---- the map ----------------------------------------------------------------

/**
 * A decoded map. Blocks live in a dense Int16Array over the scan's bounding
 * box -- ~18 MB on the largest map, which is nothing on the PC and buys an O(1)
 * lookup for a join-time diff that touches millions of cells. The console
 * cannot afford this and does not do it; it keeps sorted column runs instead.
 *
 * The array is also the proxy's model of what the console currently shows: it
 * starts as the map the console loaded from its own DOL, and every delta the
 * proxy sends is written into it. So `get` is both "what does the map say" and
 * "what does the console believe", and the difference between that and the
 * server's chunk is exactly the set of blocks worth sending.
 */
class MWorld {
    constructor(header, voxels) {
        Object.assign(this, header);
        this.voxels = voxels;
        this.maxx = this.minx + this.dimx - 1;
        this.maxy = this.miny + this.dimy - 1;
        this.maxz = this.minz + this.dimz - 1;
    }

    /** Scan bounds as loaded, before any margin was added. */
    get scan() {
        return {
            minx: this.minx + this.marginXZ, maxx: this.maxx - this.marginXZ,
            miny: this.miny + this.marginY, maxy: this.maxy - this.marginY,
            minz: this.minz + this.marginXZ, maxz: this.maxz - this.marginXZ,
        };
    }

    /** True if a *local* coordinate is inside the scan's own bounding box. */
    contains(x, y, z) {
        return x >= this.minx && x <= this.maxx && y >= this.miny && y <= this.maxy
            && z >= this.minz && z <= this.maxz;
    }

    /** Linear cell index, the key the proxy's delta maps are built on. */
    index(x, y, z) {
        return ((x - this.minx) * this.dimy + (y - this.miny)) * this.dimz
            + (z - this.minz);
    }

    /** Inverse of `index`, into a reused [x,y,z] so a flush of a thousand
     *  blocks does not allocate a thousand arrays. */
    coords(i, out) {
        const z = i % this.dimz;
        const r = (i - z) / this.dimz;
        const y = r % this.dimy;
        out[0] = ((r - y) / this.dimy) + this.minx;
        out[1] = y + this.miny;
        out[2] = z + this.minz;
        return out;
    }

    _index(x, y, z) { return this.index(x, y, z); }

    /** Global block id at a local coordinate; AIR (-1) outside the scan too,
     *  which is what makes the build margin diff correctly with no special case. */
    get(x, y, z) {
        if (!this.contains(x, y, z)) return AIR;
        return this.voxels[this._index(x, y, z)];
    }

    set(x, y, z, id) {
        if (!this.contains(x, y, z)) return false;
        this.voxels[this._index(x, y, z)] = id;
        return true;
    }

    /** Non-air cells, for the selftest's cross-check against the header count. */
    countSolid() {
        let n = 0;
        for (let i = 0; i < this.voxels.length; i++) if (this.voxels[i] !== AIR) n++;
        return n;
    }
}

/**
 * `marginXZ` / `marginY` pad the stored box past the scan's own bounds, to
 * match WORLD_MARGIN_XZ / WORLD_MARGIN_Y in world.h. The console pads every map
 * with empty grid so a player can bridge out past the scan, and blocks placed
 * in that band are legal blocks -- without the same padding here they would
 * fall outside the tracking array and be re-sent on every chunk update.
 */
function decode(blob, { marginXZ = 0, marginY = 0 } = {}) {
    if (blob.length < HEADER_BYTES || blob.readUInt32BE(0) !== MAGIC) {
        throw new Error('mworld: bad magic');
    }
    const version = blob.readUInt8(4);
    if (version !== 1) throw new Error(`mworld: version ${version}, expected 1`);

    const idbytes = blob.readUInt8(5);
    const paletteLen = blob.readUInt16BE(6);
    const scanMinX = blob.readInt16BE(8);
    const scanMinY = blob.readInt16BE(10);
    const scanMinZ = blob.readInt16BE(12);
    const scanDimX = blob.readUInt16BE(14);
    const scanDimY = blob.readUInt16BE(16);
    const scanDimZ = blob.readUInt16BE(18);
    const header = {
        version, idbytes, marginXZ, marginY,
        minx: scanMinX - marginXZ, miny: scanMinY - marginY, minz: scanMinZ - marginXZ,
        dimx: scanDimX + 2 * marginXZ, dimy: scanDimY + 2 * marginY,
        dimz: scanDimZ + 2 * marginXZ,
        spawnx: blob.readInt16BE(20), spawny: blob.readInt16BE(22), spawnz: blob.readInt16BE(24),
        blocks: blob.readUInt32BE(26),
        ncol: blob.readUInt32BE(30),
        rawS: blob.readUInt32BE(34),
    };

    let off = HEADER_BYTES;
    const palette = new Uint16Array(paletteLen);
    for (let i = 0; i < paletteLen; i++, off += 2) palette[i] = blob.readUInt16BE(off);
    header.palette = palette;

    const S = lzDecompress(blob.subarray(off), header.rawS);

    const voxels = new Int16Array(header.dimx * header.dimy * header.dimz).fill(AIR);

    // Column index c = gx * scanDimZ + gz over the *scan's* grid -- the same
    // walk the C loader does, one pass, columns ascending, y ascending. The
    // margin only shifts where each cell lands in the padded array.
    const st = { p: 0 };
    let c = 0, total = 0;
    for (let i = 0; i < header.ncol; i++) {
        c += uvarint(S, st);
        const gx = ((c / scanDimZ) | 0) + marginXZ;
        const gz = (c % scanDimZ) + marginXZ;
        const nseg = uvarint(S, st);
        let y = 0;
        for (let s = 0; s < nseg; s++) {
            y += uvarint(S, st);
            const run = uvarint(S, st) + 1;
            let li;
            if (idbytes === 1) { li = S[st.p]; st.p += 1; }
            else { li = (S[st.p] << 8) | S[st.p + 1]; st.p += 2; }
            const id = palette[li];
            let base = (gx * header.dimy + y + marginY) * header.dimz + gz;
            for (let k = 0; k < run; k++, base += header.dimz) voxels[base] = id;
            y += run;
            total += run;
        }
    }
    if (total !== header.blocks) {
        throw new Error(`mworld: decoded ${total} blocks, header says ${header.blocks}`);
    }
    return new MWorld(header, voxels);
}

/** Load data/<name>.mworld -- the same bytes bin2s bakes into the DOL. */
function load(name, opts, dataDir = path.join(__dirname, '..', 'data')) {
    return decode(fs.readFileSync(path.join(dataDir, `${name}.mworld`)), opts);
}

module.exports = { decode, load, lzDecompress, MWorld, AIR };

// `node proxy/mworld.js hontori` -- a quick look at one map's header.
if (require.main === module) {
    for (const name of process.argv.slice(2)) {
        const w = load(name);
        console.log(`${name}: ${w.blocks} blocks, palette ${w.palette.length}, ` +
            `local ${w.minx}..${w.maxx} x ${w.miny}..${w.maxy} x ${w.minz}..${w.maxz}, ` +
            `${w.countSolid()} non-air cells`);
    }
}

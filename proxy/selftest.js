#!/usr/bin/env node
'use strict';
/*
 * selftest.js -- everything about the proxy that can be checked without a
 * Minecraft server, an account or a GameCube.
 *
 *   node proxy/selftest.js
 *
 * The interesting one is the chunk-diff round trip at the bottom. Three
 * coordinate spaces meet in world.js -- absolute, map-local, and a linear cell
 * index -- and a sign or an axis swapped between them produces a map that is
 * subtly, plausibly wrong rather than one that fails. So the test builds a
 * chunk section out of the reference map itself, feeds it back through the
 * diff, and asserts that nothing comes out; then changes exactly five blocks
 * and asserts that exactly those five do, at the coordinates they were changed
 * at. That closes the loop end to end without anything live.
 */

const fs = require('fs');
const path = require('path');

const mworld = require('./mworld');
const blockmapMod = require('./blockmap');
const mapdbMod = require('./mapdb');
const gclink = require('./gclink');
const chat = require('./chat');
const { WorldTranslator, popcount16 } = require('./world');
const { EntityTranslator, firstColourCode,
        LANDED_TICKS: LANDED } = require('./entities');
const { StateTranslator, windowSlotToEngine, mcYawToGc, gcYawToMc } = require('./state');

let pass = 0, fail = 0;
const ok = (cond, what) => {
    if (cond) { pass++; }
    else { fail++; console.error(`  FAIL  ${what}`); }
};
const eq = (a, b, what) => ok(a === b, `${what}: got ${a}, expected ${b}`);
const section = (name) => console.log(`\n${name}`);

// ---------------------------------------------------------------------------
section('framing');
{
    const { Writer, Reader, frame, MAX_PAYLOAD, S } = gclink;

    const payload = new Writer(8).i16(-1234).i16(7).i16(32000).u16(0xffff).done();
    eq(payload.length, 8, 'writer length');
    eq(payload.readInt16BE(0), -1234, 'i16 negative round trip');
    eq(payload.readUInt16BE(6), 0xffff, 'u16 air sentinel');

    // Split a stream at every byte boundary: the console's reassembly has to
    // cope with any split TCP chooses, so the test should too.
    const stream = Buffer.concat([
        frame(S.BLOCK_SET, payload), frame(S.PING, new Writer(6).u32(9).u16(3).done()),
        frame(S.CHAT, Buffer.from('\x0bhello', 'ascii')),
    ]);
    for (let cut = 1; cut < stream.length; cut++) {
        const r = new Reader();
        const got = [];
        const feed = (b) => r.push(b, (t, p) => got.push([t, p.length]));
        ok(feed(stream.subarray(0, cut)) && feed(stream.subarray(cut)),
           `split at ${cut} accepted`);
        eq(got.length, 3, `split at ${cut}: frame count`);
    }

    const r = new Reader();
    const bad = Buffer.alloc(3);
    bad.writeUInt16BE(MAX_PAYLOAD + 2, 0);
    ok(!r.push(bad, () => {}), 'oversized length rejected');

    let threw = false;
    try { frame(S.BLOCK_SET, Buffer.alloc(MAX_PAYLOAD + 1)); } catch (_) { threw = true; }
    ok(threw, 'oversized payload refused at the sender');

    eq(popcount16(0), 0, 'popcount 0');
    eq(popcount16(0xffff), 16, 'popcount all');
    eq(popcount16(0b1010100000010001), 5, 'popcount mixed');
}

// ---------------------------------------------------------------------------
section('capture round trip');
{
    const { Recorder, loadCapture, DIR_TO_CONSOLE, DIR_TO_SERVER, S, C } = gclink;
    const tmp = path.join(require('os').tmpdir(), `gclink-selftest-${process.pid}.gcr`);
    const rec = new Recorder(tmp);
    rec.record(DIR_TO_CONSOLE, S.MAP_SELECT, Buffer.from([1, 2, 3]));
    rec.record(DIR_TO_SERVER, C.MOVE, Buffer.alloc(34));
    rec.record(DIR_TO_CONSOLE, S.CHAT, Buffer.from('\x0chi', 'ascii'));
    rec.close();

    const back = loadCapture(tmp);
    eq(back.length, 3, 'capture frame count');
    eq(back[0].type, S.MAP_SELECT, 'capture type');
    eq(back[1].payload.length, 34, 'capture payload length');
    eq(back[2].payload.toString('ascii', 1), 'hi', 'capture payload bytes');
    fs.unlinkSync(tmp);
}

// ---------------------------------------------------------------------------
section('blockmap');
const bm = blockmapMod.load();
{
    const ids = fs.readFileSync(path.join(__dirname, '..', 'data', 'blockids.txt'), 'utf8')
        .split(/\r?\n/).filter((s) => s.trim().length);
    eq(ids.length, bm.blockCount, 'blockids.txt lines vs blockmap blockCount');

    // Every global id the palette defines has to be reachable from some 1.8
    // state, or a map would contain blocks the server can never ask for.
    const reachable = new Set();
    for (let s = 0; s < 65536; s++) if (bm.states[s] >= 0) reachable.add(bm.states[s]);
    const unreachable = [];
    for (let g = 0; g < bm.blockCount; g++) if (!reachable.has(g) && g !== bm.sentinel) unreachable.push(g);
    eq(unreachable.length, 0, `global ids no state maps to (${unreachable.slice(0, 8)})`);

    ok(bm.isAir(0) && bm.isAir(15) && !bm.isAir(16), 'air is states 0..15');
    eq(bm.toGlobal(1 << 4 | 0), bm.states[16], 'stone maps');
    eq(bm.toGlobal(0xfff << 4), -1, 'an unmapped state is -1, not a placeholder');
}

// ---------------------------------------------------------------------------
section('mapdb');
const mapdb = mapdbMod.load();
{
    eq(mapdb.size, 32, 'map count (31 game maps plus the hub)');
    const seen = new Set();
    for (const m of mapdb.maps) {
        ok(!seen.has(m.index), `${m.name}: g_maps index ${m.index} is unique`);
        seen.add(m.index);
        // The origin is the scan's own zero, so it must be inside the map.
        ok(mapdbMod.MapDB.contains(m, m.originX, m.originY, m.originZ),
           `${m.name}: its own origin falls inside its box`);
    }

    const hontori = mapdb.get('hontori');
    eq(mapdb.findByPosition(3850, 80, -2149).name, 'hontori', 'origin identifies its map');
    // The hub is streamed like a map -- same scan, same .mworld, same
    // MAP_SELECT -- and flagged so state.js keeps it in the LOBBY game state
    // rather than treating standing in it as being in a game.
    const hub = mapdb.findByPosition(0, 101, 0);
    eq(hub && hub.name, 'spawn', 'the hub resolves to the spawn scan');
    ok(hub.hub === true, 'and is flagged as the hub');
    eq(mapdb.maps.filter((m) => m.hub).length, 1, 'exactly one hub');
    // The avoidance minigames sit at +/-500 and +/-1000, far outside every box.
    eq(mapdb.findByPosition(-500, 42, -1000), null, 'a minigame arena is no map');
    // Every map's origin must resolve to itself and to nothing else.
    for (const m of mapdb.maps) {
        eq(mapdb.findByPosition(m.originX, m.originY, m.originZ).name, m.name,
           `${m.name}: origin is unambiguous`);
    }
    // The *scan* has to fit the build height (gen_mapdb.py asserts this for
    // every map). The padded acceptance box may hang below 0 or above 255,
    // which is harmless: the server can never send a block there, so that part
    // of the console's build margin simply stays empty.
    const scanLo = hontori.origin[1] + hontori.min[1];
    const scanHi = scanLo + hontori.dim[1] - 1;
    ok(scanLo >= 0 && scanHi <= 255, `hontori's scan fits the build height (${scanLo}..${scanHi})`);
}

// ---------------------------------------------------------------------------
section('mworld decode (every shipped map)');
const worlds = {};
{
    const dataDir = path.join(__dirname, '..', 'data');
    const names = fs.readdirSync(dataDir).filter((f) => f.endsWith('.mworld'))
        .map((f) => f.slice(0, -'.mworld'.length));
    eq(names.length, 33, 'map files present');

    let totalBlocks = 0;
    for (const name of names) {
        const w = mworld.load(name);
        // decode() already asserts the walked run total against the header; this
        // checks the *stored* array, which is what the diff will actually read.
        eq(w.countSolid(), w.blocks, `${name}: non-air cells vs header block count`);
        const palette = new Set(w.palette);
        let strays = 0;
        for (let i = 0; i < w.voxels.length; i++) {
            const v = w.voxels[i];
            if (v !== mworld.AIR && !palette.has(v)) strays++;
        }
        eq(strays, 0, `${name}: every stored id is in the map palette`);
        let outOfRange = 0;
        for (const g of w.palette) if (g >= bm.blockCount) outOfRange++;
        eq(outOfRange, 0, `${name}: palette inside the global id range`);
        totalBlocks += w.blocks;
        if (mapdb.get(name)) worlds[name] = w;
    }
    console.log(`  ${names.length} maps, ${totalBlocks.toLocaleString()} blocks decoded`);
}

// ---------------------------------------------------------------------------
section('mworld margin padding');
{
    const plain = mworld.load('hontori');
    const padded = mworld.load('hontori', { marginXZ: 8, marginY: 16 });
    eq(padded.countSolid(), plain.countSolid(), 'padding adds no blocks');
    let bad = 0;
    for (let x = plain.minx; x <= plain.maxx; x += 7) {
        for (let y = plain.miny; y <= plain.maxy; y += 5) {
            for (let z = plain.minz; z <= plain.maxz; z += 7) {
                if (plain.get(x, y, z) !== padded.get(x, y, z)) bad++;
            }
        }
    }
    eq(bad, 0, 'padding leaves every coordinate where it was');
    eq(padded.get(plain.minx - 4, plain.miny, plain.minz), mworld.AIR, 'margin band is air');
    ok(padded.contains(plain.minx - 8, plain.miny - 16, plain.minz - 8), 'margin is addressable');

    const idx = padded.index(10, -20, 30);
    const c = padded.coords(idx, [0, 0, 0]);
    ok(c[0] === 10 && c[1] === -20 && c[2] === 30, 'cell index round trips');
}

// ---------------------------------------------------------------------------
section('chat');
{
    eq(chat.parse('{"text":"hello"}').text, 'hello', 'plain component');
    eq(chat.parse('{"text":"","extra":[{"text":"a"},{"text":"b"}]}').text, 'ab', 'extra concatenation');
    eq(chat.parse('{"text":"x","color":"red"}').colour, 12, 'named colour to code');
    eq(chat.parse('{"text":"§cred §ayellowish"}').colour, 12, 'first inline code wins');
    eq(chat.parse('{"text":"§cred"}').text, 'red', 'section signs stripped from the text');
    eq(chat.parse('{"translate":"chat.type.text","with":[{"text":"bob"},{"text":"hi"}]}').text,
       '<bob> hi', 'translation key');
    eq(chat.parse('{"translate":"nope.unknown","with":[{"text":"bob"}]}').text,
       'bob', 'unknown key falls back to its arguments');
    eq(chat.parse('{"text":"café — naïve • ♥"}').text,
       'cafe - naive * <3', 'fold to the console font range');
    eq(chat.parse('{"text":"日本"}').text, '', 'undrawable characters dropped');
    eq(chat.parse('{"text":"a\\nb"}').lines.length, 2, 'newlines split into lines');
    eq(chat.parse('{"text":"a\\nb"}').text, 'a b', 'and the flat form joins them');
    eq(chat.parse('not json at all').text, 'not json at all', 'a bare string still parses');

    for (const ch of chat.fold('the quick brown fox 0123456789 !@#$%^&*()')) {
        const c = ch.codePointAt(0);
        ok(c >= 32 && c <= 126, `folded char ${c} is drawable`);
    }
    eq(firstColourCode('§bTeam'), 11, 'team prefix colour');
    eq(firstColourCode('no codes'), 0xff, 'no team colour');
}

// ---------------------------------------------------------------------------
section('conventions');
{
    // MC yaw 0 faces +Z and positive pitch looks down; the engine's yaw 0 faces
    // -Z and positive pitch looks up. A sign error here reads as "moved
    // wrongly" on the server, not as anything you can see, so both directions
    // are checked rather than just one.
    for (let y = -350; y <= 350; y += 7) {
        const there = mcYawToGc(y), back = gcYawToMc(there);
        const norm = ((y % 360) + 360) % 360;
        ok(Math.abs(back - norm) < 1e-6 || Math.abs(Math.abs(back - norm) - 360) < 1e-6,
           `yaw ${y} round trips (${there} -> ${back}, expected ${norm})`);
    }

    // The four compass points, checked against the direction each convention
    // actually faces rather than against a formula -- this is a reflection and
    // not an offset, and the two only differ away from 0 and 180, which is
    // exactly where a plausible-looking `yaw - 180` would pass.
    //   MC:     0 = +Z south, 90 = -X west, 180 = -Z north, 270 = +X east
    //   engine: forward is (-sin y, -cos y), so 0 = -Z, 90 = -X, 180 = +Z
    const compass = [[0, 180], [90, 90], [180, 0], [270, -90]];
    for (const [mc, gc] of compass) {
        const got = mcYawToGc(mc);
        ok(Math.abs(got - gc) < 1e-6,
           `MC yaw ${mc} is GC yaw ${gc} (got ${got})`);
        const dir = (a) => [-Math.sin(a * Math.PI / 180), -Math.cos(a * Math.PI / 180)];
        const mcDir = [-Math.sin(mc * Math.PI / 180), Math.cos(mc * Math.PI / 180)];
        const gcDir = dir(got);
        ok(Math.abs(mcDir[0] - gcDir[0]) < 1e-9 && Math.abs(mcDir[1] - gcDir[1]) < 1e-9,
           `  ...and both face the same way (${mcDir} vs ${gcDir})`);
    }
    eq(gcYawToMc(0), 180, 'and back');

    eq(windowSlotToEngine(36), 0, 'window hotbar slot 0');
    eq(windowSlotToEngine(44), 8, 'window hotbar slot 8');
    eq(windowSlotToEngine(9), 9, 'window storage start');
    eq(windowSlotToEngine(35), 35, 'window storage end');
    eq(windowSlotToEngine(5), 39, 'helmet -> armor[3]');
    eq(windowSlotToEngine(8), 36, 'boots -> armor[0]');
    eq(windowSlotToEngine(0), -1, 'crafting output has no console slot');
    const engine = new Set();
    for (let s = 5; s <= 44; s++) engine.add(windowSlotToEngine(s));
    eq(engine.size, 40, 'the 40 console slots are covered exactly once');
}

// ---------------------------------------------------------------------------
section('chunk diff round trip');
{
    const { S, AIR } = gclink;
    const MAP = 'hontori';
    const map = mapdb.get(MAP);

    // The inverse of blockmap.states: an engine id back to some 1.8 state that
    // produces it. Only the test needs this -- the proxy is one-way.
    const stateOf = new Map();
    for (let s = 16; s < 65536; s++) {
        const g = bm.states[s];
        if (g >= 0 && !stateOf.has(g)) stateOf.set(g, s);
    }

    const sent = [];
    const link = { attached: true, send: (t, p) => sent.push([t, Buffer.from(p)]) };
    const quiet = { info() {}, warn() {}, error() {}, debug() {} };
    const w = new WorldTranslator({
        mapdb, blockmap: bm, link, log: quiet,
        config: { mapMatchRadius: 250, maxBlocksPerFrame: 1 << 20 },
        getSelfEid: () => 42,
    });

    w.onPosition(map.originX, map.originY, map.originZ);
    eq(w.map && w.map.name, MAP, 'position selects the map');
    eq(sent.length, 1, 'MAP_SELECT sent');
    eq(sent[0][0], S.MAP_SELECT, 'and it is MAP_SELECT');
    eq(sent[0][1].readUInt8(0), map.index, 'MAP_SELECT carries the g_maps index');
    eq(sent[0][1].readInt32BE(1), map.originX, 'MAP_SELECT carries the origin');
    eq(sent[0][1].readInt32BE(13), 42, 'MAP_SELECT carries selfEid');
    sent.length = 0;

    // Build the chunk column the server would send for a column over the map
    // centre, straight out of the reference map.
    const ref = w.world;
    const cx = map.originX >> 4, cz = map.originZ >> 4;
    const sections = [];
    let bitMap = 0;
    for (let sy = 0; sy < 16; sy++) {
        const y0 = sy * 16;
        if (y0 + 15 < map.absMin[1] || y0 > map.absMax[1]) continue;
        bitMap |= 1 << sy;
        const buf = Buffer.alloc(8192);
        for (let ly = 0; ly < 16; ly++) {
            for (let lz = 0; lz < 16; lz++) {
                for (let lx = 0; lx < 16; lx++) {
                    const g = ref.get(cx * 16 + lx - map.originX, y0 + ly - map.originY,
                                      cz * 16 + lz - map.originZ);
                    const st = g === AIRLESS(g) ? 0 : (stateOf.get(g) || 0);
                    buf.writeUInt16LE(st, (((ly << 8) | (lz << 4)) | lx) * 2);
                }
            }
        }
        sections.push({ sy, buf });
    }
    ok(sections.length > 0, 'the test column overlaps the map');
    const chunkData = Buffer.concat(sections.map((s) => s.buf));

    w.onMapChunk({ x: cx, z: cz, groundUp: true, bitMap, chunkData });
    eq(w.backlog, 0, 'a chunk identical to the reference produces no deltas');
    eq(w.flush(), 0, 'and nothing is sent');

    // Now change five blocks and confirm exactly those come back out.
    const edits = [];
    const stone = 1 << 4;                       // STONE:0
    const stoneG = bm.toGlobal(stone);
    let placed = 0;
    for (const s of sections) {
        for (let i = 0; i < 4096 && placed < 5; i += 811) {
            const ly = i >> 8, lz = (i >> 4) & 15, lx = i & 15;
            const ax = cx * 16 + lx, ay = s.sy * 16 + ly, az = cz * 16 + lz;
            if (ax < map.absMin[0] || ax > map.absMax[0]) continue;
            if (ay < map.absMin[1] || ay > map.absMax[1]) continue;
            if (az < map.absMin[2] || az > map.absMax[2]) continue;
            const lxl = ax - map.originX, lyl = ay - map.originY, lzl = az - map.originZ;
            if (ref.get(lxl, lyl, lzl) === stoneG) continue;
            s.buf.writeUInt16LE(stone, i * 2);
            edits.push([lxl, lyl, lzl]);
            placed++;
        }
        if (placed >= 5) break;
    }
    eq(edits.length, 5, 'five test edits placed');

    w.onMapChunk({ x: cx, z: cz, groundUp: true, bitMap,
                   chunkData: Buffer.concat(sections.map((s) => s.buf)) });
    eq(w.backlog, 5, 'exactly the changed blocks are queued');
    eq(w.flush(), 5, 'and flushed');
    eq(sent.length, 1, 'in one BLOCK_SET frame');
    eq(sent[0][0], S.BLOCK_SET, 'frame type');
    eq(sent[0][1].length, 5 * 8, 'frame length');

    const got = new Set();
    for (let i = 0; i < 5; i++) {
        const p = sent[0][1], o = i * 8;
        got.add(`${p.readInt16BE(o)},${p.readInt16BE(o + 2)},${p.readInt16BE(o + 4)}`);
        eq(p.readUInt16BE(o + 6), stoneG, `delta ${i} carries the right global id`);
    }
    for (const [x, y, z] of edits) ok(got.has(`${x},${y},${z}`), `delta at ${x},${y},${z}`);
    sent.length = 0;

    // A block change outside the map is not ours; one inside is. The map's own
    // origin is asserted to be inside its box above, so it is the safe probe.
    w.onBlockChange({ location: { x: 0, y: 64, z: 0 }, type: stone });
    eq(w.backlog, 0, 'a block change outside the map is dropped');
    // Also outside: hontori's scan tops out 20 blocks above its origin, and the
    // build margin only reaches 16 further.
    w.onBlockChange({ location: { x: map.originX, y: map.originY + 40, z: map.originZ },
                      type: stone });
    eq(w.backlog, 0, 'a block change above the map is dropped');

    const probe = { x: map.originX, y: map.originY, z: map.originZ };
    const wood = 5 << 4;                        // WOOD:0, an unambiguous change
    const newState = ref.get(0, 0, 0) === stoneG ? wood : stone;
    w.onBlockChange({ location: probe, type: newState });
    eq(w.backlog, 1, 'a block change inside the map is taken');

    // Setting it to what the console already believes is not a change.
    const before = w.backlog;
    w.onBlockChange({ location: probe, type: newState });
    eq(w.backlog, before, 'a repeat of the same block costs nothing');

    // An unmapped state is dropped outright rather than substituted -- a
    // placeholder blinking 20 times a second would be worse than nothing.
    const unmapped = 0xfff << 4;
    ok(bm.toGlobal(unmapped) < 0, 'the test state really is unmapped');
    w.onBlockChange({ location: { x: probe.x + 1, y: probe.y, z: probe.z },
                      type: unmapped });
    eq(w.backlog, before, 'an unmapped state is dropped');

    // Re-attaching a console replays every delta accumulated so far.
    w.flush(); sent.length = 0;
    w.onConsoleAttached();
    eq(w.backlog, w.deltas.size, 're-attach re-queues every delta');
    ok(w.deltas.size >= 6, 'and there are deltas to re-queue');

    // Frame splitting at the payload cap.
    const many = new WorldTranslator({
        mapdb, blockmap: bm, link, log: quiet,
        config: { mapMatchRadius: 250, maxBlocksPerFrame: 1 << 20 },
        getSelfEid: () => 1,
    });
    many.onPosition(map.originX, map.originY, map.originZ);
    sent.length = 0;
    let n = 0;
    for (let x = 0; x < 60 && n < 2500; x++) {
        for (let z = 0; z < 60 && n < 2500; z++) {
            many.applyState(x, map.localMax[1] - 1, z, stone);
            n = many.backlog;
        }
    }
    eq(many.backlog, 2500, '2500 blocks queued');
    eq(many.flush(), 2500, 'all flushed in one call');
    eq(sent.length, 3, 'split into 1024 + 1024 + 452');
    eq(sent[0][1].length, 1024 * 8, 'first frame is a full payload');
    eq(sent[2][1].length, 452 * 8, 'last frame is the remainder');
}

function AIRLESS(g) { return g < 0 ? g : NaN; }   // g === AIRLESS(g) iff g is air

// ---------------------------------------------------------------------------
section('entity translation');
{
    const { S, ENT, EFLAG, ANIM, AIR, POS_SCALE } = gclink;
    const map = mapdb.get('hontori');

    const make = (cap = 128) => {
        const sent = [];
        const link = { attached: true, send: (t, p) => sent.push([t, Buffer.from(p)]) };
        const e = new EntityTranslator({
            link, blockmap: bm, log: { info() {}, warn() {}, error() {} },
            config: { cap, updateHz: 20, minMoveDelta: 0.03 },
        });
        e.setSelfEid(1);
        e.setMap(map);
        sent.length = 0;
        return { e, sent, of: (t) => sent.filter((s) => s[0] === t) };
    };

    // -- the type filter, which is what keeps a busy fight from thrashing.
    {
        const { e, sent } = make();
        e.onObjectSpawn({ entityId: 10, type: 60, x: 0, y: 0, z: 0, yaw: 0, pitch: 0 });
        e.onObjectSpawn({ entityId: 11, type: 61, x: 0, y: 0, z: 0, yaw: 0, pitch: 0 });
        e.onObjectSpawn({ entityId: 12, type: 50, x: 0, y: 0, z: 0, yaw: 0, pitch: 0 }); // TNT
        e.onObjectSpawn({ entityId: 13, type: 66, x: 0, y: 0, z: 0, yaw: 0, pitch: 0 }); // wither skull
        e.onMobSpawn({ entityId: 20, type: 63, x: 0, y: 0, z: 0, yaw: 0, pitch: 0 });    // dragon
        e.onMobSpawn({ entityId: 21, type: 54, x: 0, y: 0, z: 0, yaw: 0, pitch: 0 });    // zombie
        e.onMobSpawn({ entityId: 22, type: 30, x: 0, y: 0, z: 0, yaw: 0, pitch: 0 });    // armor stand
        eq(e.count, 3, 'arrow, snowball and dragon kept; TNT, skull, zombie and armor stand dropped');
        eq(e.stats.dropped, 4, 'four filtered out');

        e.onPlayerSpawn({ entityId: 1, playerUUID: 'u', x: 0, y: 0, z: 0, yaw: 0, pitch: 0,
                          currentItem: 0, metadata: [] });
        eq(e.count, 3, 'selfEid never becomes an entity');
        sent.length = 0;
    }

    // -- ENTITY_ADD carries local coordinates, the name and the team colour.
    {
        const { e, sent, of } = make();
        e.onPlayerInfo({ action: 'add_player', data: [{ UUID: 'uuid-a', name: 'pvpUS' }] });
        e.onTeam({ team: 'msw3', mode: 0, prefix: '§bTeam 3 ', players: ['pvpUS'] });
        e.onPlayerSpawn({
            entityId: 7, playerUUID: 'uuid-a',
            x: (map.originX + 5) * POS_SCALE, y: (map.originY + 6) * POS_SCALE,
            z: (map.originZ + 7) * POS_SCALE,
            yaw: 64, pitch: 0, currentItem: 276, metadata: [{ key: 0, type: 0, value: 0x02 }],
        });
        e.flush(map.originX, map.originY, map.originZ);

        const add = of(S.ENTITY_ADD);
        eq(add.length, 1, 'one ENTITY_ADD');
        const p = add[0][1];
        eq(p.readInt32BE(0), 7, 'eid');
        eq(p.readUInt8(4), ENT.PLAYER, 'type');
        eq(p.readUInt8(5), EFLAG.SNEAKING, 'sneak flag from metadata');
        eq(p.readInt32BE(6) / POS_SCALE, 5, 'x is map-local');
        eq(p.readInt32BE(10) / POS_SCALE, 6, 'y is map-local');
        eq(p.readInt32BE(14) / POS_SCALE, 7, 'z is map-local');
        eq(p.readUInt8(18), 64, 'yaw byte passes through');
        eq(p.readUInt16BE(20), bm.toItem(276, 0), 'held diamond sword');
        eq(p.readUInt8(22), 11, 'aqua team colour');
        eq(p.readUInt8(23), 5, 'name length');
        eq(p.toString('ascii', 24, 29), 'pvpUS', 'name');
        eq(p.length, 24 + 5, 'ENTITY_ADD is 24 bytes plus the name');
    }

    // -- a dropped item's contents arrive as metadata, not in the spawn.
    {
        const { e, of } = make();
        e.onObjectSpawn({ entityId: 8, type: 2, x: 0, y: 0, z: 0, yaw: 0, pitch: 0 });
        e.flush(map.originX, map.originY, map.originZ);
        eq(of(S.ENTITY_ADD)[0][1].readUInt16BE(20), AIR,
           'S0E alone says only that a drop exists');

        e.onMetadata({ entityId: 8, metadata: [
            { key: 10, type: 5, value: { blockId: 276, itemCount: 1, itemDamage: 0 } },
        ] });
        e.flush(map.originX, map.originY, map.originZ);
        const eq0 = of(S.ENTITY_EQUIP);
        eq(eq0.length, 1, 'index 10 turns into an ENTITY_EQUIP');
        eq(eq0[0][1].readInt32BE(0), 8, 'for that drop');
        eq(eq0[0][1].readUInt8(4), 0, 'in the held slot');
        eq(eq0[0][1].readUInt16BE(5), bm.toItem(276, 0), 'carrying a diamond sword');
    }

    // -- relative moves accumulate; the console still gets absolutes.
    {
        const { e, of } = make();
        e.onObjectSpawn({ entityId: 30, type: 60,
                          x: map.originX * POS_SCALE, y: map.originY * POS_SCALE,
                          z: map.originZ * POS_SCALE, yaw: 0, pitch: 0 });
        e.flush(map.originX, map.originY, map.originZ);
        for (let i = 0; i < 4; i++) {
            e.onRelMove({ entityId: 30, dX: 32, dY: 0, dZ: -16 }, false);
        }
        e.flush(map.originX, map.originY, map.originZ);
        const mv = of(S.ENTITY_MOVE);
        eq(mv.length, 1, 'one batched ENTITY_MOVE');
        eq(mv[0][1].length, 18, 'one 18-byte record');
        eq(mv[0][1].readInt32BE(0), 30, 'eid');
        eq(mv[0][1].readInt32BE(4) / POS_SCALE, 4, 'four +1.0 steps accumulated');
        eq(mv[0][1].readInt32BE(12) / POS_SCALE, -2, 'and four -0.5 steps');

        // A move too small to see is not worth a packet.
        const before = of(S.ENTITY_MOVE).length;
        e.onRelMove({ entityId: 30, dX: 0, dY: 0, dZ: 0 }, false);
        e.flush(map.originX, map.originY, map.originZ);
        eq(of(S.ENTITY_MOVE).length, before, 'a sub-pixel delta sends nothing');

        e.onTeleport({ entityId: 30, x: 0, y: 0, z: 0, yaw: 0, pitch: 0 });
        e.flush(map.originX, map.originY, map.originZ);
        ok(of(S.ENTITY_MOVE).length > before, 'a teleport overrides the accumulation');
    }

    // -- destroy, equip and animation.
    {
        const { e, of } = make();
        e.onObjectSpawn({ entityId: 40, type: 60, x: 0, y: 0, z: 0, yaw: 0, pitch: 0 });
        e.flush(0, 0, 0);
        e.onDestroy({ entityIds: [40, 999] });
        const rm = of(S.ENTITY_REMOVE);
        eq(rm.length, 1, 'ENTITY_REMOVE sent');
        eq(rm[0][1].length, 4, 'only the entity we knew about');
        eq(rm[0][1].readInt32BE(0), 40, 'the right one');
        eq(e.count, 0, 'and it is gone from the table');

        e.onPlayerSpawn({ entityId: 41, playerUUID: 'x', x: 0, y: 0, z: 0, yaw: 0, pitch: 0,
                          currentItem: 0, metadata: [] });
        e.flush(0, 0, 0);
        e.onEquipment({ entityId: 41, slot: 0, item: { blockId: 276, itemCount: 1, itemDamage: 0 } });
        e.onAnimation({ entityId: 41, animation: 0 });
        e.onStatus({ entityId: 41, entityStatus: 2 });
        e.onAnimation({ entityId: 41, animation: 2 });   // leave bed: not ours
        e.flush(0, 0, 0);
        eq(of(S.ENTITY_EQUIP).length, 1, 'one ENTITY_EQUIP');
        eq(of(S.ENTITY_EQUIP)[0][1].readUInt16BE(5), bm.toItem(276, 0), 'equipped sword');
        const anims = of(S.ENTITY_ANIM).map((a) => a[1].readUInt8(4));
        eq(anims.length, 2, 'swing and hurt only');
        ok(anims.includes(ANIM.SWING) && anims.includes(ANIM.HURT), 'the right two');
    }

    // -- the cap, and what it chooses to keep.
    {
        const { e, of } = make(16);
        // 40 arrows near the player, 4 players far away.
        for (let i = 0; i < 40; i++) {
            e.onObjectSpawn({ entityId: 100 + i, type: 60,
                              x: i * POS_SCALE, y: 0, z: 0, yaw: 0, pitch: 0 });
        }
        for (let i = 0; i < 4; i++) {
            e.onPlayerSpawn({ entityId: 200 + i, playerUUID: 'p' + i,
                              x: 500 * POS_SCALE, y: 0, z: 0, yaw: 0, pitch: 0,
                              currentItem: 0, metadata: [] });
        }
        eq(e.count, 44, 'all tracked before the flush');
        e.flush(0, 0, 0);
        eq(e.count, 16, 'the cap is enforced');
        let players = 0;
        for (const ent of e.entities.values()) if (ent.type === ENT.PLAYER) players++;
        eq(players, 4, 'every distant player survives a cloud of near arrows');
        ok(e.stats.evicted === 28, 'and the rest were evicted');
    }

    // -- a re-attached console has to be told everything again.
    {
        const { e, of, sent } = make();
        e.onPlayerSpawn({ entityId: 50, playerUUID: 'y', x: 0, y: 0, z: 0, yaw: 0, pitch: 0,
                          currentItem: 0, metadata: [] });
        e.flush(0, 0, 0);
        eq(of(S.ENTITY_ADD).length, 1, 'introduced once');
        e.flush(0, 0, 0);
        eq(of(S.ENTITY_ADD).length, 1, 'and not again');
        e.onConsoleAttached();
        e.flush(0, 0, 0);
        eq(of(S.ENTITY_ADD).length, 2, 'but re-introduced to a fresh console');
    }

    // -- an arrow that sticks in a block comes off the console; one still in
    //    flight does not, however slowly it is travelling.
    {
        const { e, of } = make();
        const at = (n) => (map.originX + n) * POS_SCALE;
        e.onObjectSpawn({ entityId: 60, type: 60, x: at(0), y: at(0), z: at(0),
                          yaw: 0, pitch: 0 });
        e.onObjectSpawn({ entityId: 61, type: 60, x: at(0), y: at(0), z: at(0),
                          yaw: 0, pitch: 0 });
        e.flush(map.originX, map.originY, map.originZ);
        eq(of(S.ENTITY_ADD).length, 2, 'both arrows introduced');

        // 60 holds still; 61 keeps moving a hair under a block per tick.
        for (let i = 1; i <= LANDED + 4; i++) {
            e.onRelMove({ entityId: 61, dX: 24, dY: 0, dZ: 0 }, false);
            e.flush(map.originX, map.originY, map.originZ);
        }

        const removed = of(S.ENTITY_REMOVE);
        eq(removed.length, 1, 'exactly one removal sent');
        eq(removed[0][1].readInt32BE(0), 60, 'and it is the arrow that stopped');
        eq(e.stats.landed, 1, 'counted as landed');
        eq(e.count, 2, 'but still tracked here, so entity_destroy still matches');

        // A console attaching mid-game must not be handed the whole quiver
        // already stuck in the map.
        const adds = of(S.ENTITY_ADD).length;
        e.onConsoleAttached();
        e.flush(map.originX, map.originY, map.originZ);
        eq(of(S.ENTITY_ADD).length, adds + 1, 'a fresh console gets the flying arrow only');

        // The console already forgot it, so its destroy must not be forwarded.
        const before = of(S.ENTITY_REMOVE).length;
        e.onDestroy({ entityIds: [60] });
        eq(of(S.ENTITY_REMOVE).length, before, 'no second removal for a landed arrow');
        eq(e.count, 1, 'and it is gone from the table');
    }
}

// ---------------------------------------------------------------------------
section('player state translation');
{
    const { S, GAME, AIR } = gclink;
    const map = mapdb.get('hontori');

    const make = () => {
        const sent = [];
        const written = [];
        const link = { attached: true, send: (t, p) => sent.push([t, Buffer.from(p)]) };
        const s = new StateTranslator({
            link, blockmap: bm, config: { log: false, sendBorderSize: false },
            log: { info() {}, warn() {}, error() {}, chat() {} },
        });
        s.attach({ write: (name, pkt) => written.push([name, pkt]) });
        return { s, sent, written, of: (t) => sent.filter((x) => x[0] === t) };
    };

    // -- S08 is the one packet whose reply is not optional.
    {
        const { s, of, written } = make();
        s.setMap(map);
        s.onPosition({ x: map.originX + 1, y: map.originY + 2, z: map.originZ + 3,
                       yaw: 180, pitch: 10, flags: 0 });
        eq(written.length, 1, 'exactly one packet written back');
        eq(written[0][0], 'position_look', 'and it is C06');
        eq(written[0][1].x, map.originX + 1, 'C06 echoes the server position, not ours');
        eq(s.teleportEpoch, 1, 'epoch bumped');

        const tp = of(S.TELEPORT);
        eq(tp.length, 1, 'TELEPORT sent to the console');
        eq(tp[0][1].length, 33, 'TELEPORT is 33 bytes');
        eq(tp[0][1].readDoubleBE(0), 1, 'x is map-local');
        eq(tp[0][1].readDoubleBE(8), 2, 'y is map-local');
        eq(tp[0][1].readFloatBE(24), 0, 'MC yaw 180 becomes GC yaw 0');
        eq(tp[0][1].readFloatBE(28), -10, 'pitch is negated');
        eq(tp[0][1].readUInt8(32), 1, 'epoch travels with it');

        // Relative flags: a set bit means "add to what you had".
        s.onPosition({ x: 10, y: 0, z: 0, yaw: 0, pitch: 0, flags: 0x01 });
        eq(s.x, map.originX + 11, 'a relative x is added to the current one');
        eq(s.y, 0, 'while a clear bit replaces outright');
        eq(s.teleportEpoch, 2, 'each teleport bumps the epoch');
    }

    // -- health, xp, game mode, held slot.
    {
        const { s, of } = make();
        s.onHealth({ health: 13.5, food: 20, foodSaturation: 20 });
        eq(of(S.HEALTH)[0][1].readFloatBE(0), 13.5, 'health passes through, hunger does not');
        s.onExperience({ experienceBar: 0.5, level: 1420, totalExperience: 99 });
        const xp = of(S.XP)[0][1];
        eq(xp.readFloatBE(0), 0.5, 'xp bar');
        eq(xp.readInt16BE(4), 1420, 'level is the ranked elo');
        s.onGameStateChange({ reason: 3, gameMode: 3 });
        eq(of(S.GAME_MODE).pop()[1].readUInt8(0), 3, 'spectator');
        s.onGameStateChange({ reason: 7, gameMode: 5 });   // rain strength: not ours
        eq(of(S.GAME_MODE).length, 1, 'other game-state reasons are ignored');
        s.onHeldItemSlot({ slot: 5 });
        eq(of(S.HELD_SLOT)[0][1].readUInt8(0), 5, 'held slot is absolute');
        s.onSelfVelocity({ velocity: { x: 8000, y: 4000, z: -8000 } });
        const v = of(S.SELF_VELOCITY)[0][1];
        eq(v.readDoubleBE(0), 1, 'velocity divided by 8000');
        eq(v.readDoubleBE(16), -1, 'and signed');
    }

    // -- inventory: window numbering in, engine numbering out.
    {
        const { s, of } = make();
        const items = new Array(45).fill(null).map(() => ({ blockId: -1 }));
        items[36] = { blockId: 276, itemCount: 1, itemDamage: 0 };   // hotbar 0: sword
        items[5] = { blockId: 310, itemCount: 1, itemDamage: 0 };    // helmet
        items[9] = { blockId: 1, itemCount: 64, itemDamage: 0 };     // storage: stone
        s.onWindowItems({ windowId: 0, items });
        const inv = of(S.INV_SET);
        eq(inv.length, 1, 'one INV_SET for the whole window');
        const byslot = new Map();
        for (let o = 0; o < inv[0][1].length; o += 6) {
            byslot.set(inv[0][1].readUInt8(o), {
                item: inv[0][1].readUInt16BE(o + 1), count: inv[0][1].readUInt8(o + 3),
            });
        }
        eq(byslot.size, 40, 'the 40 console slots');
        eq(byslot.get(0).item, bm.toItem(276, 0), 'window 36 -> hotbar 0');
        eq(byslot.get(0).count, 1, 'with its count');
        // The helmet lands in the right slot, but the diamond armor icons have
        // no atlas art yet (T13), so its item resolves to the air sentinel --
        // which is the documented behaviour for an item the palette cannot draw.
        eq(bm.toItem(310, 0), null, 'diamond helmet art is still missing');
        eq(byslot.get(39).item, AIR, 'window 5 -> armor helmet, drawn as nothing for now');
        eq(byslot.get(9).item, bm.toGlobal(1 << 4), 'a block item resolves through the states');
        eq(byslot.get(9).count, 64, 'a full stack');
        eq(byslot.get(20).item, AIR, 'an empty slot is the air sentinel');
        eq(byslot.get(20).count, 0, 'with count 0');

        s.onSetSlot({ windowId: 0, slot: 44, item: { blockId: 261, itemCount: 1, itemDamage: 0 } });
        const one = of(S.INV_SET).pop()[1];
        eq(one.length, 6, 'a single slot update is one record');
        eq(one.readUInt8(0), 8, 'window 44 -> hotbar 8');
        s.onSetSlot({ windowId: 1, slot: 0, item: null });
        eq(of(S.INV_SET).length, 2, 'other windows are ignored');
    }

    // -- chat vs the action bar, and the game-state signal.
    {
        const { s, of } = make();
        s.setMap(map);
        eq(s.game, GAME.WAITING, 'entering a map is WAITING');

        s.onChat({ message: '{"text":"§eGame has started"}', position: 0 });
        eq(of(S.CHAT).length, 1, 'chat goes to the chat log');
        eq(of(S.CHAT)[0][1].toString('ascii', 1), 'Game has started', 'text folded and stripped');
        eq(of(S.CHAT)[0][1].readUInt8(0), 14, 'yellow colour byte');
        eq(s.game, GAME.PLAYING, 'and the broadcast starts the game');

        s.onChat({ message: '{"text":"§fRadius: §a175"}', position: 2 });
        eq(of(S.ACTION_BAR).length, 1, 'position 2 is the action bar');
        eq(of(S.ACTION_BAR)[0][1].toString('ascii', 1), 'Radius: 175', 'border readout');
        eq(of(S.CHAT).length, 1, 'and it did NOT go to chat');

        s.onChat({ message: '{"text":"§bTeam 3 wins!"}', position: 0 });
        eq(s.game, GAME.ENDED, 'a win ends the game');
        s.setMap(null);
        eq(s.game, GAME.LOBBY, 'leaving the map is the lobby');
        s.stop();
    }

    // -- outbound chat is truncated and stripped before it can be sent.
    {
        const { s, written } = make();
        s.say('§chello §rworld');
        s.say('x'.repeat(200));
        eq(s._chatQueue.length, 2, 'both queued rather than sent back to back');
        eq(s._chatQueue[0], 'hello world', 'section codes stripped before sending');
        eq(s._chatQueue[1], 'x'.repeat(100), 'truncated to 100 characters');
        s.say('   ');
        eq(s._chatQueue.length, 2, 'an empty line is not queued');
        s.stop();
    }
}

// ---------------------------------------------------------------------------
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);

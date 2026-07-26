#!/usr/bin/env node
'use strict';
/*
 * mkscene.js -- synthesise a GCLink capture, so the console can be worked on
 * without a server, an account, or a game in progress.
 *
 *   node proxy/mkscene.js scene.gcr --players 16
 *   node proxy/index.js --replay scene.gcr --loop
 *
 * `--record` on a live game is the real article and this is not a substitute
 * for it. What it is instead is *deterministic*: the same entities in the same
 * places every run, a block burst that always lands on the same frame, and an
 * entity count that can be dialled to 128 on demand. Every "done when" in
 * Phase 3 -- players animating in the right places, nametags legible, 128
 * entities holding 60 fps, a block batch converging without dropping frames --
 * is a thing you have to be able to reproduce twice to believe, and a live
 * skywars match is never the same twice.
 *
 * It writes only the server->console direction, which is all Milestone 1 has.
 */

const fs = require('fs');
const path = require('path');

const { S, Writer, ENT, ANIM, GAME, AIR, POS_SCALE,
        DIR_TO_CONSOLE } = require('./gclink');
const mapdbMod = require('./mapdb');

// ---- arguments --------------------------------------------------------------
const argv = process.argv.slice(2);
const arg = (name, dflt) => {
    const i = argv.indexOf('--' + name);
    return i >= 0 && i + 1 < argv.length ? argv[i + 1] : dflt;
};
if (argv.includes('--help') || argv.includes('-h')) {
    console.log(`
mkscene.js -- write a synthetic GCLink capture

  node proxy/mkscene.js [out.gcr] [options]

  --map <name>       map to stage it on          (hontori)
  --players <n>      player entities             (16)
  --props <n>        arrows/snowballs/pearls/... (12)
  --seconds <n>      length of the scene         (30)
  --no-dragon        leave the ender dragon out
`.trim());
    process.exit(0);
}

/* Positional arguments are whatever is left once each flag has eaten its
 * value. `--no-dragon` takes none, so it is listed rather than assumed. */
const VALUELESS = new Set(['--no-dragon']);
const positional = [];
for (let i = 0; i < argv.length; i++) {
    if (!argv[i].startsWith('--')) { positional.push(argv[i]); continue; }
    if (!VALUELESS.has(argv[i])) i++;
}
const outPath = positional[0] || 'scene.gcr';
const mapName = arg('map', 'hontori');
const nPlayers = Number(arg('players', 16));
const nProps = Number(arg('props', 12));
const seconds = Number(arg('seconds', 30));
const withDragon = !argv.includes('--no-dragon');

const mapdb = mapdbMod.load();
const map = mapdb.get(mapName);
if (!map) {
    console.error(`no such map: ${mapName}`);
    process.exit(1);
}

// ---- capture writer ---------------------------------------------------------
const records = [];
const at = (tMs, type, payload) =>
    records.push({ tMs: Math.round(tMs), type, payload: payload || Buffer.alloc(0) });

/** MC's yaw for a horizontal direction, as the wire's byte turn.
 *  MC's look vector is (-sin yaw, cos yaw), hence the atan2 argument order. */
function yawByte(dx, dz) {
    const deg = Math.atan2(-dx, dz) * 180 / Math.PI;
    return Math.round(deg * 256 / 360) & 0xff;
}
const pitchByte = (deg) => Math.round(deg * 256 / 360) & 0xff;
const fx = (blocks) => Math.round(blocks * POS_SCALE);

// ---- the cast ---------------------------------------------------------------
const NAMES = ['pvpUS', 'Notch', 'Dinnerbone', 'Herobrine', 'Steve', 'Alex',
               'jeb_', 'Grumm', 'Technoblade', 'Dream', 'Skeppy', 'BadBoy',
               'Purpled', 'Sammy', 'Vortex', 'Astra'];
const SELF_EID = 1;

const actors = [];
for (let i = 0; i < nPlayers; i++) {
    actors.push({
        eid: 100 + i,
        type: ENT.PLAYER,
        name: NAMES[i % NAMES.length] + (i >= NAMES.length ? i : ''),
        colour: i % 16,
        // A ring of walkers: each orbits the spawn at its own radius and rate,
        // so the body/head split, the limb swing and the nametag distance cull
        // are all exercised at once and at every distance.
        r: 5 + (i % 6) * 7,
        phase: (i / Math.max(nPlayers, 1)) * Math.PI * 2,
        rate: 0.25 + (i % 5) * 0.08,
        y: 1,
        held: i % 3 === 0 ? 672 : (i % 3 === 1 ? 410 : AIR),  // sword / stone / none
    });
}

const PROP_TYPES = [ENT.ARROW, ENT.SNOWBALL, ENT.PEARL, ENT.POTION, ENT.BOBBER];
const props = [];
for (let i = 0; i < nProps; i++) {
    props.push({
        eid: 400 + i,
        type: PROP_TYPES[i % PROP_TYPES.length],
        r: 3 + (i % 4) * 5,
        phase: (i / Math.max(nProps, 1)) * Math.PI * 2,
        rate: 1.1 + (i % 3) * 0.4,
        y: 2 + (i % 5),
    });
}

const dragon = withDragon
    ? { eid: 900, type: ENT.DRAGON, r: 28, phase: 0, rate: 0.12, y: 26 }
    : null;

const all = () => [...actors, ...props, ...(dragon ? [dragon] : [])];

/** Where an orbiting actor is at time `t`, and which way it is travelling. */
function orbit(a, t) {
    const ang = a.phase + t * a.rate;
    const x = Math.cos(ang) * a.r;
    const z = Math.sin(ang) * a.r;
    const dx = -Math.sin(ang) * a.r * a.rate;
    const dz = Math.cos(ang) * a.r * a.rate;
    return { x, y: a.y, z, dx, dz };
}

// ---- the scene --------------------------------------------------------------

// t=0: who we are and where.
at(0, S.MAP_SELECT, new Writer(17)
    .u8(map.index)
    .i32(map.originX).i32(map.originY).i32(map.originZ)
    .i32(SELF_EID).done());
at(10, S.GAME_MODE, new Writer(1).u8(0).done());
at(10, S.GAME_STATE, new Writer(1).u8(GAME.PLAYING).done());
at(10, S.HEALTH, new Writer(4).f32(20).done());
at(10, S.XP, new Writer(10).f32(0.4).i16(1337).i32(0).done());
at(20, S.TELEPORT, new Writer(33)
    .f64(0).f64(1).f64(0).f32(0).f32(0).u8(1).done());

// The hotbar the server says we are holding, so INV_SET has been exercised
// before T15 ever runs.
{
    const kit = [672, 673, 674, 410, 132, AIR, AIR, AIR, AIR];
    const w = new Writer(kit.length * 6);
    kit.forEach((item, slot) =>
        w.u8(slot).u16(item).u8(item === AIR ? 0 : 1).u16(0));
    at(30, S.INV_SET, w.done());
    at(30, S.HELD_SLOT, new Writer(1).u8(0).done());
}

// t=100ms: the block burst. A platform and a tower in one batch, which is the
// shape a join-time chunk diff arrives in -- hundreds of edits in one frame,
// spread over enough chunks that World_FlushRemesh has to drain them over
// several. Split at 1024 per frame, the GCLink payload ceiling.
{
    const edits = [];
    const STONE = 410, GLASS = 132;
    for (let x = -12; x <= 12; x++)
        for (let z = -12; z <= 12; z++)
            edits.push([x, 0, z, (x + z) & 1 ? STONE : GLASS]);
    for (let y = 1; y <= 12; y++) {
        edits.push([0, y, -13, STONE]);
        edits.push([1, y, -13, GLASS]);
    }
    for (let i = 0; i < edits.length; i += 1024) {
        const chunk = edits.slice(i, i + 1024);
        const w = new Writer(chunk.length * 8);
        for (const [x, y, z, id] of chunk) w.i16(x).i16(y).i16(z).u16(id);
        at(100 + (i / 1024) * 50, S.BLOCK_SET, w.done());
    }
}

// t=200ms: everyone arrives.
for (const a of all()) {
    const p = orbit(a, 0);
    const w = new Writer(32);
    w.i32(a.eid).u8(a.type).u8(0)
     .i32(fx(p.x)).i32(fx(p.y)).i32(fx(p.z))
     .u8(yawByte(p.dx, p.dz)).u8(pitchByte(0))
     .u16(a.held === undefined ? AIR : a.held)
     .u8(a.colour === undefined ? 0xff : a.colour)
     .str8(a.name || '', 24);
    at(200, S.ENTITY_ADD, w.done());
}

// One dropped item, which is a two-step introduction on the wire: the spawn
// says an item exists and the metadata that follows says what it is.
at(200, S.ENTITY_ADD, new Writer(32)
    .i32(800).u8(ENT.ITEM).u8(0)
    .i32(fx(2)).i32(fx(1)).i32(fx(2))
    .u8(0).u8(0).u16(AIR).u8(0xff).str8('', 24).done());
at(400, S.ENTITY_EQUIP, new Writer(7).i32(800).u8(0).u16(672).done());

// 20 Hz movement for the whole cast, exactly as the live proxy flushes it.
const TICK = 50;
for (let ms = 250; ms <= seconds * 1000; ms += TICK) {
    const t = ms / 1000;
    const cast = all();
    // ENTITY_MOVE is a batch; 18 bytes a record, capped at the payload ceiling.
    for (let i = 0; i < cast.length; i += 450) {
        const part = cast.slice(i, i + 450);
        const w = new Writer(part.length * 18);
        for (const a of part) {
            const p = orbit(a, t);
            w.i32(a.eid)
             .i32(fx(p.x)).i32(fx(p.y)).i32(fx(p.z))
             .u8(yawByte(p.dx, p.dz))
             .u8(pitchByte(a.type === ENT.PLAYER ? Math.sin(t + a.phase) * 20 : 0));
        }
        at(ms, S.ENTITY_MOVE, w.done());
    }

    // Somebody swings every half second and somebody takes a hit every second,
    // so the swing arc and the red flash are always on screen somewhere.
    if (actors.length && ms % 500 === 0) {
        const a = actors[(ms / 500) % actors.length];
        at(ms, S.ENTITY_ANIM, new Writer(5).i32(a.eid).u8(ANIM.SWING).done());
    }
    if (actors.length && ms % 1000 === 0) {
        const a = actors[(ms / 1000 + 3) % actors.length];
        at(ms + 5, S.ENTITY_ANIM, new Writer(5).i32(a.eid).u8(ANIM.HURT).done());
    }
}

// Chat and the action bar. The log is hidden by default, so this is also the
// test for the unread counter: lines keep arriving whether anyone looks or not.
const CHAT = [
    [0xff, 'MegaSkywars >> Game has started! Good luck.'],
    [11,   'pvpUS: gl hf'],
    [4,    'Technoblade was slain by Dream'],
    [0xff, 'MegaSkywars >> The border begins closing in 60 seconds.'],
    [14,   'Dream: rush mid'],
    [12,   'Skeppy was knocked into the void by Purpled'],
    [0xff, 'MegaSkywars >> This is a deliberately long line, long enough that ' +
           'it has to wrap across more than one row of the scrollback ring.'],
    [10,   'Alex: gg'],
];
CHAT.forEach(([colour, text], i) => {
    const w = new Writer(text.length + 1);
    at(1500 + i * 2500, S.CHAT, w.u8(colour).raw(text).done());
});
for (let i = 0; i * 4000 < seconds * 1000; i++) {
    const text = `BORDER ${175 - i * 4}m`;
    at(1200 + i * 4000, S.ACTION_BAR,
       new Writer(text.length + 1).u8(12).raw(text).done());
}

// ---- write ------------------------------------------------------------------
records.sort((a, b) => a.tMs - b.tMs);

const head = Buffer.alloc(8);
head.write('GCRC', 0, 'ascii');
head.writeUInt16BE(1, 4);
head.writeUInt16BE(0, 6);

const parts = [head];
for (const r of records) {
    const h = Buffer.alloc(8);
    h.writeUInt32BE(r.tMs, 0);
    h.writeUInt8(DIR_TO_CONSOLE, 4);
    h.writeUInt8(r.type, 5);
    h.writeUInt16BE(r.payload.length, 6);
    parts.push(h, r.payload);
}
const out = path.resolve(outPath);
fs.writeFileSync(out, Buffer.concat(parts));

const bytes = parts.reduce((n, b) => n + b.length, 0);
console.log(`${out}: ${records.length} frames, ${(bytes / 1024).toFixed(0)} KB, ` +
            `${seconds}s on ${map.title} (g_maps[${map.index}])`);
console.log(`  ${nPlayers} players, ${nProps} props` +
            (withDragon ? ', 1 ender dragon' : '') +
            `  ->  ${nPlayers + nProps + (withDragon ? 1 : 0) + 1} entities`);
console.log(`  replay: node proxy/index.js --replay ${outPath} --loop`);

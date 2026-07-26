#!/usr/bin/env node
'use strict';
/*
 * index.js -- the proxy.
 *
 * Speaks Minecraft 1.8.9 to the MegaSkywars server on one side and GCLink to a
 * GameCube on the other. Auth, AES, zlib, varints and the 1.8 protocol state
 * machine all live here so that none of them has to live on a 486-era CPU with
 * 24 MB of RAM; the console sends intent and receives state.
 *
 *   node proxy/index.js                        connect and serve one console
 *   node proxy/index.js --record game.gcr      ...and capture the link
 *   node proxy/index.js --replay game.gcr      replay a capture, no server
 *
 * --replay is the one to reach for while working on the console. A DOL rebuild
 * is about ninety seconds; replaying the same twenty seconds of a real game
 * into it, with no account, no server and no game in progress, is what makes
 * that loop bearable.
 */

const fs = require('fs');
const net = require('net');
const dns = require('dns');
const path = require('path');

const gclink = require('./gclink');
const { GCLinkServer, Recorder, Replayer, loadCapture, S, C, typeName } = gclink;
const blockmapMod = require('./blockmap');
const mapdbMod = require('./mapdb');
const { WorldTranslator } = require('./world');
const { EntityTranslator } = require('./entities');
const { StateTranslator } = require('./state');

const TICK_MS = 50;           // 20 Hz, the server's tick and the console's
const AUTH = 'microsoft';     // not configurable -- see loadConfig()
const MC_PORT = 25565;        // "no port given"; only then is SRV consulted
const SRV_FALLBACK_DNS = ['1.1.1.1', '8.8.8.8'];

// ---- arguments and configuration -------------------------------------------
const argv = process.argv.slice(2);
const has = (name) => argv.includes('--' + name);
const arg = (name, dflt) => {
    const i = argv.indexOf('--' + name);
    return i >= 0 && i + 1 < argv.length ? argv[i + 1] : dflt;
};

if (has('help') || has('h')) {
    console.log(`
msw-gcn proxy -- Minecraft 1.8.9 <-> GCLink

  --host <h>       server host                (config.server.host)
  --port <n>       server port                (config.server.port)
  --username <u>   Microsoft account email    (config.server.username)
  --gclink-port <n>  port the console dials   (config.gclink.port)

  --record <file>  capture the GCLink stream, both directions
  --replay <file>  serve a capture instead of connecting to a server
  --speed <x>      replay speed multiplier    (1)
  --loop           restart the replay when it ends
  --packets        log every inbound packet name (very noisy)
  --quiet          errors only

Anything typed at the proxy is said by the account, so "/join <map>" works
from here until the console can compose it itself (T23).
`.trim());
    process.exit(0);
}

function loadConfig() {
    const base = JSON.parse(fs.readFileSync(path.join(__dirname, 'config.json'), 'utf8'));
    const localPath = path.join(__dirname, 'config.local.json');
    if (fs.existsSync(localPath)) {
        const local = JSON.parse(fs.readFileSync(localPath, 'utf8'));
        for (const k of Object.keys(local)) {
            base[k] = (base[k] && typeof base[k] === 'object')
                ? Object.assign({}, base[k], local[k]) : local[k];
        }
    }
    base.server.host = arg('host', base.server.host);
    base.server.port = Number(arg('port', base.server.port));
    base.server.username = arg('username', base.server.username);
    base.gclink.port = Number(arg('gclink-port', base.gclink.port));
    // Microsoft auth, always. minecraft-protocol will happily do auth 'offline'
    // -- a real account is not required to speak the protocol -- and this proxy
    // is not the thing that makes that convenient. There is deliberately no
    // flag and no config key for it.
    base.server.auth = AUTH;
    if (has('packets')) base.log.packets = true;
    if (has('quiet')) base.log.level = 'error';
    return base;
}

// ---- logging ----------------------------------------------------------------
const LEVELS = { error: 0, warn: 1, info: 2, debug: 3 };
function makeLog(level) {
    const n = LEVELS[level] ?? LEVELS.info;
    const stamp = () => new Date().toISOString().slice(11, 23);
    return {
        error: (...a) => n >= 0 && console.error(`${stamp()} !`, ...a),
        warn: (...a) => n >= 1 && console.warn(`${stamp()} ?`, ...a),
        info: (...a) => n >= 2 && console.log(`${stamp()}  `, ...a),
        debug: (...a) => n >= 3 && console.log(`${stamp()} .`, ...a),
        chat: (s) => n >= 2 && console.log(`${stamp()} >`, s),
    };
}

// ---- main -------------------------------------------------------------------
async function main() {
    const config = loadConfig();
    const log = makeLog(config.log.level);

    const blockmap = blockmapMod.load();
    const mapdb = mapdbMod.load();
    log.info(`blockmap: ${blockmap.blockCount} block ids, hash ` +
             `0x${blockmap.paletteHash.toString(16).toUpperCase().padStart(8, '0')}`);
    log.info(`mapdb: ${mapdb.size} maps, build margin ` +
             `${mapdb.margin.xz} xz / ${mapdb.margin.y} y`);

    const recordPath = arg('record', null);
    const recorder = recordPath ? new Recorder(recordPath) : null;
    if (recorder) log.info(`recording to ${recordPath}`);

    const link = new GCLinkServer({
        host: config.gclink.host,
        port: config.gclink.port,
        paletteHash: blockmap.paletteHash,
        pingIntervalMs: config.gclink.pingIntervalMs,
        log, recorder,
    });
    await link.listen();
    for (const a of localAddresses()) log.info(`  console should dial ${a}:${config.gclink.port}`);

    // -- replay: no server, no account, just the capture --------------------
    const replayPath = arg('replay', null);
    if (replayPath) {
        const records = loadCapture(replayPath);
        log.info(`capture ${replayPath}: ${records.length} frames, ` +
                 `${(records[records.length - 1]?.tMs ?? 0) / 1000}s`);
        new Replayer(link, records, {
            speed: Number(arg('speed', 1)), loop: has('loop'), log,
        });
        link.on('attach', () => log.info('console attached -- replaying'));
        installShutdown({ link, recorder, log });
        return;
    }

    // -- the live path -------------------------------------------------------
    const state = new StateTranslator({ link, blockmap, config: config.chat, log });
    const world = new WorldTranslator({
        mapdb, blockmap, link, config: config.world, log,
        getSelfEid: () => state.selfEid,
    });
    const entities = new EntityTranslator({ link, blockmap, config: config.entities, log });

    world.on('map', (m) => { state.setMap(m); entities.setMap(m); });
    world.on('lobby', () => { state.setMap(null); entities.setMap(null); });
    state.on('position', (x, y, z) => world.onPosition(x, y, z));

    link.on('attach', () => {
        // Order matters: the console needs the map before the blocks that
        // change it and before the entities standing on it.
        world.onConsoleAttached();
        state.onConsoleAttached();
        entities.onConsoleAttached();
    });
    link.on('message', (type, payload) => onConsoleMessage(type, payload, { state, log }));

    // Once, not per reconnect: an SRV record that has just changed is not the
    // failure this is here to survive, and a resolver round trip on every
    // backoff attempt would only slow reconnection down.
    const addr = await resolveServer(config.server.host, config.server.port, log);
    config.server.host = addr.host;
    config.server.port = addr.port;

    const session = { client: null, closing: false, attempts: 0 };
    connect(session, { config, log, link, state, world, entities });

    const tick = setInterval(() => {
        world.flush();
        entities.flush(state.x, state.y, state.z);
    }, TICK_MS);

    const stats = setInterval(() => {
        log.info(`${world.report()} | ${entities.report()} | ` +
                 `console ${link.attached ? `rtt ${link.rttMs} ms` : 'detached'}, ` +
                 `${(link.bytesOut / 1024).toFixed(0)} KB out`);
    }, 30000);

    installStdinConsole({ state, log });

    installShutdown({ link, recorder, log, blockmap, world, entities, state, session,
                      timers: [tick, stats] });
}

/**
 * Type at the proxy to make the account talk.
 *
 * The console can already send CHAT and it is routed to state.say() below, but
 * composing `/join hontori` on a pad is T23 and that is two milestones away. So
 * until then this is the only way to get the account out of the hub and onto a
 * map -- and without a map there is nothing for the spectate path to show.
 *
 * Only when stdin is a terminal: the proxy is usually run with its output
 * redirected, and readline over a non-tty hits EOF immediately.
 */
function installStdinConsole({ state, log }) {
    if (!process.stdin.isTTY) return;

    const rl = require('readline').createInterface({
        input: process.stdin, output: process.stdout, prompt: '',
    });
    log.info('type to chat as the account: /join <map>, /team <n>, /start');

    rl.on('line', (line) => {
        const s = line.trim();
        if (!s) return;
        // say() queues behind the 1.1 s rate limit and then drops the line if
        // there is still no client, which from a keyboard looks like nothing
        // happened at all. Say so instead.
        if (!state.client) {
            log.warn(`not connected to the server yet -- "${s}" not sent`);
            return;
        }
        state.say(s);
    });
}

/**
 * Resolve the server address the way a Minecraft client does.
 *
 * A server domain often has no A record at all -- megaskywars.com does not, and
 * only `_minecraft._tcp.megaskywars.com` points anywhere (at a TCPShield edge).
 * minecraft-protocol does consult SRV, but through c-ares, which asks the DNS
 * servers in the adapter's configuration and gives up if they do not answer. A
 * machine with a local resolver configured but not listening is the bad case:
 * getaddrinfo still works, c-ares gets ECONNREFUSED, and the library falls back
 * to an A lookup that cannot possibly succeed.
 *
 * So resolve here, with a public resolver as the second try, and hand the
 * library a host it can reach. The handshake then carries the resolved name,
 * which is both what a real client sends and what host-based routers key on.
 *
 * Only when no port was given, matching the library and the vanilla client.
 */
async function resolveServer(host, port, log) {
    if (port !== MC_PORT || net.isIP(host) || host === 'localhost') return { host, port };

    const viaSrv = (resolver) => new Promise((resolve) => {
        resolver.resolveSrv('_minecraft._tcp.' + host, (err, addrs) =>
            resolve(err || !addrs || !addrs.length ? null : addrs));
    });

    let addrs = await viaSrv(dns);
    if (!addrs) {
        const r = new dns.Resolver();
        r.setServers(SRV_FALLBACK_DNS);
        addrs = await viaSrv(r);
        if (addrs) {
            log.warn(`the system resolver (${dns.getServers().join(', ')}) did not ` +
                     `answer SRV -- used ${SRV_FALLBACK_DNS[0]} instead`);
        }
    }
    if (!addrs) return { host, port };

    addrs.sort((a, b) => a.priority - b.priority || b.weight - a.weight);
    log.info(`SRV ${host} -> ${addrs[0].name}:${addrs[0].port}`);
    return { host: addrs[0].name, port: addrs[0].port };
}

// ---- the Minecraft side ------------------------------------------------------
function connect(session, ctx) {
    const { config, log, link, state, world, entities } = ctx;
    const mc = require('minecraft-protocol');

    const opts = {
        host: config.server.host,
        port: config.server.port,
        version: config.server.version,
        auth: AUTH,
        username: config.server.username,
        // node-minecraft-protocol answers S00 KeepAlive with C00 itself; the
        // GCLink PING/PONG is a separate, independent liveness check so the
        // console can show its own RTT and notice a dead link of its own.
        keepAlive: true,
    };
    if (config.server.profilesFolder) {
        opts.profilesFolder = path.resolve(__dirname, config.server.profilesFolder);
    }
    if (!opts.username) {
        log.error('no username configured -- set server.username in ' +
                  'proxy/config.local.json or pass --username');
        process.exit(1);
    }

    log.info(`connecting to ${opts.host}:${opts.port} as ${opts.username} ` +
             `(${opts.auth}, protocol 47)`);
    const client = mc.createClient(opts);
    session.client = client;
    state.attach(client);

    client.on('login', (pkt) => {
        session.attempts = 0;
        state.onLogin(pkt);
        entities.setSelfEid(pkt.entityId);
        // Plugins read player.getLocale(), and a client with no brand looks
        // like a bot to anything that checks. Both are join-time settings; the
        // rest of the 1.8 conformance table is T22's.
        client.write('settings', {
            locale: 'en_US', viewDistance: 8, chatFlags: 0,
            chatColors: true, skinParts: 0x7f,
        });
        client.write('custom_payload', {
            channel: 'MC|Brand', data: Buffer.from('vanilla', 'utf8'),
        });
    });

    const on = (name, fn) => client.on(name, (pkt) => {
        if (config.log.packets) log.debug(`<- ${name}`);
        try { fn(pkt); }
        catch (e) { log.error(`${name}: ${e.stack || e.message}`); }
    });

    // world
    on('map_chunk', (p) => world.onMapChunk(p));
    on('map_chunk_bulk', (p) => world.onMapChunkBulk(p));
    on('block_change', (p) => world.onBlockChange(p));
    on('multi_block_change', (p) => world.onMultiBlockChange(p));

    // entities
    on('named_entity_spawn', (p) => entities.onPlayerSpawn(p));
    on('spawn_entity', (p) => entities.onObjectSpawn(p));
    on('spawn_entity_living', (p) => entities.onMobSpawn(p));
    on('entity_destroy', (p) => entities.onDestroy(p));
    on('rel_entity_move', (p) => entities.onRelMove(p, false));
    on('entity_move_look', (p) => entities.onRelMove(p, true));
    on('entity_look', (p) => entities.onLook(p));
    on('entity_teleport', (p) => entities.onTeleport(p));
    on('entity_equipment', (p) => entities.onEquipment(p));
    on('animation', (p) => entities.onAnimation(p));
    on('entity_status', (p) => entities.onStatus(p));
    on('entity_metadata', (p) => entities.onMetadata(p));
    on('player_info', (p) => entities.onPlayerInfo(p));
    on('scoreboard_team', (p) => entities.onTeam(p));

    // self
    on('position', (p) => state.onPosition(p));
    on('update_health', (p) => state.onHealth(p));
    on('experience', (p) => state.onExperience(p));
    on('game_state_change', (p) => state.onGameStateChange(p));
    on('held_item_slot', (p) => state.onHeldItemSlot(p));
    on('set_slot', (p) => state.onSetSlot(p));
    on('window_items', (p) => state.onWindowItems(p));
    on('chat', (p) => state.onChat(p));
    on('entity_velocity', (p) => {
        if (p.entityId === state.selfEid) state.onSelfVelocity(p);
    });

    client.on('kick_disconnect', (p) => {
        const why = require('./chat').parse(p.reason).text || 'kicked';
        log.error(`server kicked us: ${why}`);
        link.disconnect(`server: ${why}`.slice(0, 100));
    });
    client.on('error', (e) => log.error(`minecraft: ${e.message}`));
    client.on('end', (reason) => {
        log.warn(`minecraft connection ended${reason ? `: ${reason}` : ''}`);
        if (session.closing) return;
        // The console has its own reconnect state machine; give the server side
        // one too, so a restart on either end does not mean restarting by hand.
        if (++session.attempts > 5) {
            log.error('giving up after 5 reconnect attempts');
            link.disconnect('proxy lost the server');
            return;
        }
        const delay = Math.min(30000, 2000 * session.attempts);
        log.info(`reconnecting in ${delay / 1000}s (attempt ${session.attempts})`);
        setTimeout(() => { if (!session.closing) connect(session, ctx); }, delay);
    });
}

// ---- the console side --------------------------------------------------------
/**
 * Console -> server. Only CHAT is wired up here: movement, digging, placing and
 * attacking are T22's, and the plan is explicit that landing half of that gets
 * the account kicked within seconds with a failure mode that is very hard to
 * see from the console. Anything else is logged once and dropped.
 */
function onConsoleMessage(type, payload, { state, log }) {
    if (type === C.CHAT) {
        state.say(payload.toString('ascii'));
        return;
    }
    if (!onConsoleMessage.seen) onConsoleMessage.seen = new Set();
    if (!onConsoleMessage.seen.has(type)) {
        onConsoleMessage.seen.add(type);
        log.warn(`console sent ${typeName(type)} (${payload.length} B) -- ` +
                 `no handler yet, that is T22's`);
    }
}

// ---- shutdown ----------------------------------------------------------------
function installShutdown(ctx) {
    let done = false;
    const bye = () => {
        if (done) return;
        done = true;
        const { link, recorder, log, blockmap, world, entities, state, session, timers } = ctx;
        for (const t of timers || []) clearInterval(t);
        if (session) session.closing = true;
        if (state) state.stop();
        if (world) log.info(world.report());
        if (entities) log.info(entities.report());
        if (blockmap) {
            const miss = blockmap.missingReport();
            if (miss.length) log.warn(`states this palette cannot express: ${miss.join(', ')}`);
        }
        if (recorder) {
            recorder.close();
            log.info(`capture: ${recorder.count} frames -> ${recorder.path}`);
        }
        if (session && session.client) session.client.end();
        link.disconnect('proxy shutting down');
        setTimeout(() => { link.close(); process.exit(0); }, 150);
    };
    process.on('SIGINT', bye);
    process.on('SIGTERM', bye);
}

function localAddresses() {
    const out = [];
    for (const list of Object.values(require('os').networkInterfaces())) {
        for (const a of list) if (a.family === 'IPv4' && !a.internal) out.push(a.address);
    }
    return out;
}

main().catch((e) => { console.error(e.stack || e.message); process.exit(1); });

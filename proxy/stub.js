#!/usr/bin/env node
/*
 * stub.js -- the smallest thing a GameCube can talk to.
 *
 * Speaks GCLink (see source/gclink.h) and nothing else: it answers HELLO,
 * validates the block-palette hash against proxy/blockmap.json, and then keeps
 * the link alive with a PING every second, feeding back the round trip it
 * measured so the console can display its own latency. Anything else the
 * console sends is logged and dropped.
 *
 * This exists so the console side of the transport can be built and debugged
 * with no Minecraft server, no account and no protocol translation in the way
 * -- when the real proxy (T6) misbehaves, this is the known-good other end to
 * bisect against.
 *
 *   node proxy/stub.js [--port 25566] [--host 0.0.0.0] [--drop-after <sec>]
 *
 * --drop-after cuts the link after N seconds, which is how the console's
 * reconnect path gets exercised deliberately rather than by unplugging things.
 * --hash <hex> answers HELLO with a deliberately wrong palette hash, to check
 * that a stale blockmap is refused at the handshake instead of quietly
 * rendering the map as the wrong blocks.
 */
'use strict';

const net = require('net');
const fs = require('fs');
const path = require('path');

// ---- the wire contract, mirrored from source/gclink.h ---------------------
const GCLINK_VERSION = 1;
const GCLINK_MAX_PAYLOAD = 8192;
const HEADER = 3;

const S = { HELLO: 0x01, DISCONNECT: 0x02, PING: 0x03 };
const C = { HELLO: 0x81, PONG: 0x83, MOVE: 0x90, CHAT: 0x97 };
const HELLO_OK = 0, HELLO_VERSION = 1, HELLO_BLOCKMAP = 2, HELLO_BUSY = 3;

const NAMES = Object.create(null);
for (const [k, v] of Object.entries(S)) NAMES[v] = 'S_' + k;
for (const [k, v] of Object.entries(C)) NAMES[v] = 'C_' + k;

// ---- arguments -------------------------------------------------------------
const argv = process.argv.slice(2);
const arg = (name, dflt) => {
    const i = argv.indexOf('--' + name);
    return i >= 0 && i + 1 < argv.length ? argv[i + 1] : dflt;
};
const PORT = Number(arg('port', 25566));
const HOST = arg('host', '0.0.0.0');
const DROP_AFTER = Number(arg('drop-after', 0));

// ---- the palette hash both ends must agree on ------------------------------
let paletteHash = null;
try {
    const doc = JSON.parse(
        fs.readFileSync(path.join(__dirname, 'blockmap.json'), 'utf8'));
    paletteHash = doc.paletteHash >>> 0;
    console.log(`blockmap.json: ${doc.blockCount} block ids, ` +
                `hash 0x${paletteHash.toString(16).toUpperCase().padStart(8, '0')}`);
    const forced = arg('hash', null);
    if (forced !== null) {
        paletteHash = parseInt(forced, 16) >>> 0;
        console.log(`  overridden to 0x${paletteHash.toString(16).toUpperCase()} ` +
                    `(--hash): the console should refuse this`);
    }
} catch (e) {
    console.error('cannot read proxy/blockmap.json ' +
                  '(run: python tools/gen_blockmap.py):', e.message);
    process.exit(1);
}

// ---- framing ---------------------------------------------------------------
function frame(type, payload) {
    const body = payload || Buffer.alloc(0);
    const buf = Buffer.alloc(HEADER + body.length);
    buf.writeUInt16BE(body.length + 1, 0);   // length counts the type byte
    buf.writeUInt8(type, 2);
    body.copy(buf, HEADER);
    return buf;
}

/* TCP is a byte stream: a frame can arrive split across segments, and several
 * can arrive in one. Accumulate and only hand up whole frames. */
class Reader {
    constructor() { this.buf = Buffer.alloc(0); }
    push(chunk, onMessage) {
        this.buf = Buffer.concat([this.buf, chunk]);
        for (;;) {
            if (this.buf.length < HEADER) return true;
            const flen = this.buf.readUInt16BE(0);
            if (flen < 1 || flen > GCLINK_MAX_PAYLOAD + 1) return false;
            const total = HEADER + flen - 1;
            if (this.buf.length < total) return true;
            const type = this.buf.readUInt8(2);
            const payload = this.buf.subarray(HEADER, total);
            onMessage(type, payload);
            this.buf = this.buf.subarray(total);
        }
    }
}

// ---- one console at a time -------------------------------------------------
let active = null;

const server = net.createServer((sock) => {
    const who = `${sock.remoteAddress}:${sock.remotePort}`;

    if (active) {
        console.log(`${who}: rejected, a console is already attached`);
        const pl = Buffer.alloc(6);
        pl.writeUInt8(GCLINK_VERSION, 0);
        pl.writeUInt32BE(paletteHash, 1);
        pl.writeUInt8(HELLO_BUSY, 5);
        sock.end(frame(S.HELLO, pl));
        return;
    }
    active = sock;
    sock.setNoDelay(true);      // MOVE is 20 Hz and tiny; Nagle would add lag

    console.log(`${who}: connected`);
    const reader = new Reader();
    let ready = false, pinger = null, dropper = null;
    let token = 1, sentAt = 0, rttMs = 0;
    const counts = Object.create(null);

    const bye = (why) => {
        console.log(`${who}: disconnecting -- ${why}`);
        sock.end(frame(S.DISCONNECT, Buffer.from(why, 'ascii')));
    };

    sock.on('data', (chunk) => {
        const ok = reader.push(chunk, (type, payload) => {
            counts[type] = (counts[type] || 0) + 1;

            if (type === C.HELLO) {
                if (payload.length < 5) return bye('short HELLO');
                const ver = payload.readUInt8(0);
                const hash = payload.readUInt32BE(1);
                let result = HELLO_OK;
                if (ver !== GCLINK_VERSION) result = HELLO_VERSION;
                else if (hash !== paletteHash) result = HELLO_BLOCKMAP;

                const pl = Buffer.alloc(6);
                pl.writeUInt8(GCLINK_VERSION, 0);
                pl.writeUInt32BE(paletteHash, 1);
                pl.writeUInt8(result, 5);
                sock.write(frame(S.HELLO, pl));

                if (result !== HELLO_OK) {
                    console.log(`${who}: HELLO rejected (` +
                        `console v${ver} hash 0x${hash.toString(16)}, ` +
                        `stub v${GCLINK_VERSION} hash 0x${paletteHash.toString(16)})`);
                    return sock.end();
                }
                ready = true;
                console.log(`${who}: HELLO ok, v${ver}, palette matches`);

                const ping = () => {
                    const pl2 = Buffer.alloc(6);
                    pl2.writeUInt32BE(token >>> 0, 0);
                    pl2.writeUInt16BE(Math.min(rttMs, 65535), 4);
                    sentAt = Date.now();
                    sock.write(frame(S.PING, pl2));
                };
                ping();
                pinger = setInterval(ping, 1000);
                if (DROP_AFTER > 0) {
                    dropper = setTimeout(
                        () => bye('drop-after elapsed'), DROP_AFTER * 1000);
                }
                return;
            }

            if (!ready) return bye('first frame was not HELLO');

            if (type === C.PONG) {
                if (payload.length >= 4 && payload.readUInt32BE(0) === (token >>> 0)) {
                    rttMs = Date.now() - sentAt;
                    token++;
                }
                return;
            }
            // Everything else belongs to a later task; note it and move on.
            const name = NAMES[type] || `0x${type.toString(16)}`;
            console.log(`${who}: ${name}, ${payload.length} B ` +
                        `(no handler yet -- that is T6/T8's job)`);
        });
        if (!ok) bye('bad frame length');
    });

    const done = (label) => {
        if (pinger) clearInterval(pinger);
        if (dropper) clearTimeout(dropper);
        if (active === sock) active = null;
        const summary = Object.entries(counts)
            .map(([t, n]) => `${NAMES[t] || t}x${n}`).join(' ') || 'nothing';
        console.log(`${who}: ${label}; received ${summary}`);
    };
    sock.on('close', () => done('closed'));
    sock.on('error', (e) => done(`error ${e.code || e.message}`));
});

server.listen(PORT, HOST, () => {
    console.log(`GCLink stub on ${HOST}:${PORT}`);
    for (const a of localAddresses()) console.log(`  reachable at ${a}:${PORT}`);
    console.log('set NET_PROXY_IP in source/main.c to whichever the console can see');
});

function localAddresses() {
    const out = [];
    const ifaces = require('os').networkInterfaces();
    for (const list of Object.values(ifaces))
        for (const a of list)
            if (a.family === 'IPv4' && !a.internal) out.push(a.address);
    return out;
}

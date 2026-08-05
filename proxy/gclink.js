'use strict';
/*
 * gclink.js -- the console-facing half of the proxy: the wire contract, the
 * framing, the one-console-at-a-time server, and the record/replay capture.
 *
 * The authority for everything in the first section is source/gclink.h. This
 * file is a transcription of it, and the two are checked against each other at
 * the handshake only for the block palette -- there is no way to check field
 * widths at runtime, so when you change a layout, change it there first.
 *
 * Framing, big-endian:   u16 length | u8 type | payload[length - 1]
 * `length` counts the type byte, so the smallest legal frame is 3 bytes.
 */

const net = require('net');
const fs = require('fs');
const { EventEmitter } = require('events');

// ---- the wire contract, mirrored from source/gclink.h ----------------------
const VERSION = 1;
const PORT = 25566;
const MAX_PAYLOAD = 8192;
const HEADER = 3;
const PING_TIMEOUT_MS = 8000;

/**
 * When the console counts as behind, and why there are two tests.
 *
 * The Broadband Adapter's lwIP drains somewhere around ten to fifteen KB/s. A
 * sender that exceeds that does not get an error -- it gets a queue, and the
 * queue is invisible from here: node hands a write straight to the kernel
 * whenever the kernel will take it, so `writableLength` sits at zero while the
 * OS send buffer quietly accumulates *seconds* of backlog. That is why the
 * console's RTT readout climbs through half a second in the middle of a fight
 * while every counter in this proxy looks healthy.
 *
 * So congestion is read off the round trip, which is the one number that
 * contains the whole queue no matter who is holding it, with the node-side
 * backlog as a second, faster test for the case where node *is* the one
 * holding it. Above either, the translators stop queueing state.
 */
const SEND_HIGH_WATER = 3072;
const CONGESTED_RTT_MS = 200;

/** How many outstanding pings to keep send times for. At four a second this is
 *  four seconds of history, which is longer than any round trip that is not
 *  already a dead link. */
const PING_HISTORY = 16;

/** Air. The engine stores air as -1, which does not survive a u16. */
const AIR = 0xffff;
/** Entity fixed-point: 32 units per block, straight off the 1.8 wire. */
const POS_SCALE = 32;

const S = {
    HELLO: 0x01, DISCONNECT: 0x02, PING: 0x03,
    MAP_SELECT: 0x10, BLOCK_SET: 0x11, GAME_STATE: 0x12, TELEPORT: 0x13,
    GAME_MODE: 0x14,
    ENTITY_ADD: 0x20, ENTITY_MOVE: 0x21, ENTITY_REMOVE: 0x22,
    ENTITY_EQUIP: 0x23, ENTITY_ANIM: 0x24,
    HEALTH: 0x30, SELF_VELOCITY: 0x31, INV_SET: 0x32, HELD_SLOT: 0x33,
    XP: 0x34, USE_STATE: 0x35,
    CHAT: 0x40, ACTION_BAR: 0x41,
};

const C = {
    HELLO: 0x81, PONG: 0x83,
    MOVE: 0x90, DIG: 0x91, PLACE: 0x92, USE_ENTITY: 0x93, USE_ITEM: 0x94,
    SWING: 0x95, HELD_SLOT: 0x96, CHAT: 0x97, ACTION: 0x98,
    WINDOW_CLICK: 0x99,
};

const HELLO = { OK: 0, VERSION: 1, BLOCKMAP: 2, BUSY: 3 };
const GAME = { LOBBY: 0, WAITING: 1, PLAYING: 2, ENDED: 3 };
const ENT = {
    PLAYER: 0, ITEM: 1, ARROW: 2, SNOWBALL: 3, PEARL: 4, POTION: 5,
    BOBBER: 6, DRAGON: 7,
};
const EFLAG = { SNEAKING: 1, SPRINTING: 2, INVISIBLE: 4, USING: 8 };
const ANIM = { SWING: 0, HURT: 1, DEATH: 2, EAT: 3 };
const ACTION = { DROP_ITEM: 0, DROP_STACK: 1, RESPAWN: 2, RESYNC: 3 };

/** Records per frame, from the payload cap: BLOCK_SET is 8 B, MOVE 18 B. */
const BLOCK_SET_MAX = Math.floor(MAX_PAYLOAD / 8);   // 1024
const ENTITY_MOVE_MAX = Math.floor(MAX_PAYLOAD / 18); // 455

const TYPE_NAME = Object.create(null);
for (const [k, v] of Object.entries(S)) TYPE_NAME[v] = 'S_' + k;
for (const [k, v] of Object.entries(C)) TYPE_NAME[v] = 'C_' + k;
const typeName = (t) => TYPE_NAME[t] || `0x${t.toString(16).padStart(2, '0')}`;

// ---- payload building ------------------------------------------------------

/**
 * A growable big-endian writer. Big-endian is free on PowerPC and every field
 * here is written and read by exactly one line of code on each side, so the
 * layouts stay legible at the call site rather than hiding in a schema.
 */
class Writer {
    constructor(hint = 64) { this.b = Buffer.alloc(hint); this.p = 0; }

    _room(n) {
        if (this.p + n <= this.b.length) return;
        let cap = this.b.length * 2;
        while (cap < this.p + n) cap *= 2;
        const nb = Buffer.alloc(cap);
        this.b.copy(nb, 0, 0, this.p);
        this.b = nb;
    }

    u8(v) { this._room(1); this.b.writeUInt8(v & 0xff, this.p); this.p += 1; return this; }
    i8(v) { this._room(1); this.b.writeInt8(clamp(v, -128, 127), this.p); this.p += 1; return this; }
    u16(v) { this._room(2); this.b.writeUInt16BE(v & 0xffff, this.p); this.p += 2; return this; }
    i16(v) { this._room(2); this.b.writeInt16BE(clamp(v, -32768, 32767), this.p); this.p += 2; return this; }
    u32(v) { this._room(4); this.b.writeUInt32BE(v >>> 0, this.p); this.p += 4; return this; }
    i32(v) { this._room(4); this.b.writeInt32BE(clamp(v | 0, -2147483648, 2147483647), this.p); this.p += 4; return this; }
    f32(v) { this._room(4); this.b.writeFloatBE(v, this.p); this.p += 4; return this; }
    f64(v) { this._room(8); this.b.writeDoubleBE(v, this.p); this.p += 8; return this; }

    /** ASCII, length-prefixed with a u8. Truncates: the console's buffers are fixed. */
    str8(s, max = 255) {
        const bytes = Buffer.from(String(s).slice(0, max), 'ascii');
        this.u8(bytes.length);
        this._room(bytes.length);
        bytes.copy(this.b, this.p);
        this.p += bytes.length;
        return this;
    }

    /** Raw ASCII with no prefix -- the frame length bounds it. */
    raw(s) {
        const bytes = Buffer.from(String(s), 'ascii');
        this._room(bytes.length);
        bytes.copy(this.b, this.p);
        this.p += bytes.length;
        return this;
    }

    get length() { return this.p; }
    done() { return this.b.subarray(0, this.p); }
}

const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

/** Byte turn: 256 units = 360 degrees, the 1.8 wire's own angle unit. */
const angleToByte = (deg) => Math.round(deg * 256 / 360) & 0xff;

function frame(type, payload) {
    const body = payload || Buffer.alloc(0);
    if (body.length > MAX_PAYLOAD) {
        throw new Error(`${typeName(type)} payload ${body.length} B > ${MAX_PAYLOAD}`);
    }
    const buf = Buffer.alloc(HEADER + body.length);
    buf.writeUInt16BE(body.length + 1, 0);   // length counts the type byte
    buf.writeUInt8(type, 2);
    body.copy(buf, HEADER);
    return buf;
}

/**
 * TCP is a byte stream: a frame can arrive split across segments and several
 * can arrive in one. Accumulate and hand up only whole frames. Returns false
 * on a length no legal sender could have written, which is a desync -- there
 * is no resynchronising a length-prefixed stream, so the caller drops the link.
 */
class Reader {
    constructor() { this.buf = Buffer.alloc(0); }
    push(chunk, onMessage) {
        this.buf = this.buf.length ? Buffer.concat([this.buf, chunk]) : chunk;
        for (;;) {
            if (this.buf.length < HEADER) return true;
            const flen = this.buf.readUInt16BE(0);
            if (flen < 1 || flen > MAX_PAYLOAD + 1) return false;
            const total = HEADER + flen - 1;
            if (this.buf.length < total) return true;
            onMessage(this.buf.readUInt8(2), this.buf.subarray(HEADER, total));
            this.buf = this.buf.subarray(total);
        }
    }
}

// ---- capture ---------------------------------------------------------------
/*
 * --record dumps every framed message, both directions, with timing. --replay
 * feeds the server->console half back at the recorded pace with no Minecraft
 * server, no account and no game in progress involved. Iterating on the console
 * costs a ~90 s DOL rebuild, so being able to replay the same three seconds of
 * a fight over and over is worth more than any other tooling in the proxy.
 *
 *   "GCRC" | u16 version | u16 reserved
 *   then: u32 tMs | u8 dir (0 = to console) | u8 type | u16 len | payload
 */
const CAPTURE_MAGIC = 'GCRC';
const CAPTURE_VERSION = 1;
const DIR_TO_CONSOLE = 0;
const DIR_TO_SERVER = 1;

class Recorder {
    constructor(path) {
        this.fd = fs.openSync(path, 'w');
        this.t0 = Date.now();
        this.count = 0;
        this.path = path;
        const head = Buffer.alloc(8);
        head.write(CAPTURE_MAGIC, 0, 'ascii');
        head.writeUInt16BE(CAPTURE_VERSION, 4);
        head.writeUInt16BE(0, 6);
        fs.writeSync(this.fd, head);
    }
    record(dir, type, payload) {
        if (this.fd === null) return;
        const body = payload || Buffer.alloc(0);
        const rec = Buffer.alloc(8 + body.length);
        rec.writeUInt32BE(Math.min(Date.now() - this.t0, 0xffffffff), 0);
        rec.writeUInt8(dir, 4);
        rec.writeUInt8(type, 5);
        rec.writeUInt16BE(body.length, 6);
        body.copy(rec, 8);
        fs.writeSync(this.fd, rec);
        this.count++;
    }
    close() {
        if (this.fd === null) return;
        fs.closeSync(this.fd);
        this.fd = null;
    }
}

function loadCapture(path) {
    const buf = fs.readFileSync(path);
    if (buf.length < 8 || buf.toString('ascii', 0, 4) !== CAPTURE_MAGIC) {
        throw new Error(`${path}: not a GCLink capture`);
    }
    const ver = buf.readUInt16BE(4);
    if (ver !== CAPTURE_VERSION) {
        throw new Error(`${path}: capture version ${ver}, expected ${CAPTURE_VERSION}`);
    }
    const out = [];
    let p = 8;
    while (p + 8 <= buf.length) {
        const tMs = buf.readUInt32BE(p);
        const dir = buf.readUInt8(p + 4);
        const type = buf.readUInt8(p + 5);
        const len = buf.readUInt16BE(p + 6);
        p += 8;
        if (p + len > buf.length) break;      // truncated tail: a killed proxy
        out.push({ tMs, dir, type, payload: buf.subarray(p, p + len) });
        p += len;
    }
    return out;
}

/**
 * Replay a capture into whichever console attaches. HELLO and PING are skipped
 * and regenerated by the live server object -- the recorded handshake belongs
 * to a different socket, and a recorded PING would carry a round trip that was
 * measured somewhere else.
 */
class Replayer {
    constructor(server, records, { speed = 1, loop = false, log = console } = {}) {
        this.server = server;
        this.records = records.filter((r) =>
            r.dir === DIR_TO_CONSOLE && r.type !== S.HELLO && r.type !== S.PING);
        this.speed = speed;
        this.loop = loop;
        this.log = log;
        this.timer = null;
        server.on('attach', () => this.start());
        server.on('detach', () => this.stop());
    }
    start() {
        this.stop();
        if (!this.records.length) return this.log.warn('capture has nothing to replay');
        this.i = 0;
        this.t0 = Date.now();
        this.log.info(`replaying ${this.records.length} frames at ${this.speed}x` +
                      (this.loop ? ', looping' : ''));
        this.step();
    }
    step() {
        const now = Date.now();
        while (this.i < this.records.length) {
            const r = this.records[this.i];
            const due = this.t0 + r.tMs / this.speed;
            if (due > now) {
                this.timer = setTimeout(() => this.step(), Math.min(due - now, 200));
                return;
            }
            this.server.send(r.type, r.payload);
            this.i++;
        }
        if (this.loop) { this.i = 0; this.t0 = Date.now(); this.timer = setTimeout(() => this.step(), 500); }
        else this.log.info('replay finished');
    }
    stop() { if (this.timer) { clearTimeout(this.timer); this.timer = null; } }
}

// ---- the server -------------------------------------------------------------

/**
 * One console at a time, by design: a second attaching would double every
 * ENTITY_MOVE and there is exactly one Minecraft account behind this.
 *
 * Emits 'attach' once HELLO succeeds, 'message' (type, payload) for everything
 * the console sends afterwards, and 'detach' (reason) when the link ends.
 * HELLO, PING and PONG never surface -- they are this object's own business,
 * the same split the console makes in Net_Poll.
 */
class GCLinkServer extends EventEmitter {
    constructor({ host = '0.0.0.0', port = PORT, paletteHash, pingIntervalMs = 250,
                  sendHighWater = SEND_HIGH_WATER, congestedRttMs = CONGESTED_RTT_MS,
                  log = console, recorder = null } = {}) {
        super();
        this.host = host;
        this.port = port;
        this.paletteHash = paletteHash >>> 0;
        this.pingIntervalMs = pingIntervalMs;
        this.sendHighWater = sendHighWater;
        this.congestedRttMs = congestedRttMs;
        this.log = log;
        this.recorder = recorder;

        this.sock = null;         // the attached console, once HELLO succeeds
        this.ready = false;
        this.rttMs = 0;
        this.peakRttMs = 0;
        this.bytesOut = 0;
        this.bytesIn = 0;
        // type -> [frames, bytes]. Which frame type is filling the link is not
        // something to reason about from first principles -- a fight looks
        // nothing like a join -- so count it.
        this.byType = new Map();
        this._token = 1;
        this._pings = new Map();  // token -> when it was queued
        this._pinger = null;

        this.server = net.createServer((s) => this._accept(s));
    }

    listen() {
        return new Promise((resolve, reject) => {
            this.server.once('error', reject);
            this.server.listen(this.port, this.host, () => {
                this.log.info(`GCLink listening on ${this.host}:${this.port}` +
                              `, palette 0x${this.paletteHash.toString(16).toUpperCase()}`);
                resolve();
            });
        });
    }

    close() {
        this._stopPing();
        if (this.sock) this.sock.destroy();
        this.server.close();
    }

    get attached() { return this.ready; }

    /** Bytes node is holding for the console. Only part of the real backlog --
     *  see SEND_HIGH_WATER -- but the part that grows without any bound at all. */
    get backlog() { return this.sock ? this.sock.writableLength : 0; }

    /**
     * Is the console behind?
     *
     * Senders of *state* -- entity positions, the block diff -- must hold off
     * while this is true: every one of those frames is superseded by the next
     * tick's, so queueing one now buys a stale position later and delays
     * everything behind it in the stream. Senders of *events* -- adds,
     * removals, animations, inventory -- must not hold off: there is no later
     * frame that carries them, and dropping one desyncs the console for good.
     */
    get congested() {
        return this.backlog > this.sendHighWater || this.rttMs > this.congestedRttMs;
    }

    /** Queue one frame. Silently drops when no console is attached -- the
     *  translation layers run whether or not anyone is listening, and making
     *  every call site check would only spread that test around. */
    send(type, payload) {
        if (!this.sock || !this.ready) return false;
        const buf = frame(type, payload);
        this.sock.write(buf);
        this.bytesOut += buf.length;
        const t = this.byType.get(type);
        if (t) { t[0]++; t[1] += buf.length; } else this.byType.set(type, [1, buf.length]);
        if (this.recorder) this.recorder.record(DIR_TO_CONSOLE, type, payload);
        return true;
    }

    /**
     * Bytes and frames per type since the last call, busiest first, and the
     * peak RTT over the same window. The averages a 30 second counter gives
     * are exactly the wrong shape for this: the link is fine at rest and the
     * question is what it is carrying during the two seconds of a fight.
     */
    takeStats() {
        const rows = [...this.byType.entries()]
            .map(([type, [n, bytes]]) => ({ type, name: typeName(type), n, bytes }))
            .sort((a, b) => b.bytes - a.bytes);
        const bytes = rows.reduce((s, r) => s + r.bytes, 0);
        const peak = this.peakRttMs;
        this.byType.clear();
        this.peakRttMs = this.rttMs;
        return { rows, bytes, peakRttMs: peak, rttMs: this.rttMs, backlog: this.backlog };
    }

    disconnect(reason) {
        if (!this.sock) return;
        this.log.info(`disconnecting console -- ${reason}`);
        try { this.sock.end(frame(S.DISCONNECT, Buffer.from(String(reason).slice(0, 120), 'ascii'))); }
        catch (_) { /* already gone */ }
    }

    // -- internals ----------------------------------------------------------
    _accept(sock) {
        const who = `${sock.remoteAddress}:${sock.remotePort}`;

        if (this.sock) {
            this.log.warn(`${who}: rejected, a console is already attached`);
            sock.end(frame(S.HELLO, helloPayload(this.paletteHash, HELLO.BUSY)));
            return;
        }

        this.sock = sock;
        this.ready = false;
        // MOVE is 20 Hz and tiny, and BLOCK_SET wants to land the frame it was
        // produced in. Nagle would trade both for bandwidth we are not short of.
        sock.setNoDelay(true);
        this.log.info(`${who}: connected`);

        const reader = new Reader();
        sock.on('data', (chunk) => {
            this.bytesIn += chunk.length;
            const ok = reader.push(chunk, (type, payload) => this._onFrame(type, payload));
            if (!ok) this._drop('bad frame length');
        });
        sock.on('close', () => this._closed(who, 'closed'));
        sock.on('error', (e) => this._closed(who, `error ${e.code || e.message}`));
    }

    _onFrame(type, payload) {
        if (this.recorder) this.recorder.record(DIR_TO_SERVER, type, payload);

        if (type === C.HELLO) {
            if (payload.length < 5) return this._drop('short HELLO');
            const ver = payload.readUInt8(0);
            const hash = payload.readUInt32BE(1);
            let result = HELLO.OK;
            if (ver !== VERSION) result = HELLO.VERSION;
            else if (hash !== this.paletteHash) result = HELLO.BLOCKMAP;

            this.sock.write(frame(S.HELLO, helloPayload(this.paletteHash, result)));
            if (result !== HELLO.OK) {
                this.log.error(`HELLO rejected: console v${ver} hash ` +
                    `0x${hash.toString(16).toUpperCase()}, proxy v${VERSION} hash ` +
                    `0x${this.paletteHash.toString(16).toUpperCase()}` +
                    (result === HELLO.BLOCKMAP
                        ? ' -- rebuild the DOL after regenerating blockmap' : ''));
                return this.sock.end();
            }
            this.ready = true;
            this.log.info(`HELLO ok, v${ver}, palette matches`);
            this._startPing();
            this.emit('attach');
            return;
        }

        if (!this.ready) return this._drop('first frame was not HELLO');

        if (type === C.PONG) {
            if (payload.length < 4) return;
            const token = payload.readUInt32BE(0);
            const sentAt = this._pings.get(token);
            if (sentAt === undefined) return;   // duplicate, or older than the history
            this.rttMs = Date.now() - sentAt;
            if (this.rttMs > this.peakRttMs) this.peakRttMs = this.rttMs;
            // Tokens go out in order, so anything not newer than this one is
            // answered or lost, and either way it is not being waited on.
            for (const t of this._pings.keys()) {
                if (t > token) break;
                this._pings.delete(t);
            }
            return;
        }

        this.emit('message', type, payload);
    }

    /**
     * A new token and a new send time every interval.
     *
     * The previous version reused one token until it was answered and stamped
     * `_sentAt` on every retransmission, so the round trip it reported was
     * measured from the *last* ping sent to the PONG for some earlier one --
     * which under exactly the congestion this is here to detect reads back as a
     * fraction of the real delay. Congestion is now a control input, so it has
     * to be the real delay.
     */
    _startPing() {
        this._stopPing();
        this._pings.clear();
        const ping = () => {
            const token = this._token++ >>> 0;
            this._pings.set(token, Date.now());
            while (this._pings.size > PING_HISTORY) {
                this._pings.delete(this._pings.keys().next().value);
            }
            this.send(S.PING, new Writer(6)
                .u32(token).u16(Math.min(this.rttMs, 65535)).done());
        };
        ping();
        this._pinger = setInterval(ping, this.pingIntervalMs);
    }

    _stopPing() {
        if (this._pinger) { clearInterval(this._pinger); this._pinger = null; }
        this._pings.clear();
    }

    _drop(why) {
        this.log.error(`console link: ${why}`);
        this.disconnect(why);
        if (this.sock) this.sock.destroy();
    }

    _closed(who, label) {
        if (!this.sock) return;
        this.sock = null;
        this.ready = false;
        this.rttMs = 0;
        this._stopPing();
        this.log.info(`${who}: ${label}`);
        this.emit('detach', label);
    }
}

function helloPayload(hash, result) {
    return new Writer(6).u8(VERSION).u32(hash).u8(result).done();
}

module.exports = {
    VERSION, PORT, MAX_PAYLOAD, HEADER, PING_TIMEOUT_MS,
    SEND_HIGH_WATER, CONGESTED_RTT_MS,
    AIR, POS_SCALE, BLOCK_SET_MAX, ENTITY_MOVE_MAX,
    S, C, HELLO, GAME, ENT, EFLAG, ANIM, ACTION,
    typeName, frame, angleToByte, clamp,
    Writer, Reader, GCLinkServer,
    Recorder, Replayer, loadCapture, DIR_TO_CONSOLE, DIR_TO_SERVER,
};

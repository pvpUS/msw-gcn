#ifndef MSW_GCLINK_H
#define MSW_GCLINK_H

#include <gccore.h>

/* GCLink -- the wire protocol between the GameCube and the Node proxy.
 *
 * The console does not speak Minecraft. Mojang auth, AES, zlib, varints and
 * the whole 1.8 protocol state machine live in the proxy; over this link the
 * console sends *intent* ("I moved, I dug here, I hit that entity") and
 * receives *state* ("this block is now stone, this entity is there, your
 * health is 14"). That split is deliberate: JS iterates in seconds where a DOL
 * rebuild is ninety.
 *
 * Framing -- big-endian, which is free on PowerPC, and length-prefixed:
 *
 *     u16 length | u8 type | payload[length - 1]
 *
 * `length` counts the type byte, so the smallest legal frame is 3 bytes and
 * carries no payload. One plaintext TCP socket, no encryption, no
 * compression: this link never leaves the LAN.
 *
 * Type ids are split by direction -- server->console below 0x80, console->
 * server at or above -- so a frame sent the wrong way is a loud error rather
 * than a plausible-looking one.
 *
 * Both ends put BLOCKMAP_HASH in HELLO. The console's atlas, shape and block
 * property tables are generated from data/blockids.txt and baked into the
 * DOL; the proxy's blockmap.json comes from the same file. If those two ever
 * disagree the console renders an entire map as the wrong blocks, which is
 * exactly the sort of failure that should stop at the handshake.
 */

#define GCLINK_VERSION      1
#define GCLINK_PORT         25566

/* Largest payload either end may send. The length field would allow 65534,
 * but the console has to hold a whole frame in a fixed buffer to hand it to a
 * handler contiguously, and heap here is scarce. 8 KB is 1024 blocks in one
 * BLOCK_SET batch, comfortably more than a join-time chunk diff coalesces
 * into per tick; the proxy splits anything larger. A frame claiming more than
 * this is a protocol error and drops the link. */
#define GCLINK_MAX_PAYLOAD  8192
#define GCLINK_HEADER       3

/* ---- server -> console --------------------------------------------------
 * Implemented by T3: HELLO, DISCONNECT, PING. The rest are the contract the
 * proxy (T6-T8) and the console (T11, T15, T22) build against; their payload
 * layouts are noted where the plan fixes them and left to the owning task
 * where it does not. */
enum {
	/* u8 version, u32 blockmapHash, u8 result (GCLINK_HELLO_*) */
	GC_S_HELLO         = 0x01,
	/* char reason[] -- not NUL-terminated; the frame length bounds it */
	GC_S_DISCONNECT    = 0x02,
	/* u32 token, u16 rttMs. Echo the token back in GC_C_PONG. rttMs is what
	 * the *proxy* measured from the previous exchange: the console has no
	 * other way to know its own latency, and this costs two bytes on a
	 * once-a-second frame. Also the link's liveness signal -- no PING within
	 * GCLINK_PING_TIMEOUT_MS means the proxy or the network is gone. */
	GC_S_PING          = 0x03,

	/* u8 mapIndex, s32 originX, originY, originZ, s32 selfEid.
	 * The console loads its own embedded .mworld; the origin converts the
	 * server's absolute coordinates to that map's grid. */
	GC_S_MAP_SELECT    = 0x10,
	/* { s16 x, y, z; u16 globalId } * n, n = payload / 8. Map-grid
	 * coordinates. Apply through World_SetBlockDeferred (T24). */
	GC_S_BLOCK_SET     = 0x11,
	/* u8 state (GCLINK_GAME_*) */
	GC_S_GAME_STATE    = 0x12,
	/* double x, y, z; float yaw, pitch; u8 epoch. Snap, zero motion and fall
	 * distance, and echo the epoch in every MOVE from then on (T22). */
	GC_S_TELEPORT      = 0x13,
	/* u8 mode: 0 survival, 3 spectator (T26) */
	GC_S_GAME_MODE     = 0x14,

	GC_S_ENTITY_ADD    = 0x20,
	GC_S_ENTITY_MOVE   = 0x21,
	GC_S_ENTITY_REMOVE = 0x22,
	GC_S_ENTITY_EQUIP  = 0x23,
	GC_S_ENTITY_ANIM   = 0x24,

	GC_S_HEALTH        = 0x30,
	/* double mx, my, mz -- server-authoritative knockback. Applying this is
	 * mandatory: ignore it and the server moves you while you do not, which
	 * is permanent rubberband while being hit (T19). */
	GC_S_SELF_VELOCITY = 0x31,
	GC_S_INV_SET       = 0x32,
	GC_S_HELD_SLOT     = 0x33,
	GC_S_XP            = 0x34,
	/* u8 active -- while set the console applies vanilla's 0.2x movement, or
	 * the position stream diverges during every golden apple (T22). */
	GC_S_USE_STATE     = 0x35,

	/* u8 colour, char text[] -- the proxy folds to printable ASCII and
	 * strips section signs into that leading colour byte. */
	GC_S_CHAT          = 0x40,
	GC_S_ACTION_BAR    = 0x41,
};

/* ---- console -> server -------------------------------------------------- */
enum {
	/* u8 version, u32 blockmapHash */
	GC_C_HELLO      = 0x81,
	/* u32 token, echoed from GC_S_PING */
	GC_C_PONG       = 0x83,

	/* 20 Hz. double x, y, z; float yaw, pitch; u8 flags, u8 epoch.
	 * flags bit0 onGround, bit1 sprinting, bit2 sneaking. The proxy turns
	 * this into the right C03/C04/C05/C06 and the C0B sprint/sneak edges
	 * (T22) -- the console sends intent, not packets. */
	GC_C_MOVE       = 0x90,
	GC_C_DIG        = 0x91,
	GC_C_PLACE      = 0x92,
	GC_C_USE_ENTITY = 0x93,
	GC_C_USE_ITEM   = 0x94,
	GC_C_SWING      = 0x95,
	GC_C_HELD_SLOT  = 0x96,
	/* char text[] -- 100 chars max, rate-limited by the proxy (T22) */
	GC_C_CHAT       = 0x97,
	GC_C_ACTION     = 0x98,
};

/* GC_S_HELLO result */
enum {
	GCLINK_HELLO_OK        = 0,
	GCLINK_HELLO_VERSION   = 1,  /* GCLINK_VERSION mismatch          */
	GCLINK_HELLO_BLOCKMAP  = 2,  /* BLOCKMAP_HASH mismatch           */
	GCLINK_HELLO_BUSY      = 3,  /* another console already attached */
};

/* GC_S_GAME_STATE */
enum {
	GCLINK_GAME_LOBBY   = 0,   /* outside any map AABB */
	GCLINK_GAME_WAITING = 1,
	GCLINK_GAME_PLAYING = 2,
	GCLINK_GAME_ENDED   = 3,
};

/* No PING for this long means the link is dead even if TCP has not noticed.
 * The proxy sends one a second. */
#define GCLINK_PING_TIMEOUT_MS 8000

/* ---- payload accessors --------------------------------------------------
 * Byte-wise on purpose. PowerPC is big-endian so the byte order costs
 * nothing, but payload fields are packed with no regard for alignment and a
 * misaligned 32-bit load is a real problem here. These also keep the wire
 * layout visible at every call site. */

static inline void gc_put_u8(u8 *p, u8 v)  { p[0] = v; }
static inline void gc_put_u16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static inline void gc_put_u32(u8 *p, u32 v) {
	p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);  p[3] = (u8)v;
}
static inline u8  gc_get_u8(const u8 *p)  { return p[0]; }
static inline u16 gc_get_u16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static inline s16 gc_get_s16(const u8 *p) { return (s16)gc_get_u16(p); }
static inline u32 gc_get_u32(const u8 *p) {
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}
static inline s32 gc_get_s32(const u8 *p) { return (s32)gc_get_u32(p); }

#endif

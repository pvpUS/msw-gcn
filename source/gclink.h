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

/* Every position on this link is in the selected map's **local block space**:
 * the same spawn-relative coordinates the .mworld stores and World_GetBlock
 * takes, i.e. absolute server coordinate minus MAP_SELECT's origin. The
 * console never sees an absolute coordinate and never needs to; the proxy adds
 * the origin back on the way out. Blocks are s16 (whole blocks), entities and
 * the player are fixed-point x32 s32 (the 1.8 wire's own unit) or doubles.
 *
 * Block ids are engine global ids as generated into blockmap_gen.h. 0xFFFF is
 * air -- the engine stores air as -1, which does not survive a u16.
 */
#define GCLINK_AIR 0xFFFFu

/* Entity fixed-point: 32 units per block, straight off the 1.8 wire, and
 * angles as a byte turn (256 = 360 degrees). Kept in the wire's own units so
 * the proxy forwards rather than re-quantises; the console divides once. */
#define GCLINK_POS_SCALE  32.0
#define GCLINK_ANGLE_SCALE (360.0 / 256.0)

/* ---- server -> console --------------------------------------------------
 * Implemented by T3: HELLO, DISCONNECT, PING. The rest are the contract the
 * proxy (T6-T8) and the console (T11, T15, T22) build against. Layouts are
 * fixed here rather than in either implementation, because the two are written
 * months and one language apart and a silent disagreement about a field width
 * renders as garbage rather than as an error. */
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

	/* u8 mapIndex, s32 originX, originY, originZ, s32 selfEid.  [17 B]
	 * mapIndex indexes g_maps[]. The console loads its own embedded .mworld;
	 * the origin is the map's teleportLocation, which is what converts the
	 * server's absolute coordinates to this map's local block space. Sent
	 * again mid-session when the game ends and a new map starts. */
	GC_S_MAP_SELECT    = 0x10,
	/* { s16 x, y, z; u16 globalId } * n, n = payload / 8, n <= 1024.
	 * Local block coordinates; GCLINK_AIR clears. Apply through
	 * World_SetBlockDeferred and flush once per frame (T24) -- the join-time
	 * chunk diff arrives as a burst of these. */
	GC_S_BLOCK_SET     = 0x11,
	/* u8 state (GCLINK_GAME_*) */
	GC_S_GAME_STATE    = 0x12,
	/* double x, y, z; float yaw, pitch; u8 epoch.  [33 B]
	 * Snap, zero motion and fall distance, and echo the epoch in every MOVE
	 * from then on (T22). Local block space, feet position, GC angle
	 * convention -- the proxy has already converted. */
	GC_S_TELEPORT      = 0x13,
	/* u8 mode: 0 survival, 3 spectator (T26) */
	GC_S_GAME_MODE     = 0x14,

	/* s32 eid, u8 type (GCLINK_ENT_*), u8 flags (GCLINK_EFLAG_*),
	 * s32 x, y, z (fixed x32), u8 yaw, u8 pitch, u16 heldItem,
	 * u8 colour (0-15 MC colour code, 0xFF none), u8 nameLen, char name[].
	 * [24 B fixed + nameLen]  heldItem is an engine item id, GCLINK_AIR for
	 * none; the name is at offset 24 and is capped at 24 characters. */
	GC_S_ENTITY_ADD    = 0x20,
	/* { s32 eid, s32 x, y, z, u8 yaw, u8 pitch } * n, n = payload / 18.
	 * Absolute position every time, not a delta: the proxy already
	 * accumulates the 1.8 relative moves, and re-deriving them here would
	 * make a single dropped frame permanent. 20 Hz, sub-pixel deltas
	 * dropped. An eid the console does not know is a stale update -- ignore
	 * it, do not synthesise an entity. */
	GC_S_ENTITY_MOVE   = 0x21,
	/* { s32 eid } * n, n = payload / 4 */
	GC_S_ENTITY_REMOVE = 0x22,
	/* s32 eid, u8 slot (0 held, 1-4 armor), u16 itemId.  [7 B] */
	GC_S_ENTITY_EQUIP  = 0x23,
	/* s32 eid, u8 anim (GCLINK_ANIM_*).  [5 B] */
	GC_S_ENTITY_ANIM   = 0x24,

	/* float health -- half-hearts, 0..20. Hunger is deliberately absent: the
	 * plugin pins food and saturation at 20. */
	GC_S_HEALTH        = 0x30,
	/* double mx, my, mz -- server-authoritative knockback. Applying this is
	 * mandatory: ignore it and the server moves you while you do not, which
	 * is permanent rubberband while being hit (T19). */
	GC_S_SELF_VELOCITY = 0x31,
	/* { u8 slot, u16 itemId, u8 count, u16 meta } * n, n = payload / 6.
	 * slot is an *engine* index -- 0-8 hotbar, 9-35 storage, 36-39 armor
	 * (boots, legs, chest, helmet), matching Inventory_GetStackInSlot. The
	 * proxy has already translated out of the 1.8 window-0 numbering.
	 * count 0 empties the slot. */
	GC_S_INV_SET       = 0x32,
	/* u8 slot (0-8), absolute -- Inventory_ChangeCurrentItem is relative */
	GC_S_HELD_SLOT     = 0x33,
	/* float bar (0..1), s16 level, s32 total. level is the ranked elo. */
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
	/* u8 status (0 start, 1 abort, 2 stop), s16 x, y, z, u8 face.  [8 B] */
	GC_C_DIG        = 0x91,
	/* s16 x, y, z, u8 face, u8 curX, curY, curZ (sixteenths).  [10 B]
	 * The clicked block and face, exactly as C08 wants them -- the proxy
	 * fills in the held stack, which only it knows for certain. */
	GC_C_PLACE      = 0x92,
	/* s32 eid, u8 action (0 interact, 1 attack).  [5 B]
	 * The proxy rejects an ATTACK against an item / xp orb / projectile /
	 * self: the server *kicks* on those (T22). */
	GC_C_USE_ENTITY = 0x93,
	/* u8 action (0 start, 1 release) -- bow draw, eating, potion throw */
	GC_C_USE_ITEM   = 0x94,
	/* no payload */
	GC_C_SWING      = 0x95,
	/* u8 slot (0-8) */
	GC_C_HELD_SLOT  = 0x96,
	/* char text[] -- 100 chars max, rate-limited by the proxy (T22) */
	GC_C_CHAT       = 0x97,
	/* u8 action (GCLINK_ACTION_*) -- out-of-band intents that are not
	 * movement and do not deserve a message type each */
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

/* GC_S_ENTITY_ADD type. The proxy filters hard before this: every kill on
 * MegaSkywars spawns real mobs, armor stands and flying skulls as cosmetics,
 * and every projectile trails particles, so an unfiltered table thrashes
 * during a fight. Only these ever reach the console. */
enum {
	GCLINK_ENT_PLAYER   = 0,
	GCLINK_ENT_ITEM     = 1,   /* dropped item                            */
	GCLINK_ENT_ARROW    = 2,
	GCLINK_ENT_SNOWBALL = 3,
	GCLINK_ENT_PEARL    = 4,
	GCLINK_ENT_POTION   = 5,   /* splash potion                           */
	GCLINK_ENT_BOBBER   = 6,   /* fishing float -- Knockback III rod      */
	GCLINK_ENT_DRAGON   = 7,   /* the one mob the plugin spawns (T12)     */
};

/* Only PLAYER and DRAGON may be the target of GC_C_USE_ENTITY(attack); the
 * server kicks with "Attempting to attack an invalid entity" otherwise, so
 * T18's entity ray-trace has to skip everything else. */
static inline int gclink_ent_attackable(u8 type) {
	return type == GCLINK_ENT_PLAYER || type == GCLINK_ENT_DRAGON;
}

/* Which entities close a position gap over several ticks instead of snapping
 * to it. The same two types as above today, but a different question: that one
 * is about what the server lets you hit, this one is about how vanilla moves
 * things.
 *
 * `Entity.setPositionAndRotation2` (Entity.java:2013) takes a
 * posRotationIncrements argument and **ignores it**, calling setPosition
 * outright. Only EntityLivingBase (EntityLivingBase.java:2111) and
 * EntityOtherPlayerMP (EntityOtherPlayerMP.java:39) override it to store the
 * target and ease toward it. So a player and the dragon smooth; an arrow, a
 * snowball, a pearl, a potion, a bobber and a dropped item do not.
 *
 * Smoothing a projectile is not a small cosmetic error. The easing closes a
 * fraction of the gap per tick, but a projectile gets a fresh target every
 * tick, so it never closes -- it settles into trailing its true position by
 * several blocks and stuttering. The artifact scales with speed, which is why
 * a player at 0.2 blocks/tick looks fine and an arrow at 3 looks broken. */
static inline int gclink_ent_smoothed(u8 type) {
	return type == GCLINK_ENT_PLAYER || type == GCLINK_ENT_DRAGON;
}

/* GC_S_ENTITY_ADD flags, from the 1.8 entity metadata byte at index 0. */
enum {
	GCLINK_EFLAG_SNEAKING  = 1 << 0,
	GCLINK_EFLAG_SPRINTING = 1 << 1,
	GCLINK_EFLAG_INVISIBLE = 1 << 2,
	GCLINK_EFLAG_USING     = 1 << 3,  /* eating / drawing a bow / blocking */
};

/* GC_S_ENTITY_ANIM */
enum {
	GCLINK_ANIM_SWING = 0,
	GCLINK_ANIM_HURT  = 1,   /* -> hurtTime = maxHurtTime = 10, red flash */
	GCLINK_ANIM_DEATH = 2,
	GCLINK_ANIM_EAT   = 3,
};

/* GC_C_ACTION */
enum {
	GCLINK_ACTION_DROP_ITEM  = 0,   /* D-pad Down (T16)                   */
	GCLINK_ACTION_DROP_STACK = 1,
	GCLINK_ACTION_RESPAWN    = 2,   /* never needed here; see T26         */
	GCLINK_ACTION_RESYNC     = 3,   /* resend inventory, held slot, health*/
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

/* Entity angles arrive as the 1.8 wire's own byte turn, unconverted, so these
 * are where the convention change happens for everything except TELEPORT
 * (which the proxy converts, because it has to send doubles anyway).
 *
 * The byte is **signed**: Minecraft encodes pitch in [-90, 90] as [-64, 64],
 * and reading it unsigned would put every downward look at +180 degrees.
 *
 * Yaw is `180 - mc`, not `mc - 180`. Both agree at 0 and 180, which is why the
 * difference is easy to miss, but they are a reflection apart everywhere else:
 * Minecraft's yaw 90 faces -X and this engine's faces -X at +90, so the two
 * conventions run in opposite directions rather than being offset by half a
 * turn. Getting this backwards mirrors every entity's facing -- a player
 * walking west appears to moonwalk east -- and, once movement is being sent
 * (T22), mirrors the yaw the server is told about too. */
static inline float gclink_yaw(u8 b) {
	return 180.0f - (float)((s8)b) * (float)GCLINK_ANGLE_SCALE;
}
static inline float gclink_pitch(u8 b) {
	return -(float)((s8)b) * (float)GCLINK_ANGLE_SCALE;
}

/* Big-endian IEEE-754 off the wire. Read through the integer accessors and
 * memcpy'd rather than cast, because the payload is packed with no regard for
 * alignment and a misaligned float load on this CPU is a real fault, not a
 * slow path. */
static inline float gc_get_f32(const u8 *p) {
	u32 v = gc_get_u32(p);
	float f;
	__builtin_memcpy(&f, &v, sizeof f);
	return f;
}
static inline double gc_get_f64(const u8 *p) {
	u64 v = ((u64)gc_get_u32(p) << 32) | (u64)gc_get_u32(p + 4);
	double d;
	__builtin_memcpy(&d, &v, sizeof d);
	return d;
}

#endif

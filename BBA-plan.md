# Mega Skywars GameCube — Broadband Adapter Multiplayer

## Context

`msw-gcn` today is an offline voxel engine with a faithful Minecraft 1.8.9 physics port. It boots to a text menu, loads one of 33 `.mworld` maps embedded in the DOL, and lets you walk around, mine, place and pick up drops. There is no networking, no entity system beyond dropped items, and no text rendering beyond digits 0–9.

The goal is to connect it to the live **MegaSkywars Spigot 1.8.8 server** over the GameCube Broadband Adapter, so a real GameCube can join a real game — and **fight in it**.

The hard parts (Mojang auth, AES encryption, zlib, packet parsing, and the whole 1.8 protocol state machine) stay off the console. A **Node.js proxy** on the LAN speaks Minecraft 1.8.9 to the server and a small, GameCube-friendly binary protocol ("GCLink") to the console over one plaintext TCP socket.

**Decisions already made:**
- Proxy: Node.js + `minecraft-protocol` (handles auth/encryption/compression/serialization)
- World data: console loads its **own embedded `.mworld`**; proxy streams only block **deltas**
- Two milestones: **spectate a live game**, then **play one**. Combat is in scope and is the headline goal.
- Test target: **Dolphin emulated BBA**, proxy on the same PC
- Scope: overworld only; players, projectiles, and the **ender dragon** (the plugin spawns one per game)
- **Full fluid support** (non-solid water/lava + swim physics), not a workaround
- **On-screen canned-command menu** for `/join` / `/team` / `/start` — no GUI window-click protocol
- **Chat is hidden by default**, shown only on demand
- **No** audio, **no** particles, **no** armor rendering, **no** border geometry. **Do** render the red hit flash.

---

## What we found — the alignment gaps

| Area | Current state | Gap for multiplayer |
|---|---|---|
| **World mutation** | **Done.** Chunked storage (`WORLD_CHUNK_XZ 16`, `world.h:23`), per-chunk display lists, `World_GetBlock` (`world.h:110`), `World_SetBlock` (`world.h:118`). | Only the **batching** half is missing — `World_SetBlock` (`world.c:697-725`) re-meshes up to 3 chunks *synchronously inside the call*. There is no `World_FlushRemesh`. A join-time chunk diff is hundreds of blocks in one batch. → **T24** |
| **Memory** | Heap ≈ **15.2 MB** (24 MB MEM1 − 7.2 MB DOL − FBs − FIFO). `mega_aegis` already peaks at **≈13.3 MB** at load. `atlas.tpl` permanently costs 5.25 MB of `.rodata`. There is **no memory or frame-time accounting at all**. | Entities, a network stack and a font do not fit without reclaiming memory first — and nothing is measurable today. → **T4**, **T27** |
| **Block IDs** | 533 "global ids" = 0-based line number in `_blockids.txt` (Bukkit `Material[:data]` strings). Metadata is *baked into the id*, so `g_blockShape[]`/`g_blockParam[]` are static per id. | Maps cleanly to 1.8's `id<<4\|meta` state ids, but no such table exists, and `_blockids.txt` lives **outside the repo**. Palette is the union of the 31 map scans, so gameplay-placed blocks are missing: `LAVA`, `STATIONARY_LAVA`, `TNT`, **and `WATER:0` + `WATER:8`** — verified, the `g_blockVariant` `WATER` row is `{-1,462,469,…,-1,475,…}`. A placed water bucket produces exactly `WATER:0`. → **T1** |
| **Combat** | **Absent.** `World_RayTrace` is blocks-only; `Interact_Tick` (`interact.c:278-300`) has no attack path. `Player_Damage` (`player.h:70`) has no attacker. `ItemDef` (`items.h:26-32`) has no attack damage, durability or use action, and only 4 tools are defined. No knockback, no `hurtTime`, no swing animation. | Everything must be built. → **T17**, **T18**, **T19** |
| **Fluids** | **Water is a solid opaque cube.** Verified: `g_blockShape[382..397] == SHAPE_CUBE`, `g_blockOpaque == 1`. The player walks *on top of* water. `moveEntityWithHeading` (`player.c:313`) has only the ground/air branch. | Reads to the server as hovering → *"Flying is not enabled"* kick. The water bucket is in the kit and MLG-water is a core skywars mechanic. → **T21** |
| **Input abstraction** | `Player_Look`/`Player_Tick` read `PAD_*` **directly** (`player.c:422-456`). Sneak is a function-local `int` that never reaches the struct. | Nothing else can supply input — no remote entity, no replayed script, no network mode. → **T16** |
| **Protocol conformance** | Nothing exists yet. | A set of *mandatory* 1.8 behaviours whose omission causes kicks and silent drops, not cosmetic bugs. → **T22** |
| **Command entry** | None. | Joining normally means clicking a 54-slot GUI. There is no way to type from a pad. → **T23** |
| **Text** | `hud.c` has a 3×5 bitmap font for **digits only**. `menu.c` uses the libogc console (incompatible with the GX pass). | Blocks chat, nametags, the command palette, the action bar, the perf HUD. → **T2** |
| **Entities** | Only `entityitem.c` — a hardcoded dropped-item pool. No entity base, no id, no type, no player model. | Everything must be built. → **T9** |
| **Networking** | `LIBS := -logc -lm`. Zero sockets, zero threads. | `libbba.a` **is installed**, and a complete working BBA socket example is vendored at `.gc-examples/devices/network/sockettest/`. → **T3** |
| **Physics / tick** | Faithful 1.8.9 `Entity.moveEntity` port at a fixed **20 Hz accumulator** with render interpolation (`main.c:156-249`). | Already exactly right. Free head start — `Player_Tick` is the natural place to hang the 20 Hz movement packet. |
| **Conventions** | Local yaw 0 faces −Z; positive pitch = up. | MC yaw 0 faces +Z, positive pitch = down. Needs `mc_yaw = yaw + 180`, `mc_pitch = -pitch` at the packet boundary. |
| **Items** | `ItemStack.item` is a global **block** id, or an atlas tile ≥ 533 for flat items. One flat item exists (diamond sword, tile 634). | Needs the kit (below). 389 free atlas tiles remain. → **T13** |

### Already built, and not to be re-planned

The working tree contains, in addition to chunked mutable world storage:
- [`interact.c`](source/interact.c) — the full `PlayerControllerMP` break-progress state machine, `blockHitDelay`, 0–9 crack overlay stages, drop tables, and `Block.onBlockPlaced` metadata derivation for stairs/slabs/pillars/torches/chests/ladders/gates/anvils.
- [`world.c`](source/world.c) `World_RayTrace` — the exact vanilla grid walk, which already ignores water (`ray_ignores`, `world.c:1115-1117`).
- [`inventory.c`](source/inventory.c) — a full `InventoryPlayer` port including `Container.slotClick`.
- [`items.c`](source/items.c) — tool tiers, `getStrVsBlock`, `canHarvestBlock`, `getPlayerRelativeBlockHardness`.
- [`entityitem.c`](source/entityitem.c) — dropped-item entities with swept-AABB physics, merging and pickup.
- [`hud.c`](source/hud.c) — hearts, hotbar, the inventory screen, on a dedicated `GX_VTXFMT1`.

### The decisive simplification

`MegaSkywars/src/me/pvpus/megaskywars/MapManager.java:33-63` hardcodes **every map's exact world origin** (`teleportLocation`), and all 31 maps live in the single world `"world"` at fixed offsets ≥1800 blocks apart:

```java
maps.put("hontori", new MapStatus(plugin,"hontori",2,..., new Location(Bukkit.getWorld("world"), 3850, 80, -2149), ...));
```

The `.mworld` scans are **spawn-relative to exactly those origins**. So the proxy identifies the map from the player's absolute coordinates alone — no chunk fingerprinting, no plugin changes — and converts absolute → map-grid with a subtraction. Combined with **no chests and no generators on any map** (the kit is the entire economy) and a map reset each game, this means the console needs **no chunk decoder at all**.

### The kit — the entire item economy

One kit, no classes. `KitManager.java:37-122`, plus armor hardcoded in `MapStatus.startGame:935-963`:

diamond sword (Sharpness II) · fishing rod (**Knockback III**) · bow (Punch II) · diamond axe (Fire Aspect I + Efficiency III) · diamond pickaxe · 20 golden apples · water bucket · lava bucket · **192 stone** · 64 snowballs · 24 ender pearls · 64 arrows · 5 splash speed · 5 splash regen · 2 fire resistance · **full enchanted diamond armor**.

---

## Architecture

```
GameCube (msw-gcn.dol)          PC / Pi                          Spigot 1.8.8
  BBA, DHCP                     Node.js proxy
  one plaintext TCP socket ───► GCLink server :25566
                                minecraft-protocol client ─────► msw server :25565
  no auth, no zlib, no AES      auth / AES / zlib / varints      online-mode
  loads hontori.mworld          map id + origin from coords      protocol 47
  applies BLOCK_SET deltas      diffs S21/S26 chunks vs its
  renders entities              own copy of the .mworld
  sends intent, not packets     owns the 1.8 protocol state machine
```

**GCLink** — big-endian (PowerPC-native), length-prefixed, no varints:

```
u16 length | u8 type | payload[length-1]
```

Server → console: `HELLO`, `MAP_SELECT`(map idx, origin xyz, self entity id), `BLOCK_SET`(batched `s16 x,y,z, u16 globalId`), `ENTITY_ADD/MOVE/REMOVE/EQUIP/ANIM`, `HEALTH`, `SELF_VELOCITY`, `INV_SET`, `HELD_SLOT`, `CHAT`, `ACTION_BAR`, `XP`, `GAME_MODE`, `USE_STATE`, `TELEPORT(…, u8 epoch)`, `GAME_STATE`, `PING`.

Console → server: `HELLO`, `MOVE`(20 Hz, `…, u8 flags, u8 epoch` — flags bit0 onGround, bit1 sprinting, bit2 sneaking), `DIG`, `PLACE`, `USE_ENTITY`, `USE_ITEM`, `SWING`, `CHAT`, `HELD_SLOT`, `ACTION`, `PONG`.

> **Not in GCLink:** `TITLE` and sidebar `SCOREBOARD`. MegaSkywars sends **no titles, no boss bar, and no sidebar scoreboard**. Its real UI surfaces are chat, the action bar (raw `S02` with position byte 2), a tab-list health objective, XP level = elo, and `S3E Teams` colour prefixes on nametags.

Both sides validate a **palette hash** in `HELLO` so a stale `blockmap` fails loudly instead of rendering garbage.

**Design decision — the protocol state machine lives in the proxy.** GCLink carries *intent* (`MOVE` with flag bits, `USE_ENTITY`, `USE_ITEM`, `CHAT`); the proxy translates it into correct 1.8 packet sequences. JS iterates in seconds; a DOL rebuild is ~90. Only what needs physics state stays console-side.

---

## Task breakdown

Tasks within a phase are parallelizable unless a dependency is noted. Each is scoped to be handed to one agent.

> **Retired task numbers.** Earlier revisions of this plan had a **T5a/T5b** (chunked storage + `World_SetBlock`) — both are implemented; only the batching half survives, as **T24**. **T20** (armor rendering) and **T25** (border geometry) were cut by decision; see "Cut by decision" for what was kept of each. Numbers are not reused.

### Housekeeping (do first, not an agent task)

There are **1,041 uncommitted insertions across 11 files plus 6 untracked new source files** (`hud.*`, `inventory.*`, `helditem.*`, `interact.*`, `items.*`, `entityitem.*`). This is the chunked mutable world, the mining/placing path, the inventory and the HUD — **the foundation every task below builds on.** **Commit it before starting.**

---

### Phase 0 — Foundations (fully parallel)

#### T1 — Block/item ID bridge
**Goal:** a single generated, versioned mapping between 1.8 block states and the engine's global ids, shared by console and proxy.
**Files:** new `tools/gen_blockmap.py`, new `data/blockids.txt` (vendored copy), new `source/blockmap_gen.h`, new `proxy/blockmap.json`.
**Details:**
- Copy `_blockids.txt` (currently only at `C:\Users\awt12\Downloads\download (1)\BlockScans\_blockids.txt`, 533 lines) **into the repo** so builds are reproducible. `tools/build_atlas.py` and `tools/compress_worlds.py` should default to the vendored path.
- **Extend the palette** with blocks the maps never contained but games do. Verified absent: `LAVA:0..15`, `STATIONARY_LAVA:0..15`, `TNT`, **`WATER:0`**, **`WATER:8`**. The last two matter most — meta 0 is what a water bucket places and meta 8 is falling water. Cross-check against the ~140 `Material.*` constants the plugin references (`grep -rhoE "Material\.[A-Z_0-9]+" src`).
- **Append-only past id 532.** Appending keeps existing ids stable; anything else forces a full regeneration of the atlas and all 33 `.mworld` files.
- Add `MAT_LAVA` to the material enum, and `Block_IsLiquid(id)` to `items.h` (`material == MAT_WATER || material == MAT_LAVA`) — T21 depends on it.
- `ENDER_PORTAL` is **not** needed; see the Lights Out entry under "Cut by decision".
- Emit `blockmap.json`: `stateId (= id<<4|meta) → globalId`, using MCP-919's `Block.registerBlocks()` (`src/minecraft/net/minecraft/block/Block.java`) as the authority for `Material name → numeric id`.
- Also emit a **`1.8 itemId(+damage) → engine item`** table for T13. Splash potions key on damage value.
- Emit `source/blockmap_gen.h` with `#define BLOCKMAP_HASH 0x…` (FNV-1a over the id list) for the `HELLO` handshake.
- Unmapped states → a sentinel global id rendered as a magenta/black placeholder **for palette diagnostics only**, and logged proxy-side. See T7 for why live block sets must be dropped instead.
**Done when:** `blockmap.json` round-trips every id in `data/blockids.txt`, and the plugin's full `Material` set is covered or explicitly listed as unsupported.

#### T2 — ASCII bitmap font + `Hud_DrawString`
**Goal:** draw arbitrary strings in the GX pass. **Unblocks T10, T23 and T27 entirely — build it early.**
**Files:** new `data/font.tpl`, new `tools/build_font.py`, `source/hud.c`, `source/hud.h`.
**Details:**
- Source glyphs from MCP-919's `assets/minecraft/textures/font/ascii.png` (128×128, 16×16 grid of 8×8 glyphs).
- Bake as a **separate TPL in I4 or IA4 format** (≈8–16 KB) rather than atlas tiles — the atlas only has 389 free tiles and this would eat 256 of them. The Makefile already has a `%.tpl.o` rule, so dropping the file in `data/` is enough.
- **No new vertex format.** `HUD_FMT` (`GX_VTXFMT1`: `POS_XY/F32 + CLR_RGBA/RGBA8 + TEX_ST/F32`, `hud.c:12`) is already exactly a glyph quad — just bind a second `GXTexObj`. Formats 0/1/2 are taken by world/HUD/helditem.
- Derive MC's variable glyph widths at bake time by scanning each cell's rightmost non-transparent column; emit `source/font_gen.h`.
- API mirroring `FontRenderer`: `int Hud_DrawString(const char *s, int x, int y, u32 rgba)`, `int Hud_StringWidth(const char *s)`, plus a shadowed variant. Respect the existing GUI scale (`hud.c:165-169`) and the GX state save/restore contract (`World_SetupRenderState()` on exit).
- Support the 16 MC colour codes as a per-span colour — the proxy strips section signs.
- Retire `DIGITS[10][5]` and `draw_number`/`draw_count` (`hud.c:68-110`) in favour of `Hud_DrawString`.
**Done when:** a test string renders correctly at GUI scales 1–3 in a Dolphin screenshot.

#### T3 — BBA transport + GCLink framing
**Goal:** the console can open a socket, exchange framed messages, and survive disconnects.
**Files:** `Makefile`, new `source/net.c`, new `source/net.h`, new `source/gclink.h` (shared message-type enum), `source/main.c`.
**Details:**
- `LIBS := -lbba -logc -lm`. Copy the idiom verbatim from `.gc-examples/devices/network/sockettest/source/sockettest.c`: `if_config(localip, netmask, gateway, TRUE, 20)` then `net_socket`/`net_connect`/`net_recv`/`net_send`.
- **Single-threaded, non-blocking** (`net_fcntl` `O_NONBLOCK`). Do not add an LWP thread — GX state is not thread-safe and there is no locking discipline anywhere in the codebase. Draining a socket costs microseconds against a 16 ms frame budget.
- `if_config` blocks for up to 20 retries — call it once in `main()` **before** the menu, with on-screen status via the libogc console (which `menu.c` already uses).
- Poll point: `main.c:157`, right after `PAD_ScanPads()` — 60 Hz, 3× the tick rate. Send point: inside the `while (accum >= TICK_US)` loop at `main.c:238-247`.
- Ring buffer + reassembly (TCP will split frames), `Net_Poll()` yielding whole messages, plus a reconnect state machine and an on-screen connection indicator.
- Server address: a `#define` for now; a menu entry later.
**Done when:** the console connects to a stub Node echo server, completes `HELLO`, and displays a live RTT.

#### T4 — Memory reclamation
**Goal:** buy back headroom before entities spend it. `mega_aegis` peaks at ~13.3 MB of a ~15.2 MB heap.
**Files:** `README.md` (atlas bake command), `tools/build_atlas.py`, `source/world.c`, `source/main.c`.
**Details:**
- Re-bake `data/atlas.tpl` from `colfmt=6` (RGBA8, 5.25 MB) to **`RGB5A3`** (≈2.6 MB, saves ~2.6 MB) — 16-bit with 3-bit alpha, fine for the alpha-test-only world shader. `CMPR` (≈0.7 MB, saves ~4.5 MB) is the emergency lever, but expect DXT1 block artifacts on 16×16 pixel art; RGB5A3 is the safe default.
- Trim the display-list allocation to `dlLen` after `GX_EndDispList` — it is currently sized to an upper bound and never shrunk.
- Before re-baking, capture a **regression baseline**: all 33 maps load, screenshots on `mega_aegis`, `hontori`, `sky_carnival` (the 2-byte-palette map), and `model_gallery` with `MODEL_TEST_MODE 1`, plus `MODEL_TEST_MODE` collision wireframes.
- The boot-time arena report widens into **T27**, below.
**Done when:** peak heap on `mega_aegis` is **≤ 11 MB**, leaving ≥ 4 MB for entities, network and font — with no visible texture regression against the baseline screenshots.

#### T27 — Perf HUD *(depends on T2; do it before T9)*
**Goal:** make every budget below checkable instead of aspirational. There is **no memory or frame-time accounting at all** today.
**Files:** `source/hud.c`, `source/main.c`, `source/world.c`.
**Details:** a toggleable overlay drawn with `Hud_DrawString`, reporting:
- free heap (`SYS_GetArena1Lo/Hi`) — **the number that governs everything**
- frame time and tick time in ms, rolling max as well as mean
- draw batches per frame, and vertex-array high-water marks (`clrCount`/`texCount` against the 65535 `GX_INDEX16` ceiling)
- live entity count, dirty-chunk count, remesh ms

~80 lines. See "Performance" below for the budgets it enforces.
**Done when:** the overlay reports live heap and frame time on `mega_aegis`, and the T4 numbers are read off it rather than from a boot printf.

---

### Phase 1 — Mutable world, finished

#### T24 — Deferred, batched re-mesh *(the un-built half of the world work)*
**Goal:** `World_SetBlock` (`world.c:697-725`) re-meshes up to 3 chunks **synchronously inside the call**, then flushes vertex arrays. The proxy's join-time chunk diff is hundreds of blocks in one GCLink batch; today that is a multi-second freeze.
**Files:** `source/world.c`, `source/world.h`.
```c
/* Record the edit and mark the owning chunk (+ seam neighbours) dirty WITHOUT
 * re-meshing. Same return contract as World_SetBlock. */
int World_SetBlockDeferred(World *w, int bx, int by, int bz, int id);

/* Re-mesh at most `maxChunks` dirty chunks, nearest-to-(px,pz) first.
 * Returns the number still dirty. Call once per rendered frame. */
int World_FlushRemesh(World *w, int maxChunks, double px, double pz);
```
**Details:**
- Add `u8 *chunkDirty` (`cxCount*czCount` bytes — trivial) plus a dirty count to `World`.
- Keep `World_SetBlock` as the correct synchronous path for single-player `interact.c`; it becomes `World_SetBlockDeferred` + an immediate targeted flush.
- Nearest-first ordering matters: on join, the chunks under the player must resolve in the first few frames.
- One `flush_vertex_arrays(w)` per flush call, not per block.
- Watch `dedup_tex`: append-only and `GX_INDEX16`-addressed, ceiling 65535. New tiles appearing post-load must append, not silently overflow.
**Done when:** a 2000-block `BLOCK_SET` burst applied in one frame drops no frames and converges within ~30 frames, at **≤ 4 ms per flushed chunk**, with no arena growth.

---

### Phase 2 — Proxy

#### T6 — Proxy core: auth, connect, GCLink server
**Goal:** a running proxy that authenticates, joins the server, and serves one console.
**Files:** new `proxy/` (package.json, `index.js`, `gclink.js`, `config.json`).
**Details:**
- `minecraft-protocol` client, `version: '1.8.9'`, `auth: 'microsoft'`. Handles S46 compression and encryption transparently. Note the **ranked** queue requires ViaVersion protocol 47 — the proxy satisfies this; casual has no version check either way.
- TCP server on `:25566` for the console. One console per instance; reject a second.
- Keepalives both ways: answer the server's `S00 KeepAlive` with `C00`, and run an independent GCLink `PING`/`PONG` so the console can show RTT and detect a dead link.
- Handle `S40 Disconnect` and `S02` login-phase failures with a clear GCLink `DISCONNECT(reason)`.
- **`--record` / `--replay` mode**: dump the GCLink byte stream to a file and replay it into the console without a live server. This is the highest-leverage thing in the whole plan for console-side iteration — build it early.
**Done when:** the proxy joins the real server as your account, and `--record` produces a replayable capture.

#### T7 — Proxy world translation: map identification + block deltas *(depends on T1, T6)*
**Goal:** the console's local map matches the server's live map exactly.
**Files:** `proxy/mapdb.js`, `proxy/mworld.js`, `proxy/world.js`.
**Details:**
- Transcribe the 31 map origins from `MapManager.java:33-63` into `proxy/mapdb.json` (name → `{x,y,z}` + the console's `g_maps[]` index).
- **Verify the scan origin assumption**: confirm that a `.mworld` grid coordinate `(gx,gy,gz)` corresponds to absolute `(gx+minx+originX, …)`. Check one distinctive block per map against the live server before trusting it. If there is a constant offset, record it per map in `mapdb.json`.
- On `position` / `login`, pick the nearest map origin within ~250 blocks → send `MAP_SELECT(idx, origin)`. Outside any map (hub/lobby) → `GAME_STATE(LOBBY)`.
- Port `compress_worlds.py`'s decoder to JS (`mworld.js`) — ~60 lines: LZSS + column/run walk, and the Python file is a complete, round-trip-verified spec.
- On `map_chunk` / `map_chunk_bulk` inside the map AABB, diff the received sections against the decoded `.mworld` and emit `BLOCK_SET` only for differences. This correctly handles joining a game already in progress. Route these through `World_SetBlockDeferred` (T24).
- On `block_change` / `multi_block_change`, translate `stateId → globalId` via `blockmap.json` and emit `BLOCK_SET`. Coalesce per tick into batches.
- **Drop unmapped block states outright** rather than emitting T1's magenta placeholder into the live world. A placeholder appearing 20×/sec would be worse than nothing — this is also what makes the unsupported Lights Out mode harmless.
- Drop everything outside the map AABB.
**Done when:** joining a game already 2 minutes in reproduces the exact current map state, verified by comparing a handful of coordinates.

#### T8 — Proxy entity + game-state translation *(depends on T6)*
**Goal:** everything else the console needs, in GCLink form.
**Files:** `proxy/entities.js`, `proxy/chat.js`, `proxy/state.js`.
**Details:**
- **Entities:** `named_entity_spawn` (S0C, players) → `ENTITY_ADD(PLAYER)`; `spawn_entity` (S0E objects) → arrows, snowballs, ender pearls, splash potions, fishing bobbers; `spawn_entity_living` (S0F) → **ender dragon only**, ignore all other mobs; `entity_destroy` (S13); `rel_entity_move`/`entity_look`/`entity_move_look`/`entity_teleport` (S14–S18) → `ENTITY_MOVE`; `entity_velocity` (S12); `entity_equipment` (S04) → `ENTITY_EQUIP`; `animation` (S0B) → swing/hurt.
  Wire encodings: absolute positions are **fixed-point ×32 int32**, relative moves are **int8 ×32**, yaw/pitch are **bytes ×(256/360)**.
- **Entity cap 128**, not 64, with nearest-N eviction — 16 players + arrows + snowballs + a 192-stone death drop overflows 64.
- **Filter cosmetic entities aggressively.** Every kill spawns real mobs, armor stands and flying skulls tagged `cosmetic`; every projectile gets a per-tick particle trail. Without a type filter the table thrashes during a busy fight.
- Rate-limit entity updates to 20 Hz and drop sub-pixel deltas.
- Names from `player_info` (S38); team colours from `S3E Teams`, carried on `ENTITY_ADD`.
- **Chat:** parse the S02 JSON chat component to plain text, fold to printable ASCII, strip `§` codes into a per-line colour byte. `S02` with **position byte 2** → `ACTION_BAR`, not `CHAT`.
- `S06 Update Health` → `HEALTH`; `S12` where `entityId == selfEid` → `SELF_VELOCITY`; `S1F SetExperience` → `XP` (level = elo); `S2B ChangeGameState` reason 3 → `GAME_MODE`; `S2F`/`S30` → `INV_SET`; `S09` → `HELD_SLOT`; `S08 PlayerPosLook` → `TELEPORT` (see T22 for the mandatory `C06` reply and the epoch).
- **Do not translate** `S45 Title` or `S3B/S3C/S3D` scoreboard — the plugin never sends them.
- Apply the convention conversion at this boundary: `gc_yaw = mc_yaw - 180`, `gc_pitch = -mc_pitch`.
**Done when:** a `--record` capture of a full game replays into a coherent GCLink stream.

---

### Phase 3 — Console rendering (Milestone 1: spectate)

#### T9 — Entity system + player model *(depends on T3, T17, T27)*
**Goal:** see other players.
**Files:** new `source/entity.c`, `source/entity.h`, new `data/steve.tpl`, `source/main.c`.
**Details:**
- Fixed array of **128** entity slots + a small id→slot hash. Per entity: type, position (prev/current for interpolation), held item, name, flags, and an embedded **`Pose`** (T17) rather than ad-hoc animation fields.
- Skip `selfEid` when rendering.
- Interpolate with the existing pattern — `Player_GetViewMatrix`'s `alpha` (`main.c:249`) is the model to copy.
- **Player model:** 6 boxes (head, body, 2 arms, 2 legs) with real Steve UVs. Ship the 64×32 skin as its own `data/steve.tpl` rather than atlas tiles — the atlas's 16×16 cell grid cannot express a skin's UV layout. (The existing `SKULL_*_TILE` 631–633 head tiles are *not* reusable for a full body.)
- Limb swing, sneak pose and arm swing all come from `Pose`.
- Render the entity's held item using `helditem.c`'s existing `draw_block`/`draw_flat` paths.
- Needs a new `GX_VTXFMT3` — formats 0/1/2 are taken by world/HUD/helditem.
**Done when:** replaying a recorded game shows other players moving and animating in the right places, **128 entities hold 60 fps**, and the entity table's fixed allocation is reported by T27 at boot.

#### T10 — Nametags, chat, action bar *(depends on T2, T9)*
**Goal:** the HUD reads like a real server.
**Files:** `source/hud.c`, `source/hud.h`, `source/entity.c`.
**Details:**
- Billboarded nametag above each player entity, depth-tested, team-coloured, scaled by distance. **Hard requirement: distance-cull past ~48 blocks**, and cap to the nearest ~16 regardless — this is the frame budget's main lever. Batch all glyphs of a frame into **one `GX_Begin`** rather than one per string.
- **Chat overlay, hidden by default** — see the "Chat visibility" block under T23. A ~50-line scrollback ring, drawn only while the chat button is held, plus an unread indicator near the hotbar.
- **Action bar** — one centred line above the hotbar, **always on**. This is where anything the player must not miss belongs.
- XP-level readout (= ranked elo).
- **Do not build** `TITLE` rendering or a sidebar scoreboard; the plugin sends neither.
- `Hud_Draw` currently takes only `Player *`; it will need the entity list and a chat/HUD state struct.
**Done when:** nametags, the action bar and on-demand chat are legible in Dolphin screenshots at 480p, with **nametags at 128 entities costing ≤ 2 ms/frame**.

#### T11 — Wire it together: spectate mode *(depends on T3, T7, T9, T24)*
**Goal:** **Milestone 1 complete** — boot, connect, spectate a live game.
**Files:** `source/main.c`, `source/menu.c`.
**Details:**
- New top-level mode alongside the offline map menu: connect → wait for `MAP_SELECT` → `World_Load` the named map → run a network frame loop.
- Camera: reuse the existing free-fly `Camera` (`camera.c`) as the spectator camera. **Do not send movement yet** — that is Milestone 2, and it carries all the kick risk.
- Drain `Net_Poll()` at 60 Hz; apply `BLOCK_SET` via `World_SetBlockDeferred`; call `World_FlushRemesh` once per frame; feed `ENTITY_*` into the entity table.
- Handle mid-session `MAP_SELECT` (game ends, new map) by tearing down and reloading the world.
- Disable `HUD_DEMO_ITEMS` (`main.c:37`) in network mode.
- Because Dolphin input injection is unreliable, add a **compile-time autoload hook** for network mode (the same pattern as `TEST_AUTOLOAD` at `main.c:350`) rather than menu navigation.
**Done when:** the GameCube in Dolphin shows a live MegaSkywars game — correct map, players moving, blocks breaking, chat on demand — at 60 fps.

#### T12 — Ender dragon *(depends on T9)*
**Goal:** render the one mob the plugin spawns (`MapStatus.java:1076`).
**Files:** `source/entity.c`, new `data/dragon.tpl`.
**Details:** A boxy approximation — body, head, neck segments, two wings, tail — driven by `ENTITY_MOVE`. Keep the polygon count low; it is atmospheric, not a gameplay target. A simplified silhouette is acceptable and preferable to a faithful 30-part model.

---

### Phase 4 — Playable (Milestone 2)

Everything in this phase either sends input or reacts to the server's authority over it. **T16, T21 and T22 gate the rest.**

#### T16 — Input capture struct *(keystone; blocks T14, T18, T19, T22, T23)*
**Goal:** all gameplay `PAD_*` reads move behind one struct, so input can be gated, replayed, and turned into packets.
**Files:** new `source/input.c`, `source/input.h`; `source/player.c`, `source/player.h`, `source/interact.c`, `source/main.c`.
**Why first:** `player.c:422-456` reads `PAD_SubStickX`/`PAD_StickX`/`PAD_ButtonsHeld`/`PAD_TriggerR` **directly inside** `Player_Look`/`Player_Tick`, and sneak is a function-local `int` that never reaches the struct. Nothing else can supply input.
```c
typedef struct {
    float moveForward, moveStrafe;  /* digitised to -1/0/+1 (MovementInputFromOptions) */
    float dYaw, dPitch;             /* degrees, applied per render frame */
    u8 jump, sneak, sprintHeld;
    u8 attackHeld, useHeld;         /* left/right mouse */
    u8 attackEdge, useEdge;         /* pressed this frame (clickMouse) */
    s8 hotbarDelta, hotbarSet;      /* scroll / absolute 0-8, -1 = none */
    u8 dropItem, openInv, showChat;
} PlayerInput;

void Input_Sample(PlayerInput *in, int chan);  /* the only gameplay PAD_* reader */
void Input_Clear (PlayerInput *in);            /* replaces the `frozen` flag     */
```
**Details:**
- `Player_Look(Player*, float dYaw, float dPitch)`, `Player_Tick(Player*, const World*, const PlayerInput*)`, `Interact_Tick(…, const PlayerInput*)`. `main.c` passes a cleared input while `invOpen`, preserving today's "still simulate gravity" semantics exactly.
- **Keep the ±1 digitisation** (`player.c:438-444`) — it is what reproduces keyboard-exact walk speed, and it is the single strongest anti-rubberband measure in the codebase.
- Add to `Player`: `sneaking`, `serverSprint` (last sprint state sent, T22), `gameMode`, `inWater`, `inLava`, `itemInUse`, `itemInUseCount`, and an embedded `Pose` (T17).
- Button map: `L` attack/dig, `Y` use/place, `R` sprint, `A` jump, `B` sneak, `X` inventory, D-pad L/R hotbar (all as today); **D-pad Up** = command palette (T23), **D-pad Down** = drop item, **`Z`** reclaimed from the debug freecam → **hold to show the chat log** (T23). `Start` = network pause menu. That is every input assigned, with no spares — any further binding must displace one of these.
- `Input_Sample` owns the 10-tick `leftClickCounter` miss lockout (`Minecraft.clickMouse`) — it gates attacks against blocks *and* entities, so it belongs here rather than in `interact.c`.
**Done when:** offline play behaves identically (same walk speed, dig timings, inventory behaviour), and a scripted `PlayerInput` sequence drives the player with `INTERACT_TEST_MODE` (`main.c:75`) deleted in its favour.

#### T17 — `Pose`: shared living-entity animation state *(depends on T16; blocks T9, T19)*
**Goal:** one animation state block used by the local player *and* remote entities, so swing/hurt/limb-swing exist once.
**Files:** new `source/pose.c`, `source/pose.h`; `source/player.h`, `source/helditem.c`, `source/entity.h`.
```c
/* The RendererLivingEntity-facing slice of EntityLivingBase */
typedef struct {
    float limbSwing, limbSwingAmount, prevLimbSwingAmount;
    int   swingProgressInt;      /* 0..6, getArmSwingAnimationEnd() = 6 */
    u8    isSwingInProgress;
    int   hurtTime, maxHurtTime; /* 10 on hit -> red flash + camera tilt */
    float attackedAtYaw;         /* degrees, relative to rotationYaw */
    float renderYawOffset, prevRenderYawOffset, headYaw, prevHeadYaw;
    u8    sneaking, sprinting, onGround;
} Pose;

void  Pose_Tick (Pose*, double dx, double dz, float yaw);
void  Pose_Swing(Pose*);
void  Pose_Hurt (Pose*, float attackerRelYaw);
float Pose_SwingProgress(const Pose*, float alpha);
```
**Details:**
- `Pose_Tick` is the `RendererLivingEntity` limb update: `f = min(sqrt(dx²+dz²)*4, 1)`, `limbSwingAmount += (f - limbSwingAmount)*0.4`, `limbSwing += limbSwingAmount`.
- `swingProgressInt` counts 0→6 then clears `isSwingInProgress`.
- `helditem.c`'s bob phase is a **file-static** (`helditem.c:149-157`) — move it onto `Pose` and drive vanilla's `transformFirstPersonItem` arc from `Pose_SwingProgress` (`sin(sqrt(f)*PI)` translate, `sin(sqrt(f)*PI)*-20` rotate), keeping the existing walk bob as the idle component.
**Done when:** attacking produces a first-person swing arc completing in 6 ticks, and `Pose` compiles as a member of both `Player` and a stubbed `Entity`.

#### T21 — Fluids: liquid collision + water/lava movement *(depends on T1; blocks T22)*
**Goal:** stop walking on water. **This is the highest-probability source of server kicks**, and the cheapest fix in the plan.
**Files:** `source/world.c`, `source/items.h`, `source/player.c`, `source/entityitem.c`.
**Verified problem:** `g_blockShape[382..397] == SHAPE_CUBE` and `g_blockOpaque == 1` for `WATER` and `STATIONARY_WATER`. The player stands on the surface; `moveEntityWithHeading` (`player.c:313`) has only the ground/air branch. To the server this reads as hovering, and 80 ticks of that is the *"Flying is not enabled"* kick.

**Approach: fix collision now, defer the render change.** The ray-trace **already ignores water** (`ray_ignores`, `world.c:1115-1117`), so decoupling collision from render costs ~5 lines in `world.c` plus a ~70-line physics port — **no atlas re-bake, no translucency pass, no new allocation.**
**Details:**
- `World_BlockBoxes` / `World_BlockBoxesFor` (`world.c:224`/`238`): return 0 boxes for liquids, before the `SHAPE_CUBE` fast path. `World_BlockSolid` (`world.c:252`): false for liquids.
- Exclude liquids from `connect_mask` (`world.c:210`) so fences don't connect to water.
- Set **`g_blockOpaque = 0`** for fluids — otherwise neighbouring terrain faces stay culled against water and you see straight through the map from underwater (the same failure the existing opacity-aware culling work fixed for glass and leaves).
- Extend `ray_ignores` to cover `MAT_LAVA` as well as `MAT_WATER`.
- Port `Entity.handleWaterMovement`/`isInLava`: AABB shrunk by `(0.001, 0.401, 0.001)`, fluid height from `meta&7` (`8/9` when `meta&8`).
- Two new `moveEntityWithHeading` branches ahead of the ground/air branch:
  - **Water:** `moveFlying(0.02)`, `moveEntity()`, `motionX/Y/Z *= 0.8`, `motionY -= 0.02`, plus the ledge-bump `if (isCollidedHorizontally && isOffsetPositionInLiquid(...)) motionY = 0.3`.
  - **Lava:** the same shape with `*= 0.5`.
  - Jump held in liquid: `motionY += 0.04`.
- **`fallDistance = 0` while in fluid** — this is what makes MLG water actually work.
- `entityitem.c` stops resting on water surfaces for free via the same change.
- Deferred cosmetics (post-M3): source-water top face at 14/16, alpha blending, underwater screen tint, animated frames.
**Done when:** you sink into a map's water and swim up with `A`; a 30-block drop into a 1-deep water column does zero fall damage; 5 minutes including 10 MLG-water landings logs zero `moved wrongly`; and **the change adds 0 bytes of allocation** (it is a lookup change, not a data structure).

#### T22 — Protocol conformance *(depends on T6, T16, T21; **must land before any `MOVE` is sent**)*
**Goal:** the highest-risk task in the plan. Every item below causes a kick or a *silent drop*, not a cosmetic bug.
**Files:** new `proxy/netplayer.js`, `proxy/index.js`; new `source/netgame.c`, `source/netgame.h`; `source/gclink.h`.

**Proxy side — the whole protocol state machine:**

| Behaviour | Implementation |
|---|---|
| **C0B sprint/sneak edges** | Server-side sprint is driven **entirely** by `C0B`. Track `lastSentSprint`/`lastSentSneak`, diff against `MOVE.flags` each tick → `C0B(3/4)` / `C0B(0/1)`. Sprint locally without this and you get "moved too quickly" **and** no sprint knockback. |
| **The 1.8 sprint-reset asymmetry** | **After emitting `C02 ATTACK`, do not touch `lastSentSprint`.** The server silently cleared its own sprint flag and never tells the client, so vanilla never re-sends `START_SPRINTING`. That asymmetry *is* the W-tap mechanic; re-sending it is the classic custom-client tell. |
| **S08 → C06** | Reply **immediately** with the **server's** x/y/z/yaw/pitch, never the console's. Until that lands within 0.25 blocks the server's `hasMoved` stays false and *every* placement and movement packet is silently discarded. Then emit `TELEPORT(…, epoch++)` and **discard `MOVE`s carrying a stale epoch**. |
| **Movement packet selection** | Synthesize `C03/C04/C05/C06` from the 20 Hz `MOVE`: position if moved > 0.03² **or every 20 ticks**; look if yaw/pitch changed; else `C03` onGround-only. Y is the AABB `minY`, not `posY`. |
| **Attack ordering** | `C09(slot)` if the held slot changed → `C0A ArmSwing` → `C02 UseEntity(eid, ATTACK)`. The server computes damage from *its* `currentItem`, so the `C09` must precede. |
| **C02 filter** | Reject `ATTACK` against item / xp orb / arrow / projectile / self — the server **kicks** with *"Attempting to attack an invalid entity"*. |
| **S12 self** | `entityId == selfEid` → `SELF_VELOCITY(mx,my,mz)` (wire units `/8000.0`). |
| **C0F ConfirmTransaction** | Auto-ack every `S32`, or the server ignores all further window clicks. |
| **C15 / C17 on join** | `C15 ClientSettings("en_US", view 8, chatMode 0, colours true, skinParts 0x7F)` and `C17 CustomPayload("MC\|Brand", "vanilla")`. Plugins read `player.getLocale()`. |
| **Bow / eat / throw** | `USE_ITEM(start)` → `C08(pos -1/-1/-1, face 255, heldStack)`; `USE_ITEM(release)` → `C07 status 5 RELEASE_USE_ITEM`. 20 ticks = full bow charge. |
| **Item-use slowdown** | Return `USE_STATE(active)` so the console applies `moveForward/moveStrafe *= 0.2` — otherwise the position stream diverges during every golden apple. |
| **Chat rate limit** | Truncate `CHAT` to 100 chars and rate-limit to ~1/sec; Spigot kicks on chat spam. |

**Console side (`netgame.c`):**
```c
typedef struct {
    int selfEid;
    u8  teleportEpoch;   /* echoed in every MOVE; bumped by TELEPORT */
    int serverDriven;    /* 1 = disable local fall damage / respawn / drops */
    int gameMode;        /* 0 survival, 3 spectator */
    int usingItem;       /* from USE_STATE -> 0.2x movement */
    int moveTicks;       /* force a position packet every 20 */
} NetGame;

void NetGame_ApplyTeleport(NetGame*, Player*, double x, double y, double z,
                           float yaw, float pitch, u8 epoch);  /* snap, zero motion, zero fallDistance */
void NetGame_ApplyVelocity(NetGame*, Player*, double mx, double my, double mz);
void NetGame_SendMove(NetGame*, const Player*, const Inventory*);
```
- Keep the yaw/pitch conversion at the proxy boundary, but **round-trip-test it in both directions** — a sign error here surfaces as "moved wrongly", not as a visual bug.
**Done when:** 10 minutes of live walking, sprinting, jumping, swimming, mining and placing logs **zero** `moved wrongly` / `moved too quickly` / `Flying is not enabled`; placing a block immediately after a teleport succeeds; and attacking from a sprint produces visible knockback on the target.

#### T15 — Server-driven player state *(depends on T22)*
**Goal:** the server owns health and inventory.
**Files:** `source/player.c`, `source/inventory.c`, `source/netgame.c`.
**Details:**
- `HEALTH` → `p->health`, with local `player_fall()` and `player_respawn()` (`player.c:497`) **gated off** on `serverDriven` — otherwise they fight `S06 UpdateHealth`.
- `INV_SET` via `Inventory_SetSlot` (already the right primitive) and `HELD_SLOT` via a new **absolute** `Inventory_SetCurrentItem(Inventory*, int slot)` — `Inventory_ChangeCurrentItem` is relative-only.
- **Hunger: cut entirely.** The plugin forces food 20 and saturation 20.
- Velocity / teleport / sprint live in T22; death lives in T26.
**Done when:** taking fall damage on the server drops the console's hearts, and the server's inventory is mirrored exactly.

#### T14 — Dig/place over the network *(depends on T16, T17, T22)*
**Goal:** mine and build in a live game.
**Files:** `source/interact.c`, `source/main.c`.
**Details:** the block ray-trace, dig state machine, `blockHitDelay`, crack stages and `onBlockPlaced` metadata derivation **all already exist**. What remains:
- Send `DIG`(start/abort/stop) and `PLACE` to the proxy from the existing state machine.
- Keep local break-progress as **prediction**, but let the server's `BLOCK_SET` win.
- **Suppress `destroy_block`'s local `ItemWorld_SpawnAt`** in network mode — drops must come from the server, or every broken block duplicates.
- Swing animation comes from `Pose` (T17).
**Done when:** breaking a block on the console removes it on the server and vice versa, with no duplicate drops.

#### T18 — Entity ray-trace + `objectMouseOver` entity branch *(depends on T9, T17)*
**Goal:** the crosshair can target a player, not just a block.
**Files:** `source/entity.c`, `source/entity.h`, `source/interact.c`, `source/interact.h`.
```c
typedef struct { int slot, eid; double t, hx, hy, hz; } EntityHit;

/* Minecraft.getMouseOver's entity pass. `maxDist` = 3.0 in survival.
 * `blockT` is the block hit's distance (or maxDist when none) -- an entity
 * only wins if it is strictly closer. Returns 1 on a hit. */
int Entity_RayTrace(const EntityWorld *ew, double ex, double ey, double ez,
                    double lx, double ly, double lz,
                    double maxDist, double blockT, int excludeEid, EntityHit *out);
```
**Details:**
- **Reach: entities 3.0, blocks 4.5.** `interact.c:14` already has `REACH_DISTANCE 4.5`; add a separate `REACH_ENTITY 3.0`.
- Measured **eye → intercept on the expanded AABB**, not centre-to-centre. Each entity AABB **expanded by 0.1 per side** (`getCollisionBorderSize`), so a player's 0.6×1.8 becomes 0.8×2.0 for picking.
- `update_target()` (`interact.c:37-49`) runs the block trace first, unchanged, then the entity pass with `blockT` = the block hit distance.
- **Never target** dropped items, XP orbs, arrows, snowballs, pearls, potions, fish hooks, or `selfEid` — the server kicks on `C02 ATTACK` against those.
**Done when:** a replayed two-player capture shows an entity target inside 3.0 m, a block target beyond it, and never a dropped item.

#### T19 — Attack path, `ItemDef` combat fields, hurt feedback *(depends on T16, T17, T18)*
**Goal:** left-click on a player produces the correct packets and readable feedback.
**Files:** `source/items.h`, `source/items.c`, `source/player.h`, `source/player.c`, `source/interact.c`, `source/entity.c`, new `source/combat.c`, `source/combat.h`.
```c
/* ItemDef gains: */
float attackDamage;   /* ItemTool/ItemSword.attackDamage + 1.0 base */
u16   maxDamage;      /* 0 = indestructible */
u8    useAction;      /* USE_NONE/BOW/EAT/DRINK (EnumAction) */
u8    useDuration;    /* ticks: 32 food, 20 bow full charge, 0 = instant */

/* attackEntityFrom: srcX/srcZ drive attackedAtYaw. Sets pose.hurtTime = 10. */
void Player_Damage(Player*, float amount, int source, double srcX, double srcZ);
/* S12 handler: assign motion outright. Server-authoritative, never predicted. */
void Player_SetVelocity(Player*, double mx, double my, double mz);

typedef struct { int pendingAttackEid, pendingSwing, leftClickCounter; } Combat;
```
**Details:**
- Add `ItemDef` entries for the whole kit (`items.c` currently defines only 4 diamond tools): sword 7.0, axe 6.0, pickaxe 5.0, plus bow, fishing rod, golden apple, buckets, snowball, ender pearl, splash potions, arrow, stone, and the 4 armor pieces.
- `Combat_Tick` on `attackEdge && leftClickCounter == 0`: **always swing first** (`Pose_Swing`), then dispatch to the entity target; on a total miss set `leftClickCounter = 10`.
- **No local damage prediction whatsoever.** 1.8's `attackEntityFrom` returns early client-side (`EntityLivingBase.java:869-872`), so vanilla predicts no damage, no knockback, and **no sprint reset**. Do not add any. There is also **no attack cooldown in 1.8** — that arrived in 1.9. Do not add one.
- **Red hit flash** — on `ENTITY_ANIM(hurt)` (from `S19` status 2 / `S0B` type 1) set `hurtTime = maxHurtTime = 10` and tint the entity red while it decays: a TEV constant-colour blend on the entity draw, matching `RenderLivingBase.setBrightness`. Approximate is fine; this is the primary combat-legibility cue.
- **Hurt camera tilt** on the local player: roll the view by `sin((hurtTime/maxHurtTime)⁴·π)` about `attackedAtYaw`, in `Player_GetViewMatrix`. Note **1.8 has no red screen tint for the local player** — the tilt is the entire effect. Do not add a screen vignette.
- **Knockback** is `SELF_VELOCITY` → `Player_SetVelocity`. Applying it is **mandatory**: ignore it and the server moves you while you don't, which is permanent rubberband while being hit.
**Done when:** one `SWING` + one `USE_ENTITY(ATTACK)` per press, a 10-tick lockout after a miss, and a synthetic `HEALTH` drop + `SELF_VELOCITY` produces the red flash, camera tilt and knockback arc.

#### T23 — Chat + command palette *(depends on T2, T16)*
**Goal:** the practical "how does someone on a GameCube actually start a game" gap.
**Files:** new `source/cmdmenu.c`, `source/cmdmenu.h`; `source/hud.c`, `source/hud.h`, `source/main.c`, `proxy/index.js`.
**Details:** `/join <map>`, `/team 1-8`, `/start`, `/leave`, `/kit` are all **chat commands**, so a client that can send `C01` never needs the 54-slot GUI or the `C0E`/`C0F`/`C0D` window-click protocol.
- Two-level D-pad menu opened with **D-pad Up**, drawn with `Hud_DrawString` in the existing ortho pass — no new GX state:
  - `Join map ▸` — populated **from the existing `g_maps[]` already in the DOL**; `menu.c`'s scroll logic ports directly.
  - `Team ▸ 1-8` · `Start` · `Leave` · `Kit`
  - `Say ▸` — ~10 canned lines (`gg`, `gl hf`, `rush mid`, `help`, `sorry`, …)
- GCLink `CHAT(u8 len, char[len])` console→proxy.
- Doubles as the network pause menu on `Start`: Disconnect / Reconnect / Toggle nametags / RTT.
- *Optional follow-on:* a 6×8 on-screen character grid for free text (D-pad moves, `A` appends, `B` backspaces, `Y` toggles case). Needed for `/msg`; not needed to play.

**Chat visibility — the chat log is off unless the player asks for it.** This deliberately differs from vanilla (which shows every line for 10 s then fades): the screen is 480p, MegaSkywars chat is high-volume and heavily formatted, and an always-on log would sit on top of a fight.
- **`Z` holds the chat log open.** Held: the last ~10 lines, bottom-left, over a dimmed panel. Released: nothing drawn at all.
- Keep a scrollback ring of ~50 lines so opening it shows history, not only what arrived while it was open.
- **Unread indicator** — a small dot or count near the hotbar when lines have arrived since the log was last shown. Costs one glyph; without it, hidden chat means silently missing every game announcement.
- **The action bar stays always-on** (T10). Anything the player must not miss goes there, not into chat.
- Perf consequence: the chat log leaves the steady-state frame entirely, so T10's ≤ 2 ms text budget is effectively nametags only.
**Done when:** from a cold Dolphin boot with only a virtual pad you can connect, `/join hontori`, `/team 3`, `/start`, and be in a live game — no PC client involved at any point.

#### T26 — Spectator transition *(depends on T15, T22)*
**Goal:** survive dying. **This is not vanilla, and assuming it is would hang the client.**
**Files:** `source/netgame.c`, `source/hud.c`, `source/main.c`.
**Details:** MegaSkywars **cancels all lethal damage** (`Listeners.java:341-388`, `706-759`). The actual sequence is: health forced back to 20 → `S2B ChangeGameState(reason 3, value 3)` → `S08 PlayerPosLook` to map spawn → inventory cleared. There is **no death screen, no `S07 Respawn`, and `C16 ClientStatus` is never needed**. A client waiting for a death packet waits forever.
- `GAME_MODE(3)` → hide the hotbar, hearts and crosshair; show a SPECTATING banner; reuse the existing free-fly `camera.c`.
- Keep sending `MOVE` — `floatingTickCount` is not enforced for spectators. Verify flight permissions against the live server before implementing local noclip.
- Handle the inventory wipe and teleport that arrive with it.
- `GAME_MODE(0)` on the next game restores normal play. Local `player_respawn()` stays disabled (T15).
**Done when:** dying in a live game transitions cleanly to a flying spectator with the session intact, and the next game restores normal play without a reboot.

#### T13 — Item art + palette extension *(depends on T1)*
Extend `ITEM_TEXTURES` in `tools/build_atlas.py` with the kit's items: bow, arrow, fishing rod, golden apple, water bucket, lava bucket, diamond pickaxe, diamond axe, snowball, ender pearl, splash potions (keyed on damage value), fire resistance potion, plus the 4 diamond armor icons for the inventory screen. Add the missing *blocks* from T1. Regenerate the atlas and all `*_gen.h`. Budget: 389 free tiles.

> **Note:** `tools/build_atlas.py:44,48` has **hardcoded absolute paths** to the resource pack and MCP-919. Any task that regenerates the atlas (T4, T13) needs those to resolve on the machine running it.

---

## Performance: instrument early, budget per task

The GameCube is a fixed target — 24 MB MEM1, a 162 MHz GPU, no headroom to buy later. A deferred "optimization pass" means reaching Milestone 2 at 20 fps with 15.5 MB used and nothing left to cut. A standing "make it fast" task has no definition of done. So: **measure from Phase 0 (T27), put a budget line in every task's "done when", and pre-commit the one hot spot that is already predictable.**

| Task | Budget |
|---|---|
| **T4** | Peak heap on `mega_aegis` **≤ 11 MB** |
| **T9** | 128 entities hold **60 fps**; table allocation reported at boot |
| **T10** | Nametags at 128 entities **≤ 2 ms/frame** |
| **T24** | 2000-block burst drops no frames; **≤ 4 ms per flushed chunk** |
| **T21** | **0 bytes** of new allocation |

**The predictable hot spot is text.** 128 billboarded nametags is a few thousand quads per frame through the immediate-mode path that today draws nine hotbar slots. Mitigations, all cheap, in order: hard distance-cull past ~48 blocks; cap to the nearest ~16; batch a frame's glyphs into one `GX_Begin`; skip occluded tags using the block ray-trace that already exists. Hiding chat by default (T23) removes the other half of the problem.

**Contingency levers, if a budget gate fails** — a shelf to pull from rather than a redesign:
1. **Don't embed all 33 maps in the DOL** — recovers ~1.75 MB.
2. **CMPR atlas instead of RGB5A3** — saves ~4.5 MB rather than ~2.6 MB, at the cost of DXT1 artifacts on 16×16 pixel art. The emergency lever, not the default.
3. **Entity LOD** — a 2-box silhouette past ~32 blocks.
4. **Lower the entity cap back toward 64** — T8's nearest-N eviction makes the cap a tunable.
5. **ARAM (16 MB, DMA-only)** for cold map blobs. Last resort; a real engineering cost, not a flag.

---

## Cut by decision — deferred, not forgotten

- **Armor rendering.** Every player wears full enchanted diamond; on the console everyone will look unarmoured. Cosmetic only (damage is server-authoritative), but it removes a real read on how dangerous an opponent is. If revisited: one 64×32 `armor_diamond.tpl`, the same `ModelBiped` boxes re-drawn at scale 1.1, single layer, no glint (a glint costs a second TEV stage and a scrolling texgen matrix and is illegible at 480p).
- **Wall of Death geometry.** The border activates 2 minutes after game start at radius 175, shrinks 0.4 blocks/sec to a minimum of 20, and deals 1 HP/sec outside (`MapStatus.java:1441-1496`). It is rendered **only** as per-player particle packets, so it is invisible to the console. The obvious mitigation — "the server chats a red warning every second you're outside" — is no longer free now that chat is hidden by default. **So do the proxy-only half:** the proxy already receives the `S2A WorldParticles` packets and can fit the radius from them, or simply predict `175 − 0.4t` from the game-start chat line, and push a `BORDER 43m` line to the always-on action bar. No console rendering, and it is the difference between dying to a visible timer and dying to nothing. Full wall geometry stays cut.
- **Audio and particles.** No hit sounds, no crit particles, no countdown notes, no block-break puffs. The T19 red flash carries combat legibility on its own.
- **Lights Out mode.** The server sends `S2B GameStateChange` (2 = begin raining, 7 = strength 5) and rewrites an `ENDER_PORTAL` block 4 blocks above the player **every tick** to force a dark sky (`MapStatus.java:733-785`). Unsupported: the console ignores the game-state change, and T7's "drop unmapped states" rule makes the block spam harmless. The map plays normally, just without the darkness effect.
- **Hunger, XP mechanics, crafting, chest GUIs.** The plugin forces food 20, disables chests entirely, and needs no crafting for the kit.

---

## Milestones

| Milestone | Contents | Success criterion |
|---|---|---|
| **M0 Foundations** | Housekeeping commit, T4, **T27**, T1, **T2 (promoted)**, T3, T24 | ≥4 MB more free heap; a string renders; console echoes off a stub server; the perf overlay reports frame time and heap |
| **M1 Spectate** | T6, T7, T8, T9, T10, T11, T12 | Live game on screen — correct map, players, blocks, action bar, chat on demand — at 60 fps with 128 entities |
| **M2 Walk without being kicked** | **T16**, T17, **T21**, **T22**, T15 | 10 minutes live with zero kick or rubberband log lines |
| **M3 Play** | **T23**, T14, T13, **T18**, **T19**, T26 | Start a game from the pad alone, break/place, land a kill |

**Critical path:** T2 → T27; T16 → T17 → {T18, T19}; T21 → T22 → everything in M2/M3; T2 → T23.

**Do not partially land T22.** Sending movement without the `C0B` edges, the `S08`→`C06` reply and the epoch guard gets the account kicked within seconds, and the failure mode — silent packet drops — is very hard to diagnose from the console side.

**Highest value per line of code:** T21's collision fix. Five lines plus a short physics port, and it removes the largest single category of desync.

---

## Risks and open items

1. **Memory is the binding constraint**, and the whole performance story hangs on it. `mega_aegis` peaks at ~13.3 MB of ~15.2 MB. T4 must land before T9, and T27 makes every budget above checkable from Phase 0 onward. If the numbers do not work, see "Contingency levers".
2. **Scan origin assumption is unverified.** The entire delta approach rests on `.mworld` grid coords lining up with `teleportLocation`. T7 must prove this per map before anything is built on it. Mitigation is cheap (a per-map offset in `mapdb.json`); discovering it late is not.
3. **Palette gaps are silent today.** `world.c` indexes `g_blockShape[g]`/`g_topTile[g]` with **no bounds check** — a global id ≥ 533 reads out of bounds. T1's sentinel must be enforced on the console side too.
4. **Dolphin's BBA is finicky.** If TAP does not work, the built-in HLE adapter is the fallback. Because input injection into Dolphin is unreliable, network mode needs a compile-time autoload hook (the `TEST_AUTOLOAD` pattern at `main.c:350`) rather than menu navigation.
5. **Vanilla movement validation — not an anticheat.** `MegaCheck.java` is a **Hypixel API poller** (it GETs `api.hypixel.net/v2/counts` and posts to Discord); there is **no anticheat plugin on this server**. The real hazard is `NetHandlerPlayServer`, which is always on and un-bypassable: `floatingTickCount > 80` → kick *"Flying is not enabled on this server"*; `dist² > 100` → *"moved too quickly"*; `> 0.0625` discrepancy → *"moved wrongly"* + teleport-back. The plugin only *suppresses the log line* (`Main.suppressLogger`), it does not cancel the kick. The faithful physics port and the ±1 stick digitisation are the strongest mitigations, but **T21 and T22 are both prerequisites** — walking on water and unsynced sprint each trip these directly. Separately, the **ranked** queue rejects any protocol != 47; casual has no version check.
6. **Local prediction fights the server.** Three places predict state the server owns: `player_fall()` and `player_respawn()` (`player.c:497`), and `destroy_block`'s local `ItemWorld_SpawnAt`. All must be gated on `serverDriven` (T15/T14) or they will fight `S06 UpdateHealth` and duplicate every drop.
7. **Cosmetic entity spam.** Every kill spawns real mobs, armor stands and flying skulls tagged `cosmetic` (`Cosmetics.java:262-371`); every projectile gets a per-tick particle trail. The proxy **must** filter by entity type (T8) or the 128-slot table thrashes during a busy fight.
8. **The invisible border** *(accepted — see "Cut by decision")*. Mitigated by the proxy-side action-bar readout.
9. **`build_atlas.py` has hardcoded absolute paths** to the resource pack and MCP-919 (`tools/build_atlas.py:44,48`). Any task that regenerates the atlas needs those to resolve on the machine running it.

---

## References

Everything this plan depends on that lives outside the repo. **All paths verified to exist as written.** Note that MCP-919 keeps Java source under `src\` but *assets* under `temp\src\` — a common trip-up.

### Source material

| What | Where | Used by |
|---|---|---|
| **MegaSkywars plugin** (Spigot 1.8.8, NMS `v1_8_R3`) | `D:\Java Mods\MegaSkywars` | The authority for game rules. `src\me\pvpus\megaskywars\` — `MapManager.java:33-63` (map origins), `KitManager.java:37-122` + `MapStatus.java:935-963` (the kit), `Listeners.java:341-388,706-759` (damage/death), `MapStatus.java:1441-1496` (border). T7, T8, T13, T26 |
| **Minecraft 1.8.9 decompiled** (MCP-919) | `C:\Users\awt12\Downloads\MCP-919-main\MCP-919-main` | The authority for client-side mechanics. Java under `src\minecraft\net\minecraft\` — `block\Block.java` (`registerBlocks()` → numeric ids), `client\multiplayer\PlayerControllerMP.java`, `client\entity\EntityPlayerSP.java`, `network\NetHandlerPlayServer.java`. T1, T16–T22 |
| ├ vanilla texture fallback | `…\MCP-919-main\temp\src\minecraft\assets\minecraft\textures\blocks` | `tools/build_atlas.py:49` (`FALLBACK`) |
| ├ ASCII font sheet | `…\temp\src\minecraft\assets\minecraft\textures\font\ascii.png` (128×128, 16×16 grid of 8×8 glyphs) | T2 |
| └ default player skin | `…\temp\src\minecraft\assets\minecraft\textures\entity\steve.png` (64×32) | T9 |
| **RKYfault 16× resource pack** | `C:\Users\awt12\AppData\Roaming\.minecraft\resourcepacks\!                  §bRKYfault§3[16x]` | `tools/build_atlas.py:44` (`PACK`). The first source to look to for getting textures. Note the literal `§` colour codes and the run of spaces in the folder name. A `.zip` of the same pack and a newer `!    §bRKYfault §fV2 §7[§b16x§7].zip` sit alongside it; the build uses the **unzipped non-V2 directory**. T4, T13 |
| **Block id palette** (`_blockids.txt`, 533 lines) | `C:\Users\awt12\Downloads\download (1)\BlockScans\_blockids.txt` | **Outside the repo — T1 vendors a copy to `data/blockids.txt`** so builds are reproducible. |

### Hardware / platform

| What | Where |
|---|---|
| **GameCube online functionality** (BBA background, modem/adapter history, supported titles) | https://en.wikipedia.org/wiki/GameCube_online_functionality |
| **Nintendont** — the Wii loader that supports the Broadband Adapter; the eventual real-hardware target | https://github.com/fix94/nintendont |
| **Working BBA socket example**, vendored in-repo | `.gc-examples/devices/network/sockettest/source/sockettest.c` — `if_config()` → `net_socket`/`net_connect`/`net_recv`/`net_send`. Copy the idiom verbatim. T3 |
| `libbba.a` | Already installed in the devkitPPC portlibs; T3 adds `-lbba` to `LIBS`. |

### Proxy dependencies

| What | Where |
|---|---|
| **`minecraft-protocol`** (Node) — handles auth, encryption, S46 compression, serialization | https://github.com/PrismarineJS/node-minecraft-protocol — use `version: '1.8.9'`, `auth: 'microsoft'` |
| 1.8 protocol packet reference | https://wiki.vg/index.php?title=Protocol&oldid=7368 (protocol 47). Cross-check against MCP-919 rather than trusting the wiki alone. |

---

## Verification

**Per-task, before integration:**
- T1: `python tools/gen_blockmap.py`, then assert every line of `data/blockids.txt` round-trips through `blockmap.json`.
- T2/T4/T24: build the DOL (**PowerShell + msys2 make — Git Bash breaks gcc's `TEMP`**), boot in Dolphin, screenshot. T4 must be screenshot-identical to the pre-rebake baseline on `mega_aegis`, `hontori`, `sky_carnival` and `model_gallery` with `MODEL_TEST_MODE 1`.
- T3: run the stub Node echo server, confirm `HELLO` and RTT on screen.
- T24: stress-test script driving a 2000-block burst; watch T27 for leaks and remesh ms.
- T16: offline play must be behaviourally identical to the pre-refactor build.
- T22: diff the proxy's outbound packet log against a vanilla 1.8.9 client's for the same scripted actions.

**End-to-end, Milestone 1 (spectate):**
1. Start the proxy against the live server; join a game normally from a PC client so a match is running.
2. Dolphin: Config → GameCube → SP1 → **Broadband Adapter (TAP)** (or built-in), boot `msw-gcn.dol`.
3. Console connects, loads the right map, shows players and blocks.
4. Break a block on the PC client → it disappears on the GameCube within ~100 ms.
5. Replay a `--record` capture with the server offline, to confirm the console path is independent of live-server timing.

**End-to-end, Milestone 2 (play):**
1. Proxy against the live server, `--record` on. Build and boot as above.
2. From the pad alone: `/join hontori` → `/team 1` → `/start`.
3. Walk, sprint, jump, swim, place and break blocks for 5 minutes — **zero** teleport-backs or kicks in the proxy log.
4. Fight: hits land within 3.0 m, targets flash red and recoil, your camera tilts when you are hit.
5. Draw and fire the bow; eat a golden apple; pearl across a gap; MLG a water bucket.
6. Die → clean transition to a flying spectator, session intact; next game restores normal play.
7. Confirm the chat log draws **only** while `Z` is held, the unread indicator fires when lines arrive with it closed, and no chat text appears during a fight.
8. With the T27 overlay on throughout: frame time never exceeds 16.6 ms and free heap never dips below ~2 MB.

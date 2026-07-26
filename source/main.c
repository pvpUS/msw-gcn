/*---------------------------------------------------------------------------------

	msw-gcn - Mega Skywars for the GameCube

	Loads compressed voxel worlds (see tools/compress_worlds.py) selected from a
	main menu, decodes them relative to the spawn point, and renders them with
	textures pulled from the RKYfault resource pack atlas.

---------------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/console.h>

#include "camera.h"
#include "player.h"
#include "world.h"
#include "hud.h"
#include "helditem.h"
#include "entityitem.h"
#include "interact.h"
#include "net.h"
#include "netgame.h"
#include "entity.h"
#include "blockmap_gen.h"  /* BLOCKMAP_HASH, for the handshake readout */
#include "menu.h"
#include "maps_gen.h"
#include "gallery_tour_gen.h"
#include "block_book_gen.h"   /* ITEM_*_TILE */

#define DEFAULT_FIFO_SIZE (256 * 1024)

/* Starting kit: a set of tools and a few stacks of blocks, so mining and
 * building can be exercised straight off spawn (vanilla survival would start
 * you empty-handed, but there's no crafting here to earn a pickaxe with). Set
 * to 0 for a genuinely empty inventory -- everything else still works, bare
 * hands just mine slowly and stone-type blocks drop nothing, exactly as in
 * Minecraft. Block ids are 0-based global ids (== _blockids.txt line - 1). */
#define HUD_DEMO_ITEMS 1

/* Analog trigger threshold for the mine/place buttons (the digital click at
 * the bottom of the travel also counts). */
#define TRIGGER_THRESHOLD 60

static void *xfb[2] = {NULL, NULL};
static GXRModeObj *rmode;

/* Perspective projection, reloaded before each World_Draw because the HUD pass
 * swaps in an orthographic matrix. */
static Mtx44 g_proj;

/* Physics runs at a fixed 20 Hz (one Minecraft tick = 50 ms) regardless of
 * the 50/60 Hz video field rate; positions are interpolated for rendering. */
#define TICK_US 50000.0
#define MAX_ACCUM_US 250000.0   /* cap catch-up to 5 ticks after a hitch */

/* Model verification mode (see tools/gen_model_gallery.py and the
 * block-shapes-architecture verification notes): boots straight into the
 * synthetic "Model Gallery" map with a scripted camera, so a new/changed block
 * model can be screenshotted deterministically with zero controller input.
 * (The collision-box wireframe this used to overlay is gone; a block's
 * collision can still be sanity-checked by walking into it.) MODEL_TEST_ROW
 * picks one row (GALLERY_ROW_* from the generated gallery_tour_gen.h) to jump
 * straight to and hold -- the common case when checking a single block;
 * MODEL_TEST_ROW -1 instead cycles every row automatically (~4s each) for a
 * full regression pass. Leave MODEL_TEST_MODE 0 for normal play. */
#define MODEL_TEST_MODE 0
#define MODEL_TEST_ROW GALLERY_ROW_ANVIL
#define TOUR_HOLD_FRAMES 240

/* Scripted break/place self-test. Dolphin can't be driven by injected
 * controller input in this setup (see the project's testing notes), so this
 * aims the player down at the ground and holds mine, then place, on a fixed
 * frame schedule -- enough to screenshot the whole break -> drop -> pickup ->
 * place loop with an empty controller. Leave at 0 for normal play; pair with
 * TEST_AUTOLOAD to skip the menu. */
#define INTERACT_TEST_MODE 0

/* Dirty chunks World_FlushRemesh (T24) may re-mesh per rendered frame. A
 * ceiling, not a quota: the flush also stops on its own wall-clock budget, so
 * on the dense maps (where the worst chunk measures ~9 ms) a heavy chunk
 * self-limits the frame to itself, and the rest of the time both go. The perf
 * overlay's `dirty` and `chunk` readouts are how that is checked rather than
 * assumed.
 *
 * Offline play never has a dirty chunk -- interact.c uses the synchronous
 * World_SetBlock -- so this costs an early-out until network mode (T11) starts
 * feeding World_SetBlockDeferred. */
#define REMESH_PER_FRAME 2

/* Deferred-re-mesh stress test (T24's "done when"): on frame 120, apply a
 * 2000-block burst through World_SetBlockDeferred in a single frame, the shape
 * a proxy join-time chunk diff arrives in. Fires twice: the first burst warms
 * the path (under Dolphin the first deferred flush also pays to JIT-compile
 * pad_fill's edit-overlay branch, which the load-time mesh never takes), the
 * second is the one whose numbers count. Read them off the perf overlay --
 * `dirty` spikes and drains, `chunk` is the worst single re-mesh, `frame` max
 * catches the burst frame -- plus the two lines this draws underneath.
 *
 * Measured on mega_aegis: 2000 edits stage in 6.2 ms, 42 chunks drain in 25
 * frames, worst chunk 8.6 ms and worst frame 22.9 (mesher-bound, see
 * FLUSH_BUDGET_US in world.c for what the residual is and what removing it
 * would take). Pair with TEST_AUTOLOAD to skip the menu; 0 for normal play. */
#define REMESH_BURST_TEST 0
#define REMESH_BURST_BLOCKS 2000

/* Inventory-screen cursor movement. The 36 main slots are navigated as a 4x9
 * grid -- rows 0-2 are the storage grid (indices 9-35), row 3 is the hotbar
 * (0-8) -- matching how they're laid out on screen. Both axes wrap. The 4
 * armor slots are deliberately outside the cursor's reach: no armor exists in
 * this engine, so anything moved there would just be stranded. */
static int InvCursorMove(int cur, int dcol, int drow) {
	int row = (cur < INV_HOTBAR_SIZE) ? 3 : (cur - INV_HOTBAR_SIZE) / 9;
	int col = (cur < INV_HOTBAR_SIZE) ? cur : (cur - INV_HOTBAR_SIZE) % 9;
	col = (col + dcol + 9) % 9;
	row = (row + drow + 4) % 4;
	return (row == 3) ? col : INV_HOTBAR_SIZE + row * 9 + col;
}

static void SetTourCamera(Camera *cam, int idx) {
	const CamKeyframe *kf = &g_galleryTour[idx];
	Camera_Init(cam, kf->x * WORLD_BLOCK_SIZE, kf->y * WORLD_BLOCK_SIZE,
	            kf->z * WORLD_BLOCK_SIZE, kf->yaw, kf->pitch);
}

#if HUD_DEMO_ITEMS
static void FillDemoItems(Player *player) {
	Inventory *inv = &player->inventory;
	/* hotbar (slots 0-8): the four diamond tools, then blocks to build with
	 * (stone, cobble, oak log, glass, sand). The tools are ITEMS rather than
	 * placeable blocks -- their id is an atlas tile >= NUM_BLOCK_IDS -- so
	 * they also exercise the flat-item held/HUD rendering path. Which tool is
	 * held decides both mining speed and whether a block drops at all
	 * (Block_PlayerRelativeHardness / Block_CanHarvest). */
	Inventory_SetSlot(inv, 0, (ItemStack){ITEM_DIAMOND_PICKAXE_TILE, 0, 1});
	Inventory_SetSlot(inv, 1, (ItemStack){ITEM_DIAMOND_AXE_TILE,     0, 1});
	Inventory_SetSlot(inv, 2, (ItemStack){ITEM_DIAMOND_SHOVEL_TILE,  0, 1});
	Inventory_SetSlot(inv, 3, (ItemStack){ITEM_DIAMOND_SWORD_TILE,   0, 1});
	Inventory_SetSlot(inv, 4, (ItemStack){410, 0, 64});   /* stone       */
	Inventory_SetSlot(inv, 5, (ItemStack){ 61, 0, 32});   /* cobblestone */
	Inventory_SetSlot(inv, 6, (ItemStack){195, 0, 16});   /* oak log     */
	Inventory_SetSlot(inv, 7, (ItemStack){132, 0, 16});   /* glass       */
	Inventory_SetSlot(inv, 8, (ItemStack){281, 0, 16});   /* sand        */
	/* a few storage slots so the 3x9 grid isn't empty (obsidian, brick, stone) */
	Inventory_SetSlot(inv,  9, (ItemStack){234, 0, 10});
	Inventory_SetSlot(inv, 13, (ItemStack){ 33, 0, 20});
	Inventory_SetSlot(inv, 18, (ItemStack){410, 0, 64});
	/* exercise the real add/merge path: this stacks onto the hotbar cobble
	 * (slot 5, 32 -> 64) via storePartialItemStack rather than taking a new
	 * slot -- the same path block pickups take. */
	Inventory_AddItem(inv, 61, 0, 32);
}
#endif

/* Perf overlay (T27) on by default: every memory and frame-time budget in the
 * BBA plan is read off it, and on a 24 MB machine an unmeasured budget is an
 * aspiration. */
#define PERF_HUD 1

/* GCLink transport self-test (T3). Boots straight past the menu into a screen
 * that brings up the Broadband Adapter, dials the proxy and reports the link:
 * DHCP address, state machine, round trip, byte counters and the reason the
 * last link ended. Everything above the transport -- entities, blocks, the
 * game itself -- is later work; what this proves is that a GameCube can hold a
 * framed conversation with the proxy and recover on its own when it cannot.
 *
 * Pull the proxy down while it runs: the indicator should go amber, retry on
 * its own, and come back green without a reboot.
 *
 * NET_PROXY_IP is the machine running proxy/stub.js. A #define for now; T11
 * gives it a menu entry. */
#define NET_TEST_MODE 0
#define NET_PROXY_IP  "192.168.4.40"

/* Boot straight into network mode instead of the menu (T11). Dolphin cannot
 * be driven by injected controller input in this setup -- see the project's
 * testing notes -- so the only way to reach a mode that lives behind a
 * keypress is to compile the keypress away, the same trick TEST_AUTOLOAD uses
 * for the map list. Leave at 0 for normal play. */
#define NET_AUTOLOAD 0

#if NET_TEST_MODE
static void NetTest(void) {
	console_init(xfb[0], 0, 0, rmode->fbWidth, rmode->xfbHeight,
	             rmode->fbWidth * 2);
	VIDEO_SetNextFramebuffer(xfb[0]);
	VIDEO_Flush();

	printf("\x1b[2J\x1b[1;1H  GCLINK TRANSPORT TEST\n\n");
	printf("  bringing up the broadband adapter (DHCP)...\n");

	if (!Net_Init(20)) {
		printf("\n  FAILED: %s\n", Net_LastError());
		printf("  In Dolphin: Config > GameCube > SP1 > Broadband Adapter.\n");
		while (1) VIDEO_WaitVSync();
	}
	printf("  ip %s  gateway %s\n\n", Net_LocalIp(), Net_Gateway());
	printf("  dialling %s:%d ...\n\n", NET_PROXY_IP, GCLINK_PORT);
	Net_Connect(NET_PROXY_IP, GCLINK_PORT);

	u32 frames = 0, msgs = 0;
	u8  lastType = 0;
	while (1) {
		VIDEO_WaitVSync();
		PAD_ScanPads();
		if (PAD_ButtonsDown(0) & PAD_BUTTON_START) break;
		if (PAD_ButtonsDown(0) & PAD_BUTTON_A) Net_Reconnect();

		NetMsg m;
		while (Net_Poll(&m)) { msgs++; lastType = m.type; }

		/* Once a second, so the console is readable rather than a blur. */
		if (++frames % 60) continue;
		printf("\x1b[8;1H");
		printf("  state    %-12s                       \n", Net_StateText());
		printf("  rtt      %u ms                        \n", Net_RttMs());
		printf("  traffic  %u B in / %u B out           \n",
		       Net_BytesIn(), Net_BytesOut());
		printf("  frames   %u passed up, last type 0x%02X\n", msgs, lastType);
		printf("  last err %-40s\n",
		       Net_LastError()[0] ? Net_LastError() : "-");
		printf("\n  blockmap hash 0x%08X   A: redial   Start: exit\n",
		       (unsigned)BLOCKMAP_HASH);
	}
	Net_Disconnect(NULL);
}
#endif

/* Boot-time map audit: load and free every embedded map in turn and report,
 * on the libogc console, what each one costs and whether it gave the memory
 * back. Set to 1, build, boot, screenshot -- the whole regression check for
 * "all the maps still load" in one run with no controller input, which is how
 * anything that changes the palette, the mesh format or the atlas gets
 * cleared. Leave at 0 for normal play. */
#define MAP_AUDIT_MODE 0

#if MAP_AUDIT_MODE
static void MapAudit(void) {
	console_init(xfb[0], 0, 0, rmode->fbWidth, rmode->xfbHeight,
	             rmode->fbWidth * 2);
	VIDEO_SetNextFramebuffer(xfb[0]);
	VIDEO_Flush();

	printf("\x1b[2J\x1b[1;1H  MAP AUDIT  -  %d maps, free heap %uK at boot\n",
	       MAP_COUNT, Hud_HeapFree() / 1024);
	printf("  peak = heap the loaded world holds; leak = not returned on free\n\n");

	u32 worst = 0, leaked = 0;
	const char *worstName = "-";
	int failed = 0, i;

	for (i = 0; i < MAP_COUNT; i++) {
		World w;
		u32 before = Hud_HeapUsed();
		u32 size = (u32)(g_maps[i].end - g_maps[i].data);
		int ok = World_Load(&w, g_maps[i].data, size);
		u32 peak = ok ? Hud_HeapUsed() - before : 0;
		if (ok) World_Free(&w);
		u32 after = Hud_HeapUsed();

		if (!ok) failed++;
		if (peak > worst) { worst = peak; worstName = g_maps[i].name; }
		if (after > before) leaked += after - before;

		/* Two per row, so 32 maps fit a 480p console without scrolling. */
		printf("  %-14.14s %s%5uK%s", g_maps[i].name,
		       ok ? "" : "FAIL ", peak / 1024,
		       (after > before) ? "*" : " ");
		if (i % 2) printf("\n");
	}
	if (MAP_COUNT % 2) printf("\n");

	printf("\n  worst: %s at %uK    free heap now %uK\n",
	       worstName, worst / 1024, Hud_HeapFree() / 1024);
	printf("  %d/%d loaded, %uK not returned on free%s\n",
	       MAP_COUNT - failed, MAP_COUNT, leaked / 1024,
	       leaked ? "  (* marks the maps)" : "");

	while (1) VIDEO_WaitVSync();
}
#endif

#if REMESH_BURST_TEST
/* Lay REMESH_BURST_BLOCKS blocks in one frame through the deferred path. A
 * stride-2 lattice around the player rather than a compact slab: the point is
 * to dirty several dozen chunks, which is what a proxy join-time diff does and
 * what makes the nearest-first flush ordering observable (the lattice fills in
 * from under the player outwards over the following frames).
 *
 * Alternating two block ids also exercises the post-load dedup_tex append --
 * whichever of them the map does not already contain arrives as a new texcoord
 * against a display list that was recorded at load time. */
static struct {
	int   edits;        /* blocks that actually changed                      */
	int   peakDirty;    /* chunks the burst marked, before any flush          */
	int   frames;       /* rendered frames until the queue drained            */
	int   converged;
	float editMs;       /* the World_SetBlockDeferred calls alone             */
	u32   heapBefore, heapAfter;
} g_burst;

static void RemeshBurst(World *w, const Player *p, int dy) {
	int side = 1;
	while (side * side < REMESH_BURST_BLOCKS) side++;
	int bx0 = (int)floor(p->x) - side;
	int bz0 = (int)floor(p->z) - side;
	int by  = (int)floor(p->y) + dy;

	g_burst.edits = 0;
	g_burst.frames = 0;
	g_burst.converged = 0;
	g_burst.heapBefore = Hud_HeapUsed();
	u64 t0 = gettime();
	int n = 0, i, j;
	for (i = 0; i < side && n < REMESH_BURST_BLOCKS; i++)
		for (j = 0; j < side && n < REMESH_BURST_BLOCKS; j++, n++)
			g_burst.edits += World_SetBlockDeferred(w, bx0 + i * 2, by,
			                                        bz0 + j * 2,
			                                        ((i ^ j) & 1) ? 410 : 132);
	g_burst.editMs = (float)ticks_to_microsecs(gettime() - t0) / 1000.0f;

	WorldStats st;
	World_GetStats(w, &st);
	g_burst.peakDirty = (int)st.dirtyChunks;
}

/* Two lines under the perf panel with what the overlay's own rolling numbers
 * cannot show: how the burst frame split between edits and re-mesh, how many
 * rendered frames the queue took to drain, and whether the heap came back. */
static void RemeshBurstReport(int fbWidth, int efbHeight) {
	char line[80];
	Hud_Begin2D(fbWidth, efbHeight);
	snprintf(line, sizeof line, "burst %d edits %d ch in %d.%01d ms",
	         g_burst.edits, g_burst.peakDirty,
	         (int)g_burst.editMs, ((int)(g_burst.editMs * 10.0f)) % 10);
	Hud_DrawStringShadow(line, 2, 9 * 8 + 6, 0x55FF55FFu);
	snprintf(line, sizeof line, "converged %d fr  heap %+dK",
	         g_burst.frames,
	         ((int)g_burst.heapAfter - (int)g_burst.heapBefore) / 1024);
	Hud_DrawStringShadow(line, 2, 9 * 9 + 6,
	                     g_burst.converged ? 0x55FF55FFu : 0xFFAA00FFu);
	Hud_End2D();
}
#endif

static u32 CountEntities(const ItemWorld *iw) {
	u32 n = 0;
	int i;
	for (i = 0; i < MAX_ENTITY_ITEMS; i++) if (iw->e[i].alive) n++;
	return n;
}

static void RunWorld(World *w, u32 curr) {
	Mtx v;
	Player player;
	Camera cam;    /* debug free-fly camera (toggled with Z) */
	Interact interact;   /* block breaking/placing (PlayerControllerMP port) */
	ItemWorld items;     /* loose EntityItem drops                           */
	HudPerf perf;
	int perfOn = PERF_HUD;
	int freecam = 0;
	int invOpen = 0;   /* full inventory screen (toggle: X)                */
	int invCursor = 0; /* main-slot index the inventory cursor is over     */
	int tourIdx = (MODEL_TEST_ROW >= 0) ? MODEL_TEST_ROW : 0;
	int tourFrame = 0;
	float alpha = 0.0f;   /* inter-tick fraction, for interpolated rendering */
#if INTERACT_TEST_MODE
	int testFrame = 0;
#endif
#if REMESH_BURST_TEST
	int burstFrame = 0;
#endif

	Player_Spawn(&player, w);
	Interact_Init(&interact);
	ItemWorld_Init(&items);
	Hud_PerfInit(&perf);
#if HUD_DEMO_ITEMS
	FillDemoItems(&player);
#endif

	if (MODEL_TEST_MODE) {
		freecam = 1;
		SetTourCamera(&cam, tourIdx);
	}

	u64 prevTB = gettime();
	double accum = 0.0;

	while (SYS_MainLoop()) {
		PAD_ScanPads();
		u32 down = PAD_ButtonsDown(0);
		u32 held = PAD_ButtonsHeld(0);
		if (down & PAD_BUTTON_START) break;

		/* Mine (left mouse) on L, place/use (right mouse) on Y. L is analog,
		 * so either a firm pull or the digital click at the bottom counts. */
		int attackHeld = (held & PAD_TRIGGER_L) != 0 ||
		                 PAD_TriggerL(0) > TRIGGER_THRESHOLD;
		int useHeld    = (held & PAD_BUTTON_Y) != 0;

#if INTERACT_TEST_MODE
		/* Repeating cycle so any screenshot lands somewhere useful:
		 * 0-119 settle and aim down, 120-479 mine with the pickaxe,
		 * 480-839 place cobblestone, then start over. */
		testFrame++;
		if (testFrame == 120) player.pitch = -50.0f;
		int phase = (testFrame < 120) ? 0 : ((testFrame - 120) / 360) % 2;
		if (testFrame >= 120) player.inventory.currentItem = phase ? 5 : 0;
		attackHeld = (testFrame >= 120 && phase == 0);
		useHeld    = (testFrame >= 120 && phase == 1);
#endif

		/* X toggles the inventory screen. Closed: D-pad L/R scrolls the held
		 * hotbar slot (changeCurrentItem: +1 = left, -1 = right). Open: the
		 * D-pad drives the slot cursor and A/B stand in for the left/right
		 * mouse buttons (movement input is frozen while it's up, so borrowing
		 * jump/sneak is free). */
		if (down & PAD_BUTTON_X) {
			invOpen = !invOpen;
			/* Container.onContainerClosed: a stack still on the cursor when
			 * the screen closes is thrown into the world. */
			if (!invOpen && !ItemStack_IsEmpty(&player.inventory.carried)) {
				ItemWorld_SpawnAt(&items, (int)floor(player.x),
				                  (int)floor(player.y + 1.0),
				                  (int)floor(player.z),
				                  player.inventory.carried);
				player.inventory.carried = (ItemStack){-1, 0, 0};
			}
		}
		if (!invOpen) {
			if (down & PAD_BUTTON_LEFT)  Inventory_ChangeCurrentItem(&player.inventory,  1);
			if (down & PAD_BUTTON_RIGHT) Inventory_ChangeCurrentItem(&player.inventory, -1);
			/* Perf overlay toggle. T16's button map reclaims D-pad Down for
			 * drop-item; move this then. */
			if (down & PAD_BUTTON_DOWN) {
				perfOn = !perfOn;
				if (perfOn) { Hud_PerfInit(&perf); World_ResetStatsMax(w); }
			}
		} else {
			if (down & PAD_BUTTON_LEFT)  invCursor = InvCursorMove(invCursor, -1,  0);
			if (down & PAD_BUTTON_RIGHT) invCursor = InvCursorMove(invCursor,  1,  0);
			if (down & PAD_BUTTON_UP)    invCursor = InvCursorMove(invCursor,  0, -1);
			if (down & PAD_BUTTON_DOWN)  invCursor = InvCursorMove(invCursor,  0,  1);
			if (down & PAD_BUTTON_A) Inventory_SlotClick(&player.inventory, invCursor, 0);
			if (down & PAD_BUTTON_B) Inventory_SlotClick(&player.inventory, invCursor, 1);
		}

		if (down & PAD_TRIGGER_Z) {
			freecam = !freecam;
			if (freecam) {
				/* start the debug camera at the player's eye */
				Camera_Init(&cam,
				            (float)(player.x * WORLD_BLOCK_SIZE),
				            (float)((player.y + 1.62) * WORLD_BLOCK_SIZE),
				            (float)(player.z * WORLD_BLOCK_SIZE),
				            player.yaw, player.pitch);
			}
		}

		if (MODEL_TEST_MODE && MODEL_TEST_ROW < 0 && ++tourFrame >= TOUR_HOLD_FRAMES) {
			tourFrame = 0;
			tourIdx = (tourIdx + 1) % GALLERY_TOUR_COUNT;
			SetTourCamera(&cam, tourIdx);
		}

#if REMESH_BURST_TEST
		/* Two bursts, at different heights so the second is genuinely new work.
		 * The first is a warm-up whose numbers are thrown away: under Dolphin
		 * the very first deferred flush also pays to JIT-compile a path the
		 * load-time mesh never takes (pad_fill's edit-overlay branch, which
		 * only runs once World.editCount is non-zero), and that one-time cost
		 * is not something a real GameCube would pay. The maxima are reset in
		 * between, so what the overlay reports is the steady-state cost. */
		++burstFrame;
		if (burstFrame == 120) RemeshBurst(w, &player, 8);
		if (burstFrame == 300) { World_ResetStatsMax(w); Hud_PerfInit(&perf); }
		if (burstFrame == 301) RemeshBurst(w, &player, 12);
#endif

		u64 nowTB = gettime();
		double dtUs = (double)ticks_to_microsecs(nowTB - prevTB);
		prevTB = nowTB;

		double tickUs = 0.0;
		if (freecam) {
			Camera_Update(&cam, 0);
			Camera_GetViewMatrix(&cam, v);
		} else {
			if (!invOpen) Player_Look(&player, 0);   /* freeze the view when browsing */
			accum += dtUs;
			if (accum > MAX_ACCUM_US) accum = MAX_ACCUM_US;
			u64 tickTB = gettime();
			while (accum >= TICK_US) {
				/* Minecraft.runTick's order: the controller acts on the
				 * targeted block first, then the player moves, then the
				 * world's entities update. */
				Interact_Tick(&interact, w, &player, &items,
				              attackHeld, useHeld, invOpen);
				Player_Tick(&player, w, 0, invOpen);
				ItemWorld_Tick(&items, w, &player);
				accum -= TICK_US;
			}
			tickUs = (double)ticks_to_microsecs(gettime() - tickTB);
			alpha = (float)(accum / TICK_US);
			Player_GetViewMatrix(&player, alpha, v);
		}

		/* Drain the deferred re-mesh queue (T24) before drawing, so a chunk
		 * rebuilt this frame is the one submitted this frame. Nearest-first
		 * from the player, or the camera when it has been detached. */
		int stillDirty = World_FlushRemesh(w, REMESH_PER_FRAME,
		                  freecam ? cam.pos.x / WORLD_BLOCK_SIZE : player.x,
		                  freecam ? cam.pos.z / WORLD_BLOCK_SIZE : player.z);
		(void)stillDirty;
#if REMESH_BURST_TEST
		/* Frames from the burst until the queue drains, and whether the heap
		 * came back to what it was: T24 budgets convergence at ~30 frames with
		 * no arena growth, and neither is visible in a rolling average. */
		if (burstFrame >= 301 && !g_burst.converged) {
			g_burst.frames++;
			if (!stillDirty) {
				g_burst.converged = 1;
				g_burst.heapAfter = Hud_HeapUsed();
			}
		}
#endif

		GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
		GX_InvVtxCache();
		GX_InvalidateTexAll();

		/* Reload the perspective projection each frame -- the HUD pass leaves an
		 * orthographic matrix loaded. */
		GX_LoadProjectionMtx(g_proj, GX_PERSPECTIVE);
		World_Draw(w, v);

		if (!MODEL_TEST_MODE) {
			/* Targeted-block outline and, while mining, the crack overlay --
			 * both sit on the block's own surfaces, so they draw right after
			 * the terrain and before anything that floats in front of it. */
			if (!freecam && interact.hasTarget) {
				World_DrawBlockOutline(w, v, interact.target.bx,
				                       interact.target.by, interact.target.bz);
				int stage = Interact_BreakStage(&interact);
				if (stage >= 0)
					World_DrawBreakOverlay(w, v, interact.curBx, interact.curBy,
					                       interact.curBz, stage);
			}
			ItemWorld_Draw(&items, v, alpha);
		}

		if (!MODEL_TEST_MODE) {
			/* First-person held item, on top of the world but under the HUD.
			 * Hidden while the full inventory screen is open (like vanilla). */
			if (!invOpen)
				HeldItem_Draw(&player, rmode->fbWidth, rmode->efbHeight);
			Hud_Draw(&player, rmode->fbWidth, rmode->efbHeight, invOpen, invCursor);
		}

		/* Perf overlay last, on top of everything, in its own 2D pass. Sampled
		 * every frame even when hidden so the maxima and the heap low-water
		 * mark stay honest across a toggle. */
		Hud_PerfSample(&perf, dtUs, tickUs, w, CountEntities(&items));
		if (perfOn) Hud_DrawPerf(&perf, rmode->fbWidth, rmode->efbHeight);
#if REMESH_BURST_TEST
		if (burstFrame >= 301)
			RemeshBurstReport(rmode->fbWidth, rmode->efbHeight);
#endif
		/* Draws nothing until the network has been brought up, so this costs
		 * offline play a branch and gives network mode (T11) the indicator
		 * already in place. */
		Hud_DrawNetStatus(rmode->fbWidth, rmode->efbHeight);

		GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
		GX_SetColorUpdate(GX_TRUE);
		GX_CopyDisp(xfb[curr], GX_TRUE);
		GX_DrawDone();

		VIDEO_SetNextFramebuffer(xfb[curr]);
		VIDEO_Flush();
		VIDEO_WaitVSync();
		curr ^= 1;
	}
}

/* ---- network mode (T11) --------------------------------------------------
 * Milestone 1: connect, load whichever map the server is running, and watch a
 * live game. The console draws and does not act -- no movement is sent, which
 * is the single decision that keeps this milestone free of the kick risk that
 * T22 exists to manage.
 *
 * The camera is camera.c's free-fly, parked on the proxy account's position
 * the first time the server teleports it. That is the right spectator camera
 * and it is already written; a first-person one would need the movement path
 * this milestone deliberately does not have. */

/* Bring the Broadband Adapter up, on the libogc console so a DHCP failure is
 * readable rather than a black screen. if_config blocks for up to `retries`
 * attempts and cannot be made asynchronous, so it happens here -- at the point
 * the player asked for multiplayer -- rather than in front of every offline
 * boot. */
static int NetBringUp(void) {
	if (Net_GetState() != NET_DOWN) return 1;

	console_init(xfb[0], 0, 0, rmode->fbWidth, rmode->xfbHeight,
	             rmode->fbWidth * 2);
	VIDEO_SetNextFramebuffer(xfb[0]);
	VIDEO_Flush();

	printf("\x1b[2J\x1b[1;1H  MEGA SKYWARS  -  multiplayer\n\n");
	printf("  bringing up the broadband adapter (DHCP)...\n");
	if (!Net_Init(20)) {
		printf("\n  FAILED: %s\n", Net_LastError());
		printf("  In Dolphin: Config > GameCube > SP1 > Broadband Adapter.\n");
		printf("\n  Press Start to go back.\n");
		while (1) {
			VIDEO_WaitVSync();
			PAD_ScanPads();
			if (PAD_ButtonsDown(0) & PAD_BUTTON_START) return 0;
		}
	}
	printf("  ip %s  gateway %s\n", Net_LocalIp(), Net_Gateway());
	printf("  dialling %s:%d ...\n", NET_PROXY_IP, GCLINK_PORT);
	return 1;
}

static void RunNetwork(u32 curr) {
	NetGame ng;
	Camera  cam;
	World   world;
	HudPerf perf;
	HudTag  tags[HUD_TAG_MAX];
	Mtx     v;
	int   haveWorld = 0;
	int   camSeeded = 0;
	int   perfOn = PERF_HUD;
	float alpha = 0.0f;
	NetState lastState = NET_DOWN;

	NetGame_Init(&ng);
	Hud_PerfInit(&perf);
	Camera_Init(&cam, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
	Net_Connect(NET_PROXY_IP, GCLINK_PORT);

	u64 prevTB = gettime();
	double accum = 0.0;

	while (SYS_MainLoop()) {
		PAD_ScanPads();
		u32 down = PAD_ButtonsDown(0);
		u32 held = PAD_ButtonsHeld(0);
		if (down & PAD_BUTTON_START) break;
		if (down & PAD_BUTTON_Y) Net_Reconnect();
		if (down & PAD_BUTTON_DOWN) {
			perfOn = !perfOn;
			if (perfOn) {
				Hud_PerfInit(&perf);
				if (haveWorld) World_ResetStatsMax(&world);
			}
		}
		/* Z holds the chat log open -- T23's binding, in place from here so
		 * the hidden-by-default log is testable in this milestone rather than
		 * three tasks later. */
		ng.hud.show = (held & PAD_TRIGGER_Z) != 0;

		/* A reconnect makes the proxy restate everything (world, then state,
		 * then entities), so the table has to be empty first: anything that
		 * died while the link was down would otherwise stand there forever. */
		NetState st = Net_GetState();
		if (st == NET_READY && lastState != NET_READY) NetGame_Reset(&ng);
		lastState = st;

		/* Drain the link. A pending map change stops the drain, so the
		 * join-time block diff behind it is applied to the right world. */
		if (NetGame_Poll(&ng, haveWorld ? &world : NULL)) {
			int idx = ng.wantMap;
			if (haveWorld) { World_Free(&world); haveWorld = 0; }
			u32 size = (u32)(g_maps[idx].end - g_maps[idx].data);
			haveWorld = World_Load(&world, g_maps[idx].data, size);
			/* Marked loaded either way: a map that will not load is not going
			 * to load on the next frame either, and retrying it every frame
			 * would wedge the drain instead of just missing the geometry. */
			NetGame_MapLoaded(&ng, idx);
			camSeeded = 0;
		}

		/* Park the camera on the account the proxy is logged in as, once per
		 * map. After that it is the player's to fly; X re-centres it. */
		if (ng.tpPending && (!camSeeded || (down & PAD_BUTTON_X))) {
			Camera_Init(&cam,
			            (float)(ng.tpX * WORLD_BLOCK_SIZE),
			            (float)((ng.tpY + 1.62) * WORLD_BLOCK_SIZE),
			            (float)(ng.tpZ * WORLD_BLOCK_SIZE),
			            ng.tpYaw, ng.tpPitch);
			camSeeded = 1;
		}

		u64 nowTB = gettime();
		double dtUs = (double)ticks_to_microsecs(nowTB - prevTB);
		prevTB = nowTB;

		accum += dtUs;
		if (accum > MAX_ACCUM_US) accum = MAX_ACCUM_US;
		u64 tickTB = gettime();
		while (accum >= TICK_US) { NetGame_Tick(&ng); accum -= TICK_US; }
		double tickUs = (double)ticks_to_microsecs(gettime() - tickTB);
		alpha = (float)(accum / TICK_US);

		Camera_Update(&cam, 0);
		Camera_GetViewMatrix(&cam, v);

		double eyeX = cam.pos.x / WORLD_BLOCK_SIZE;
		double eyeY = cam.pos.y / WORLD_BLOCK_SIZE;
		double eyeZ = cam.pos.z / WORLD_BLOCK_SIZE;

		if (haveWorld) World_FlushRemesh(&world, REMESH_PER_FRAME, eyeX, eyeZ);

		GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
		GX_InvVtxCache();
		GX_InvalidateTexAll();
		GX_LoadProjectionMtx(g_proj, GX_PERSPECTIVE);

		if (haveWorld) World_Draw(&world, v);
		Entity_Draw(&ng.ents, v, alpha);

		int nTags = Entity_CollectTags(&ng.ents, haveWorld ? &world : NULL,
		                               v, g_proj, eyeX, eyeY, eyeZ,
		                               rmode->fbWidth, rmode->efbHeight,
		                               tags, HUD_TAG_MAX);
		Hud_DrawTags(tags, nTags, rmode->fbWidth, rmode->efbHeight);
		Hud_DrawNetOverlay(&ng.hud, rmode->fbWidth, rmode->efbHeight);

		if (!haveWorld) {
			/* Nothing on screen is ambiguous between "connecting", "the proxy
			 * is in the lobby" and "something is broken"; say which. */
			HudScreen sc = Hud_Begin2D(rmode->fbWidth, rmode->efbHeight);
			const char *msg = (st != NET_READY) ? "connecting to the proxy..."
			                : "waiting for a map (the account is in the lobby)";
			Hud_DrawStringShadow(msg,
			                     (int)(sc.w / 2) - Hud_StringWidth(msg) / 2,
			                     (int)(sc.h / 2) - 4, 0xFFFFFFFFu);
			Hud_End2D();
		}

		Hud_PerfSample(&perf, dtUs, tickUs, haveWorld ? &world : NULL,
		               Entity_Count(&ng.ents));
		if (perfOn) Hud_DrawPerf(&perf, rmode->fbWidth, rmode->efbHeight);
		Hud_DrawNetStatus(rmode->fbWidth, rmode->efbHeight);

		GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
		GX_SetColorUpdate(GX_TRUE);
		GX_CopyDisp(xfb[curr], GX_TRUE);
		GX_DrawDone();

		VIDEO_SetNextFramebuffer(xfb[curr]);
		VIDEO_Flush();
		VIDEO_WaitVSync();
		curr ^= 1;
	}

	Net_Disconnect(NULL);
	if (haveWorld) World_Free(&world);
}

int main(int argc, char **argv) {
	GXColor background = {135, 190, 235, 0xff}; /* sky blue */

	VIDEO_Init();
	PAD_Init();

	/* Block drops use rand() for their spawn offset, quantity and the
	 * 1-in-N drop rolls (gravel's flint, tall grass's seeds). */
	srand((unsigned)gettime());

	rmode = VIDEO_GetPreferredMode(NULL);

	void *gp_fifo = memalign(32, DEFAULT_FIFO_SIZE);
	memset(gp_fifo, 0, DEFAULT_FIFO_SIZE);

	xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb[0]);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

	GX_Init(gp_fifo, DEFAULT_FIFO_SIZE);
	GX_SetCopyClear(background, 0x00ffffff);

	GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
	f32 yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
	u32 xfbHeight = GX_SetDispCopyYScale(yscale);
	GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopyDst(rmode->fbWidth, xfbHeight);
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
	GX_SetFieldMode(rmode->field_rendering,
		((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));

	GX_CopyDisp(xfb[0], GX_TRUE);
	GX_SetDispCopyGamma(GX_GM_1_0);

	f32 w = rmode->viWidth;
	f32 h = rmode->viHeight;
	guPerspective(g_proj, 60, w / h, 1.0f, 8000.0f);
	GX_LoadProjectionMtx(g_proj, GX_PERSPECTIVE);

	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);

	World_InitGX();
	Hud_InitGX();
	HeldItem_InitGX();
	Entity_InitGX();

#if MAP_AUDIT_MODE
	MapAudit();   /* never returns */
#endif
#if NET_TEST_MODE
	NetTest();
	return 0;
#endif

#if NET_AUTOLOAD
	if (NetBringUp()) RunNetwork(0);
	return 0;
#endif

	/* Set to a map index to bypass the menu (rendering smoke test); -1 = menu. */
#define TEST_AUTOLOAD (-1)
	while (1) {
#if MODEL_TEST_MODE
		/* Looked up by name rather than a hardcoded index so the gallery's
		 * position in g_maps[] (alphabetical, regenerated by
		 * compress_worlds.py) can't silently drift out of sync. */
		int sel = -1;
		{
			int mi;
			for (mi = 0; mi < MAP_COUNT; mi++) {
				if (!strcmp(g_maps[mi].name, "Model Gallery")) { sel = mi; break; }
			}
		}
		if (sel < 0) break; /* gallery map missing from this build */
#elif TEST_AUTOLOAD >= 0
		/* Read through a volatile so g_maps[] stays live. With a constant index
		 * GCC folds the array access, every other map blob becomes unreferenced,
		 * and --gc-sections (on by default in gamecube_rules, together with
		 * -fdata-sections) drops all 31 of them -- a DOL 4.4 MB smaller than the
		 * shipping one, and a heap reading to match. An autoload build has to
		 * measure like the real thing or the perf overlay lies. */
		static volatile int autoloadIndex = TEST_AUTOLOAD;
		int sel = autoloadIndex;
#else
		char netTarget[32];
		snprintf(netTarget, sizeof netTarget, "%s:%d",
		         NET_PROXY_IP, GCLINK_PORT);
		int sel = Menu_Run(g_maps, MAP_COUNT, xfb[0], rmode, netTarget);
		if (sel == MENU_NETWORK) {
			if (NetBringUp()) RunNetwork(0);
			continue;
		}
		if (sel < 0) break;
#endif

		World world;
		u32 size = (u32)(g_maps[sel].end - g_maps[sel].data);
		if (!World_Load(&world, g_maps[sel].data, size))
			continue; /* decode/alloc failed - back to the menu */

		RunWorld(&world, 0);
		World_Free(&world);
#if MODEL_TEST_MODE || TEST_AUTOLOAD >= 0
		break;
#endif
	}

	return 0;
}

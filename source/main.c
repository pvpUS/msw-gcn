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

#if MAP_AUDIT_MODE
	MapAudit();   /* never returns */
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
		int sel = Menu_Run(g_maps, MAP_COUNT, xfb[0], rmode);
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

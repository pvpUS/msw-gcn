/*---------------------------------------------------------------------------------

	msw-gcn - Mega Skywars for the GameCube

	Loads compressed voxel worlds (see tools/compress_worlds.py) selected from a
	main menu, decodes them relative to the spawn point, and renders them with
	textures pulled from the RKYfault resource pack atlas.

---------------------------------------------------------------------------------*/
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include "camera.h"
#include "player.h"
#include "world.h"
#include "menu.h"
#include "maps_gen.h"

#define DEFAULT_FIFO_SIZE (256 * 1024)

static void *xfb[2] = {NULL, NULL};
static GXRModeObj *rmode;

/* Physics runs at a fixed 20 Hz (one Minecraft tick = 50 ms) regardless of
 * the 50/60 Hz video field rate; positions are interpolated for rendering. */
#define TICK_US 50000.0
#define MAX_ACCUM_US 250000.0   /* cap catch-up to 5 ticks after a hitch */

static void RunWorld(World *w, u32 curr) {
	Mtx v;
	Player player;
	Camera cam;    /* debug free-fly camera (toggled with Z) */
	int freecam = 0;

	Player_Spawn(&player, w);

	u64 prevTB = gettime();
	double accum = 0.0;

	while (SYS_MainLoop()) {
		PAD_ScanPads();
		u32 down = PAD_ButtonsDown(0);
		if (down & PAD_BUTTON_START) break;

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

		u64 nowTB = gettime();
		double dtUs = (double)ticks_to_microsecs(nowTB - prevTB);
		prevTB = nowTB;

		if (freecam) {
			Camera_Update(&cam, 0);
			Camera_GetViewMatrix(&cam, v);
		} else {
			Player_Look(&player, 0);
			accum += dtUs;
			if (accum > MAX_ACCUM_US) accum = MAX_ACCUM_US;
			while (accum >= TICK_US) {
				Player_Tick(&player, w, 0);
				accum -= TICK_US;
			}
			Player_GetViewMatrix(&player, (float)(accum / TICK_US), v);
		}

		GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
		GX_InvVtxCache();
		GX_InvalidateTexAll();

		World_Draw(w, v);

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
	Mtx44 p;
	GXColor background = {135, 190, 235, 0xff}; /* sky blue */

	VIDEO_Init();
	PAD_Init();

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
	guPerspective(p, 60, w / h, 1.0f, 8000.0f);
	GX_LoadProjectionMtx(p, GX_PERSPECTIVE);

	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);

	World_InitGX();

	/* Set to a map index to bypass the menu (rendering smoke test); -1 = menu. */
#define TEST_AUTOLOAD (-1)
	while (1) {
#if TEST_AUTOLOAD >= 0
		int sel = TEST_AUTOLOAD;
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
#if TEST_AUTOLOAD >= 0
		break;
#endif
	}

	return 0;
}

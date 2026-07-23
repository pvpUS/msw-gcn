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

#include "camera.h"
#include "world.h"
#include "menu.h"
#include "maps_gen.h"

#define DEFAULT_FIFO_SIZE (256 * 1024)

static void *xfb[2] = {NULL, NULL};
static GXRModeObj *rmode;

static void RunWorld(World *w, u32 curr) {
	Mtx v;
	Camera cam;
	guVector pos;
	float yaw, pitch;

	World_SpawnCamera(w, &pos, &yaw, &pitch);
	Camera_Init(&cam, pos.x, pos.y, pos.z, yaw, pitch);

	while (SYS_MainLoop()) {
		PAD_ScanPads();
		if (PAD_ButtonsDown(0) & PAD_BUTTON_START) break;

		Camera_Update(&cam, 0);
		Camera_GetViewMatrix(&cam, v);

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

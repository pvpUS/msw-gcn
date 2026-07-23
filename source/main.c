/*---------------------------------------------------------------------------------

	msw-gcn - Mega Skywars for the GameCube
	Voxel tech demo: a floating island rendered with per-face culling, and a
	flying camera. No gameplay yet - this proves out the rendering pipeline.

---------------------------------------------------------------------------------*/
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <gccore.h>

#include "camera.h"
#include "chunk.h"

#define DEFAULT_FIFO_SIZE (256 * 1024)

static void *xfb[2] = {NULL, NULL};
static u32 curr_fb = 0;
static GXRModeObj *rmode;

int main(int argc, char **argv) {
	Mtx44 p; // projection matrix
	Mtx v;   // view matrix
	GXColor background = {96, 160, 220, 0xff}; // sky blue

	VIDEO_Init();
	PAD_Init();

	rmode = VIDEO_GetPreferredMode(NULL);

	void *gp_fifo = memalign(32, DEFAULT_FIFO_SIZE);
	memset(gp_fifo, 0, DEFAULT_FIFO_SIZE);

	xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb[curr_fb]);
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

	GX_SetCullMode(GX_CULL_NONE);
	GX_CopyDisp(xfb[curr_fb], GX_TRUE);
	GX_SetDispCopyGamma(GX_GM_1_0);

	f32 w = rmode->viWidth;
	f32 h = rmode->viHeight;
	guPerspective(p, 60, w / h, 1.0f, 4000.0f);
	GX_LoadProjectionMtx(p, GX_PERSPECTIVE);

	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);

	Camera cam;
	Camera_Init(&cam, 0.0f, 40.0f, 220.0f, 0.0f, -10.0f);

	Chunk_GenerateIsland();
	Chunk_InitGX();

	while (SYS_MainLoop()) {
		PAD_ScanPads();

		if (PAD_ButtonsDown(0) & PAD_BUTTON_START) break;

		Camera_Update(&cam, 0);
		Camera_GetViewMatrix(&cam, v);

		GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
		GX_InvVtxCache();
		GX_InvalidateTexAll();

		GX_LoadPosMtxImm(v, GX_PNMTX0);
		Chunk_Draw();

		GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
		GX_SetColorUpdate(GX_TRUE);
		GX_CopyDisp(xfb[curr_fb], GX_TRUE);
		GX_DrawDone();

		VIDEO_SetNextFramebuffer(xfb[curr_fb]);
		VIDEO_Flush();
		VIDEO_WaitVSync();
		curr_fb ^= 1;
	}

	return 0;
}

#include <stdio.h>
#include <gccore.h>
#include <ogc/console.h>

#include "menu.h"

#define VISIBLE 20
#define STICK_THRESH 42
#define STICK_REPEAT 7

static void DrawList(const MapEntry *maps, int count, int sel, int top) {
	printf("\x1b[2J\x1b[1;1H");
	printf("  MEGA SKYWARS  -  GameCube voxel worlds\n");
	printf("  ======================================\n\n");
	printf("  Select a map  (%d available):\n\n", count);

	int i;
	for (i = 0; i < VISIBLE; i++) {
		int idx = top + i;
		if (idx >= count) break;
		if (idx == sel)
			printf("  \x1b[7m> %-22s %7u blocks \x1b[0m\n",
			       maps[idx].name, maps[idx].blocks);
		else
			printf("    %-22s %7u blocks\n",
			       maps[idx].name, maps[idx].blocks);
	}

	printf("\n  D-Pad / stick: move    A: load    Start: quit\n");
	if (count > VISIBLE)
		printf("  (%d-%d of %d)\n", top + 1,
		       (top + VISIBLE < count) ? top + VISIBLE : count, count);
}

int Menu_Run(const MapEntry *maps, int count, void *xfb, GXRModeObj *rmode) {
	/* full-screen console so no uninitialised framebuffer shows at the edges */
	console_init(xfb, 0, 0, rmode->fbWidth, rmode->xfbHeight,
	             rmode->fbWidth * 2);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_Flush();

	int sel = 0, top = 0;
	int stickCooldown = 0;
	DrawList(maps, count, sel, top);

	while (1) {
		VIDEO_WaitVSync();
		PAD_ScanPads();

		u32 down = PAD_ButtonsDown(0);
		int prevSel = sel;
		int move = 0;

		if (down & PAD_BUTTON_UP)   move = -1;
		if (down & PAD_BUTTON_DOWN) move = +1;

		int sy = PAD_StickY(0);
		if (stickCooldown > 0) stickCooldown--;
		if (sy > STICK_THRESH && stickCooldown == 0)  { move = -1; stickCooldown = STICK_REPEAT; }
		if (sy < -STICK_THRESH && stickCooldown == 0) { move = +1; stickCooldown = STICK_REPEAT; }
		if (sy <= STICK_THRESH && sy >= -STICK_THRESH) stickCooldown = 0;

		sel += move;
		if (sel < 0) sel = count - 1;
		if (sel >= count) sel = 0;

		if (down & PAD_BUTTON_A)     return sel;
		if (down & PAD_BUTTON_START) return -1;

		if (sel < top) top = sel;
		if (sel >= top + VISIBLE) top = sel - VISIBLE + 1;

		if (sel != prevSel)
			DrawList(maps, count, sel, top);
	}
}

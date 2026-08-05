#ifndef MSW_PAD_H
#define MSW_PAD_H

#include <gccore.h>

/* One controller, whatever it physically is.
 *
 * The GameCube build has exactly one answer to "where does input come from",
 * and libogc's PAD_* is it. The Wii build has three, because the console this
 * targets (an RVL-101) has no GameCube ports at all:
 *
 *   - a Wii U / Mayflash GameCube adapter on USB   (gcadapter.c)
 *   - the native controller ports                  (RVL-001 only)
 *   - a Classic Controller or Wii U Pro Controller (WPAD)
 *
 * Rather than teach the game about any of that, every backend is translated
 * into *GameCube* button bits and GameCube analog ranges -- so PAD_BUTTON_A is
 * the canonical name for "the A button" even on a Classic Controller, and
 * input.c and settings.c are the same code on both platforms. The two adapter
 * paths are a real GameCube pad anyway, so for them the translation is free and
 * the feel is identical to the console build; only the Classic Controller is a
 * genuine remap, and its layout is a straight overlay (see pad.c).
 *
 * Analog ranges match PAD_* exactly, because input.c's deadzones are tuned in
 * those units: sticks are -127..127 about a captured origin, triggers 0..255.
 */

#define MSW_BTN_A       PAD_BUTTON_A
#define MSW_BTN_B       PAD_BUTTON_B
#define MSW_BTN_X       PAD_BUTTON_X
#define MSW_BTN_Y       PAD_BUTTON_Y
#define MSW_BTN_L       PAD_TRIGGER_L
#define MSW_BTN_R       PAD_TRIGGER_R
#define MSW_BTN_Z       PAD_TRIGGER_Z
#define MSW_BTN_START   PAD_BUTTON_START
#define MSW_BTN_UP      PAD_BUTTON_UP
#define MSW_BTN_DOWN    PAD_BUTTON_DOWN
#define MSW_BTN_LEFT    PAD_BUTTON_LEFT
#define MSW_BTN_RIGHT   PAD_BUTTON_RIGHT

/* Call once, before anything samples. On Wii this also starts the USB adapter
 * hunt, which is asynchronous -- an adapter plugged in later still gets picked
 * up, so this never blocks waiting for one. */
void Pad_Init(void);

/* Once per rendered frame, before any of the readers below. Replaces the bare
 * PAD_ScanPads() call. */
void Pad_Scan(void);

u32 Pad_ButtonsHeld(int chan);
/* Press edges since the previous Pad_Scan. Computed here rather than taken from
 * PAD_ButtonsDown so that it means the same thing across all three backends. */
u32 Pad_ButtonsDown(int chan);

int Pad_StickX(int chan);
int Pad_StickY(int chan);
int Pad_SubStickX(int chan);
int Pad_SubStickY(int chan);
int Pad_TriggerL(int chan);
int Pad_TriggerR(int chan);

/* Which backend `chan` is currently being read from, for the boot banner and
 * for working out on real hardware why a controller is doing nothing. Returns
 * "none" when nothing is connected. */
const char *Pad_SourceName(int chan);

#endif

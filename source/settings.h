#ifndef MSW_SETTINGS_H
#define MSW_SETTINGS_H

#include <gccore.h>

/* Player settings: FOV, look sensitivity, view bobbing, auto-sprint and the
 * button map, changed from the command palette's Settings page.
 *
 * Everything here used to be a `#define`: the 60-degree FOV in main.c, the
 * `LOOK_SPEED 2.2f` in input.c, the button map in Input_Sample. That is the
 * right shape for a value the author picks once and the wrong shape for a value
 * the *player* picks, and the button map is the clearest case -- a pad has
 * twelve inputs, this game needs eleven of them, and which finger does which
 * job is not something a compile-time constant can be right about.
 *
 * One global, deliberately. There is one player on one controller port; passing
 * a Settings* through Input_Sample, HeldItem_Draw and the projection setup would
 * be threading a singleton through three subsystems to no end.
 *
 * **These live for the session only.** Memory card persistence was built and
 * working -- record format, checksum, validation, slot A/B, full save/load round
 * trip verified against a real card -- and then removed, because a *successful*
 * CARD_Mount anywhere in this program stops the world from rendering: the frame
 * clears to the sky colour, World_Draw still reports every chunk submitted, and
 * only the display-list geometry vanishes while the entire 2D HUD keeps drawing.
 * That was bisected to the mount call alone; where it sits relative to
 * VIDEO_Init/GX_Init, the mount retry's usleep, and the size of the work area
 * were each ruled out by their own build. Whatever the card driver and GX are
 * contending over, it is not this file's logic -- so persistence can come back
 * once that is understood, and the shape here does not need to change for it.
 *
 * The defaults are exactly what the constants they replaced were, with one
 * intentional exception: auto-sprint ships on. See Settings_Defaults.
 */

/* ---- bindable actions ---------------------------------------------------
 * The gameplay half of input.h's button map. Menu navigation is *not* here:
 * the D-pad, A, B and Start keep their menu duty unconditionally, because a
 * remapping screen you can rebind yourself out of is a screen that can brick
 * the controls with no way back. That is also why SET_BTN_* has no Start --
 * Start always opens and closes the command palette, so the settings page is
 * always reachable however badly the rest of the pad is bound. */
enum {
	SET_ACT_ATTACK = 0,   /* dig / attack     -- L by default */
	SET_ACT_USE,          /* place / use item -- Y            */
	SET_ACT_JUMP,         /* A                                */
	SET_ACT_SNEAK,        /* B                                */
	SET_ACT_SPRINT,       /* R                                */
	SET_ACT_INVENTORY,    /* X                                */
	SET_ACT_HOTBAR_PREV,  /* D-pad Left                       */
	SET_ACT_HOTBAR_NEXT,  /* D-pad Right                      */
	SET_ACT_MENU,         /* D-pad Up: the command palette    */
	SET_ACT_DROP,         /* D-pad Down                       */
	SET_ACT_CHAT,         /* Z held (offline: the freecam)     */
	SET_ACT_COUNT
};

/* Assignable physical inputs. Index 0 is "unbound", so a zeroed record binds
 * nothing rather than binding everything to A. */
enum {
	SET_BTN_NONE = 0,
	SET_BTN_A, SET_BTN_B, SET_BTN_X, SET_BTN_Y,
	SET_BTN_L, SET_BTN_R, SET_BTN_Z,
	SET_BTN_DUP, SET_BTN_DDOWN, SET_BTN_DLEFT, SET_BTN_DRIGHT,
	SET_BTN_COUNT
};

/* Vertical FOV in degrees. Minecraft's slider is 30..110 ("Quake Pro"); the
 * default is this engine's own 60 rather than vanilla's 70, so a player who
 * never opens the menu sees exactly the frame they saw before. */
#define SETTINGS_FOV_MIN   30
#define SETTINGS_FOV_MAX  110
#define SETTINGS_FOV_STEP   5
#define SETTINGS_FOV_DEF   60

/* Look sensitivity as a percentage of the base C-stick rate, 0.5x to 8x.
 * Stored as the percentage rather than as an index into the step table below,
 * so widening or re-spacing that table cannot silently change a saved value. */
#define SETTINGS_SENS_MIN   50
#define SETTINGS_SENS_MAX  800
#define SETTINGS_SENS_DEF  100

typedef struct {
	u16 fovDeg;                /* SETTINGS_FOV_MIN..MAX                     */
	u16 sensPct;               /* SETTINGS_SENS_MIN..MAX, 100 = unchanged   */
	u8  viewBob;               /* the held-item sway (see Settings_ViewBob) */
	u8  autoSprint;            /* sprint whenever walking forward           */
	u8  bind[SET_ACT_COUNT];   /* SET_ACT_* -> SET_BTN_*                    */
} Settings;

extern Settings g_settings;

/* Reset to the shipped map and the shipped values. */
void Settings_Defaults(void);

/* ---- mutators -----------------------------------------------------------
 * `dir` is -1 or +1: one press of D-pad Left or Right on the settings page.
 * FOV moves by SETTINGS_FOV_STEP and clamps; sensitivity walks a table of
 * useful values rather than a linear ramp, because 0.5x to 8x in even steps is
 * either too coarse at the bottom or an unreasonable number of presses at the
 * top. */
void Settings_StepFov(int dir);
void Settings_StepSens(int dir);
void Settings_ToggleViewBob(void);
void Settings_ToggleAutoSprint(void);
void Settings_StepBind(int act, int dir);
void Settings_DefaultBinds(void);

/* Multiplier to apply to the base look rate: sensPct / 100. */
float Settings_LookScale(void);

/* The PAD_* bit an action is bound to, or 0 when it is unbound. Analog L and R
 * are folded into their own bits by Input_Sample before the test, so a bind is
 * always a single mask compare. */
u32 Settings_ButtonMask(int btn);

/* Display names, for the settings pages. Both clamp rather than trap. */
const char *Settings_ButtonName(int btn);
const char *Settings_ActionName(int act);

/* True when another action is bound to the same (non-None) button. Not an
 * error -- vanilla allows it too -- but the page draws it in red. */
int Settings_BindConflict(int act);

#endif

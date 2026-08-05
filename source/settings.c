#include <stdio.h>
#include <string.h>

#include "pad.h"
#include "settings.h"

Settings g_settings;


/* ---- defaults ------------------------------------------------------------ */

/* The map input.c used to hard-code, one entry per action. Changing this
 * changes what "Reset to default" means and nothing else. */
static const u8 g_defBind[SET_ACT_COUNT] = {
	SET_BTN_L,        /* attack / dig    */
	SET_BTN_Y,        /* use / place     */
	SET_BTN_A,        /* jump            */
	SET_BTN_B,        /* sneak           */
	SET_BTN_R,        /* sprint          */
	SET_BTN_X,        /* inventory       */
	SET_BTN_DLEFT,    /* hotbar prev     */
	SET_BTN_DRIGHT,   /* hotbar next     */
	SET_BTN_DUP,      /* command palette */
	SET_BTN_DDOWN,    /* drop item       */
	SET_BTN_Z,        /* chat            */
};

void Settings_DefaultBinds(void) {
	memcpy(g_settings.bind, g_defBind, sizeof g_settings.bind);
}

void Settings_Defaults(void) {
	memset(&g_settings, 0, sizeof g_settings);
	g_settings.fovDeg  = SETTINGS_FOV_DEF;
	g_settings.sensPct = SETTINGS_SENS_DEF;
	g_settings.viewBob = 1;
	/* On by default, and the one place these defaults are not simply "what the
	 * constants were". Holding R for the whole of a skywars round is not a
	 * choice anyone makes twice, and this engine's sprint is already gated on
	 * walking forward -- so auto-sprint costs nothing except the ability to
	 * walk slowly, which sneak still does. */
	g_settings.autoSprint = 1;
	memcpy(g_settings.bind, g_defBind, sizeof g_settings.bind);
}

/* ---- mutators ----------------------------------------------------------- */

void Settings_StepFov(int dir) {
	int v = g_settings.fovDeg + dir * SETTINGS_FOV_STEP;
	if (v < SETTINGS_FOV_MIN) v = SETTINGS_FOV_MIN;
	if (v > SETTINGS_FOV_MAX) v = SETTINGS_FOV_MAX;
	g_settings.fovDeg = (u16)v;
}

/* 0.5x to 8x: fine where a small change is noticeable, coarse where it is not.
 * Thirteen stops rather than a 25%-per-press ramp, which would be thirty
 * presses to cross the range. */
static const u16 g_sensStep[] = {
	50, 75, 100, 125, 150, 175, 200, 250, 300, 400, 500, 600, 800
};
#define SENS_STEPS ((int)(sizeof g_sensStep / sizeof g_sensStep[0]))

void Settings_StepSens(int dir) {
	/* Nearest stop rather than the exact one: a value loaded from an older
	 * table (or a future one) still steps somewhere sensible. */
	int i, best = 0, bestd = 0x7FFF;
	for (i = 0; i < SENS_STEPS; i++) {
		int d = (int)g_sensStep[i] - (int)g_settings.sensPct;
		if (d < 0) d = -d;
		if (d < bestd) { bestd = d; best = i; }
	}
	best += dir;
	if (best < 0) best = 0;
	if (best >= SENS_STEPS) best = SENS_STEPS - 1;
	g_settings.sensPct = g_sensStep[best];
}

void Settings_ToggleViewBob(void) {
	g_settings.viewBob = !g_settings.viewBob;
}

void Settings_ToggleAutoSprint(void) {
	g_settings.autoSprint = !g_settings.autoSprint;
}

void Settings_StepBind(int act, int dir) {
	if (act < 0 || act >= SET_ACT_COUNT) return;
	int b = g_settings.bind[act] + dir;
	while (b < 0) b += SET_BTN_COUNT;
	while (b >= SET_BTN_COUNT) b -= SET_BTN_COUNT;
	g_settings.bind[act] = (u8)b;
}

/* Clamped for the same reason UpdateProjection clamps: a sensPct of 0 would
 * multiply the look rate to nothing and leave the player unable to turn, with no
 * on-screen sign of why. An out-of-range setting reads as the default rather than
 * as a dead control. */
float Settings_LookScale(void) {
	u16 pct = g_settings.sensPct;
	if (pct < SETTINGS_SENS_MIN || pct > SETTINGS_SENS_MAX) pct = SETTINGS_SENS_DEF;
	return (float)pct / 100.0f;
}

/* ---- names and masks ---------------------------------------------------- */

static const struct { u32 mask; const char *name; } g_btn[SET_BTN_COUNT] = {
	{ 0,                 "None"    },
	{ MSW_BTN_A,         "A"       },
	{ MSW_BTN_B,         "B"       },
	{ MSW_BTN_X,         "X"       },
	{ MSW_BTN_Y,         "Y"       },
	{ MSW_BTN_L,         "L"       },
	{ MSW_BTN_R,         "R"       },
	{ MSW_BTN_Z,         "Z"       },
	{ MSW_BTN_UP,        "D-Up"    },
	{ MSW_BTN_DOWN,      "D-Down"  },
	{ MSW_BTN_LEFT,      "D-Left"  },
	{ MSW_BTN_RIGHT,     "D-Right" },
};

static const char *const g_actName[SET_ACT_COUNT] = {
	"Attack/dig", "Use/place", "Jump", "Sneak", "Sprint", "Inventory",
	"Hotbar prev", "Hotbar next", "Menu", "Drop item", "Chat",
};

u32 Settings_ButtonMask(int btn) {
	if (btn <= SET_BTN_NONE || btn >= SET_BTN_COUNT) return 0;
	return g_btn[btn].mask;
}

const char *Settings_ButtonName(int btn) {
	if (btn < 0 || btn >= SET_BTN_COUNT) return "?";
	return g_btn[btn].name;
}

const char *Settings_ActionName(int act) {
	if (act < 0 || act >= SET_ACT_COUNT) return "?";
	return g_actName[act];
}

int Settings_BindConflict(int act) {
	if (act < 0 || act >= SET_ACT_COUNT) return 0;
	int b = g_settings.bind[act], i;
	if (b == SET_BTN_NONE) return 0;
	for (i = 0; i < SET_ACT_COUNT; i++)
		if (i != act && g_settings.bind[i] == b) return 1;
	return 0;
}

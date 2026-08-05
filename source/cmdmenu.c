#include <string.h>
#include <stdio.h>

#include "cmdmenu.h"
#include "hud.h"
#include "settings.h"
#include "maps_gen.h"

/* Submenus. SUB_SETTINGS and SUB_CONTROLS are the two "options" pages: they
 * carry a value column and are adjusted with D-pad Left/Right rather than
 * picked with A, which is why level_row() reports a label and a value instead
 * of just a string. */
enum { SUB_NONE = 0, SUB_MAP, SUB_TEAM, SUB_SAY, SUB_SETTINGS, SUB_CONTROLS };

/* Root entries. Order is the order they are drawn in; the two destructive
 * ones sit at the bottom so a mis-timed A press lands on "Kit" rather than on
 * "Disconnect". */
enum {
	ROOT_JOIN = 0, ROOT_TEAM, ROOT_SAY, ROOT_START, ROOT_LEAVE, ROOT_KIT,
	ROOT_SETTINGS, ROOT_PERF, ROOT_RECONNECT, ROOT_DISCONNECT,
	ROOT_COUNT
};

static const char *const g_root[ROOT_COUNT] = {
	"Join map    >",
	"Team        >",
	"Say         >",
	"Start game",
	"Leave game",
	"Re-give kit",
	"Settings    >",
	"Perf overlay",
	"Reconnect",
	"Disconnect",
};

/* Rows of the settings page. */
enum {
	SET_FOV = 0, SET_BOB, SET_SPRINT, SET_SENS, SET_CONTROLS, SET_RESET,
	SET_ROW_COUNT
};

/* Rows of the controls page: one per bindable action, then a reset. */
#define CTL_ROW_COUNT (SET_ACT_COUNT + 1)
#define CTL_RESET      SET_ACT_COUNT

/* Ten lines that cover most of what anyone types during a skywars round. Free
 * text would need a 6x8 character grid and about a minute per sentence; this
 * is one D-pad press and it is what the pad is actually good at. */
static const char *const g_say[] = {
	"gg", "gl hf", "rush mid", "help!", "sorry",
	"nice", "team?", "im low", "watch out", "ez",
};
#define SAY_COUNT ((int)(sizeof(g_say) / sizeof(g_say[0])))

#define TEAM_COUNT 8

/* Rows of a list drawn at once. The map list is 33 long and the screen is
 * 480p; the cursor scrolls a window over it exactly as menu.c does. */
#define VISIBLE_ROWS 10

/* Maps that are in the DOL but are not a game you can join: the hub, and the
 * synthetic block-model gallery. Offering either would send the server a
 * command it can only answer with an error. */
static int joinable(const char *name) {
	return strcmp(name, "Spawn") != 0 && strcmp(name, "Model Gallery") != 0;
}

/* Display name -> the plugin's map key: lower case, spaces to underscores.
 * MapManager registers "mega_aegis" where compress_worlds.py titled it "Mega
 * Aegis", and the two have agreed on every map since; the conversion is the
 * whole of the difference. */
static void map_key(const char *name, char *out, int cap) {
	int i = 0;
	for (; name[i] && i < cap - 1; i++) {
		char c = name[i];
		if (c == ' ') c = '_';
		else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		out[i] = c;
	}
	out[i] = '\0';
}

/* The n'th joinable map's index in g_maps[], or -1. */
static int nth_joinable(int n) {
	int i;
	for (i = 0; i < MAP_COUNT; i++) {
		if (!joinable(g_maps[i].name)) continue;
		if (n-- == 0) return i;
	}
	return -1;
}

static int joinable_count(void) {
	int i, n = 0;
	for (i = 0; i < MAP_COUNT; i++) if (joinable(g_maps[i].name)) n++;
	return n;
}

/* ---- levels -------------------------------------------------------------
 * One accessor set over the whole stack, so the draw code has no idea which
 * page it is looking at. */

static int cur_kind(const CmdMenu *m) {
	return (m->depth == 0) ? SUB_NONE : m->kind[m->depth];
}

static int level_count(const CmdMenu *m) {
	switch (cur_kind(m)) {
	case SUB_NONE:     return ROOT_COUNT;
	case SUB_MAP:      return joinable_count();
	case SUB_TEAM:     return TEAM_COUNT;
	case SUB_SAY:      return SAY_COUNT;
	case SUB_SETTINGS: return SET_ROW_COUNT;
	case SUB_CONTROLS: return CTL_ROW_COUNT;
	default:           return 0;
	}
}

static const char *level_title(const CmdMenu *m) {
	switch (cur_kind(m)) {
	case SUB_MAP:      return "JOIN MAP";
	case SUB_TEAM:     return "TEAM";
	case SUB_SAY:      return "SAY";
	case SUB_SETTINGS: return "SETTINGS";
	case SUB_CONTROLS: return "CONTROLS";
	default:           return "COMMANDS";
	}
}

/* The settings pages are adjusted rather than picked, so they say so. */
static const char *level_footer(const CmdMenu *m) {
	switch (cur_kind(m)) {
	case SUB_SETTINGS: return "L/R set  A pick  B back";
	case SUB_CONTROLS: return "L/R rebind  B back";
	default:           return "A pick  B back  Start close";
	}
}

/* Is this page one of the settings pages? Leaving them is the point at which
 * anything changed gets written to the card. */
static int settings_kind(int kind) {
	return kind == SUB_SETTINGS || kind == SUB_CONTROLS;
}

/* Row `i` of the current level: its label, and the value column (empty when the
 * row has none). Both buffers are caller-owned; `label` may be pointed at a
 * constant instead, so the return value is what to draw. */
static const char *level_row(const CmdMenu *m, int i,
                             char *label, int lcap, char *value, int vcap) {
	value[0] = '\0';
	switch (cur_kind(m)) {
	case SUB_NONE:
		return g_root[i];
	case SUB_MAP: {
		int idx = nth_joinable(i);
		return (idx >= 0) ? g_maps[idx].name : "?";
	}
	case SUB_TEAM:
		snprintf(label, lcap, "Team %d", i + 1);
		return label;
	case SUB_SAY:
		return g_say[i];
	case SUB_SETTINGS:
		switch (i) {
		case SET_FOV:
			snprintf(value, vcap, "%d", g_settings.fovDeg);
			return "FOV";
		case SET_BOB:
			snprintf(value, vcap, "%s", g_settings.viewBob ? "On" : "Off");
			return "View bobbing";
		case SET_SPRINT:
			snprintf(value, vcap, "%s", g_settings.autoSprint ? "On" : "Off");
			return "Auto sprint";
		case SET_SENS:
			/* Two decimals without pulling float formatting into the DOL:
			 * the setting is a percentage and always was. */
			snprintf(value, vcap, "%d.%02dx",
			         g_settings.sensPct / 100, g_settings.sensPct % 100);
			return "Sensitivity";
		case SET_CONTROLS: snprintf(value, vcap, ">"); return "Controls";
		default:           return "Reset defaults";
		}
	case SUB_CONTROLS:
		if (i == CTL_RESET) return "Reset controls";
		snprintf(value, vcap, "%s", Settings_ButtonName(g_settings.bind[i]));
		return Settings_ActionName(i);
	default:
		return "?";
	}
}

void CmdMenu_Init(CmdMenu *m) {
	memset(m, 0, sizeof(*m));
}

static void say(CmdMenu *m, const char *text) {
	snprintf(m->pending, sizeof m->pending, "%s", text);
	m->open = 0;
	m->depth = 0;
}

static void enter_sub(CmdMenu *m, int kind) {
	if (m->depth + 1 >= CMDMENU_DEPTH) return;
	m->depth++;
	m->kind[m->depth] = kind;
	m->sel[m->depth] = 0;
}

static void confirm_root(CmdMenu *m) {
	switch (m->sel[0]) {
	case ROOT_JOIN:  enter_sub(m, SUB_MAP);  break;
	case ROOT_TEAM:  enter_sub(m, SUB_TEAM); break;
	case ROOT_SAY:   enter_sub(m, SUB_SAY);  break;
	case ROOT_START: say(m, "/start"); break;
	case ROOT_LEAVE: say(m, "/leave"); break;
	case ROOT_KIT:   say(m, "/kit");   break;
	case ROOT_SETTINGS:   enter_sub(m, SUB_SETTINGS); break;
	case ROOT_PERF:       m->pendingAction = CMDMENU_ACT_TOGGLE_PERF; m->open = 0; break;
	case ROOT_RECONNECT:  m->pendingAction = CMDMENU_ACT_RECONNECT;   m->open = 0; break;
	case ROOT_DISCONNECT: m->pendingAction = CMDMENU_ACT_DISCONNECT;  m->open = 0; break;
	default: break;
	}
}

static void confirm_sub(CmdMenu *m) {
	int sel = m->sel[m->depth];
	/* Sized for what fits after "/join ", so the map key can never be the
	 * thing that gets truncated. */
	char key[CMDMENU_TEXT_MAX - 8];
	switch (cur_kind(m)) {
	case SUB_MAP: {
		int idx = nth_joinable(sel);
		if (idx < 0) return;
		map_key(g_maps[idx].name, key, sizeof key);
		snprintf(m->pending, sizeof m->pending, "/join %s", key);
		break;
	}
	case SUB_TEAM:
		snprintf(m->pending, sizeof m->pending, "/team %d", sel + 1);
		break;
	case SUB_SAY:
		snprintf(m->pending, sizeof m->pending, "%s", g_say[sel]);
		break;
	case SUB_SETTINGS:
		/* A on a toggle flips it -- Left/Right is the general idiom on this
		 * page, but nobody reaches for it to switch On to Off. The two ramps
		 * ignore A rather than guessing a direction. */
		switch (sel) {
		case SET_BOB:      Settings_ToggleViewBob();    break;
		case SET_SPRINT:   Settings_ToggleAutoSprint(); break;
		case SET_CONTROLS: enter_sub(m, SUB_CONTROLS);  break;
		case SET_RESET:    Settings_Defaults();         break;
		default: break;
		}
		return;   /* the settings pages never close on A */
	case SUB_CONTROLS:
		if (sel == CTL_RESET) Settings_DefaultBinds();
		return;
	default:
		return;
	}
	m->open = 0;
	m->depth = 0;
}

/* D-pad Left/Right on a settings row. The same presses that step a cursor
 * left and right on every other page -- there is nothing horizontal to move to
 * on a one-column list, so the value column gets them. */
static void adjust(CmdMenu *m, int dir) {
	int sel = m->sel[m->depth];
	switch (cur_kind(m)) {
	case SUB_SETTINGS:
		switch (sel) {
		case SET_FOV:    Settings_StepFov(dir);  break;
		case SET_SENS:   Settings_StepSens(dir); break;
		case SET_BOB:    Settings_ToggleViewBob();    break;
		case SET_SPRINT: Settings_ToggleAutoSprint(); break;
		default: break;
		}
		break;
	case SUB_CONTROLS:
		if (sel < SET_ACT_COUNT) Settings_StepBind(sel, dir);
		break;
	default:
		break;
	}
}

int CmdMenu_Update(CmdMenu *m, const PlayerInput *in) {
	if (!m->open) {
		if (in->menu) { m->open = 1; m->depth = 0; }
		return m->open;
	}

	/* Start closes from anywhere: it is the button someone reaches for when
	 * they want out of a menu, and there is nothing else it does while one is
	 * up. */
	if (in->pause) {
		m->open = 0;
		m->depth = 0;
	} else {
		int n = level_count(m);
		if (n < 1) n = 1;
		int *sel = &m->sel[m->depth];

		if (in->navY) {
			*sel += in->navY;
			while (*sel < 0)  *sel += n;
			while (*sel >= n) *sel -= n;
		}
		if (in->navX) adjust(m, (in->navX > 0) ? 1 : -1);

		if (in->cancel) {
			if (m->depth > 0) m->depth--;
			else m->open = 0;
		} else if (in->confirm) {
			if (m->depth == 0) confirm_root(m);
			else               confirm_sub(m);
		}
	}

	return m->open;
}

const char *CmdMenu_TakeChat(CmdMenu *m) {
	if (!m->pending[0]) return NULL;
	/* Returned out of the struct rather than copied: the caller sends it
	 * immediately and the next Update is the earliest anything can overwrite
	 * it. Cleared through a static so the pointer stays valid across that. */
	static char out[CMDMENU_TEXT_MAX];
	memcpy(out, m->pending, sizeof out);
	m->pending[0] = '\0';
	return out;
}

int CmdMenu_TakeAction(CmdMenu *m) {
	int a = m->pendingAction;
	m->pendingAction = CMDMENU_ACT_NONE;
	return a;
}

/* ---- drawing ------------------------------------------------------------
 * Centred, over a dimmed panel. Not top-left, where the perf overlay lives:
 * two stacks of white text over the same corner of a 480p frame are
 * individually legible and together unreadable. */

#define ROW_H     10
#define PANEL_W  124
#define PANEL_W2 176   /* the settings pages: a label and a value column */
#define PAD        6

static void row(const char *text, const char *value, int x, int y, int right,
                int selected, int warn) {
	u32 c = selected ? 0xFFFF55FFu : 0xC0C0C0FFu;
	if (selected) Hud_DrawStringShadow(">", x - 8, y, c);
	Hud_DrawStringShadow(text, x, y, c);
	if (value && value[0]) {
		/* A duplicate binding is legal -- vanilla allows it too -- but it is
		 * almost never what someone meant, so it is said in red rather than
		 * refused. */
		Hud_DrawStringShadow(value, right - Hud_StringWidth(value), y,
		                     warn ? 0xFF5555FFu : c);
	}
}

void CmdMenu_Draw(const CmdMenu *m, int fbWidth, int efbHeight) {
	if (!m->open) return;

	char label[CMDMENU_TEXT_MAX], value[16];
	HudScreen sc = Hud_Begin2D(fbWidth, efbHeight);

	int kind = cur_kind(m);
	int n = level_count(m);
	if (n < 1) n = 1;
	int sel = m->sel[m->depth];

	int rows = (n > VISIBLE_ROWS) ? VISIBLE_ROWS : n;
	int panelW = settings_kind(kind) ? PANEL_W2 : PANEL_W;
	int panelH = PAD * 2 + (ROW_H + 2) + rows * ROW_H + ROW_H;
	int px = (int)(sc.w / 2) - panelW / 2;
	/* A little below centre: the perf overlay owns the top-left corner and its
	 * nine lines reach past the middle of a 480p frame. */
	int py = (int)(sc.h * 0.58f) - panelH / 2;

	/* Panels first: Hud_DrawString leaves the font bound and the TEV stage in
	 * modulate mode, so a flat quad after one would come out textured. */
	Hud_DrawPanel((float)px, (float)py, (float)panelW, (float)panelH,
	              0x000000C0u);

	int x = px + PAD + 10, y = py + PAD;
	int right = px + panelW - PAD;

	Hud_DrawStringShadow(level_title(m), px + PAD, y, 0xFFFFFFFFu);
	/* Scroll position, on the lists long enough to need it. */
	if (n > VISIBLE_ROWS) {
		snprintf(label, sizeof label, "%d/%d", sel + 1, n);
		Hud_DrawStringShadow(label, right - Hud_StringWidth(label), y,
		                     0x808080FFu);
	}
	y += ROW_H + 2;

	/* Window the list around the cursor, the same scroll menu.c uses. */
	int first = sel - VISIBLE_ROWS / 2;
	if (first > n - VISIBLE_ROWS) first = n - VISIBLE_ROWS;
	if (first < 0) first = 0;

	int i;
	for (i = first; i < n && i < first + VISIBLE_ROWS; i++, y += ROW_H) {
		const char *text = level_row(m, i, label, sizeof label,
		                             value, sizeof value);
		int warn = (kind == SUB_CONTROLS) && i < SET_ACT_COUNT &&
		           Settings_BindConflict(i);
		row(text, value, x, y, right, i == sel, warn);
	}

	Hud_DrawStringShadow(level_footer(m), px + PAD, y + 2, 0x808080FFu);
	Hud_End2D();
}

#include <string.h>
#include <stdio.h>

#include "cmdmenu.h"
#include "hud.h"
#include "maps_gen.h"

/* Submenus. */
enum { SUB_NONE = 0, SUB_MAP, SUB_TEAM, SUB_SAY };

/* Root entries. Order is the order they are drawn in; the two destructive
 * ones sit at the bottom so a mis-timed A press lands on "Kit" rather than on
 * "Disconnect". */
enum {
	ROOT_JOIN = 0, ROOT_TEAM, ROOT_SAY, ROOT_START, ROOT_LEAVE, ROOT_KIT,
	ROOT_PERF, ROOT_RECONNECT, ROOT_DISCONNECT,
	ROOT_COUNT
};

static const char *const g_root[ROOT_COUNT] = {
	"Join map    >",
	"Team        >",
	"Say         >",
	"Start game",
	"Leave game",
	"Re-give kit",
	"Perf overlay",
	"Reconnect",
	"Disconnect",
};

/* Ten lines that cover most of what anyone types during a skywars round. Free
 * text would need a 6x8 character grid and about a minute per sentence; this
 * is one D-pad press and it is what the pad is actually good at. */
static const char *const g_say[] = {
	"gg", "gl hf", "rush mid", "help!", "sorry",
	"nice", "team?", "im low", "watch out", "ez",
};
#define SAY_COUNT ((int)(sizeof(g_say) / sizeof(g_say[0])))

#define TEAM_COUNT 8

/* Rows of a submenu drawn at once. The map list is 33 long and the screen is
 * 480p; the cursor scrolls a window over it exactly as menu.c does. */
#define VISIBLE_ROWS 9

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

static int sub_count(int kind) {
	switch (kind) {
	case SUB_MAP:  return joinable_count();
	case SUB_TEAM: return TEAM_COUNT;
	case SUB_SAY:  return SAY_COUNT;
	default:       return 0;
	}
}

void CmdMenu_Init(CmdMenu *m) {
	memset(m, 0, sizeof(*m));
}

static void say(CmdMenu *m, const char *text) {
	snprintf(m->pending, sizeof m->pending, "%s", text);
	m->open = 0;
	m->level = 0;
}

static void enter_sub(CmdMenu *m, int kind) {
	m->level = 1;
	m->subKind = kind;
	m->subSel = 0;
}

static void confirm_root(CmdMenu *m) {
	switch (m->rootSel) {
	case ROOT_JOIN:  enter_sub(m, SUB_MAP);  break;
	case ROOT_TEAM:  enter_sub(m, SUB_TEAM); break;
	case ROOT_SAY:   enter_sub(m, SUB_SAY);  break;
	case ROOT_START: say(m, "/start"); break;
	case ROOT_LEAVE: say(m, "/leave"); break;
	case ROOT_KIT:   say(m, "/kit");   break;
	case ROOT_PERF:       m->pendingAction = CMDMENU_ACT_TOGGLE_PERF; m->open = 0; break;
	case ROOT_RECONNECT:  m->pendingAction = CMDMENU_ACT_RECONNECT;   m->open = 0; break;
	case ROOT_DISCONNECT: m->pendingAction = CMDMENU_ACT_DISCONNECT;  m->open = 0; break;
	default: break;
	}
}

static void confirm_sub(CmdMenu *m) {
	/* Sized for what fits after "/join ", so the map key can never be the
	 * thing that gets truncated. */
	char key[CMDMENU_TEXT_MAX - 8];
	switch (m->subKind) {
	case SUB_MAP: {
		int idx = nth_joinable(m->subSel);
		if (idx < 0) return;
		map_key(g_maps[idx].name, key, sizeof key);
		snprintf(m->pending, sizeof m->pending, "/join %s", key);
		break;
	}
	case SUB_TEAM:
		snprintf(m->pending, sizeof m->pending, "/team %d", m->subSel + 1);
		break;
	case SUB_SAY:
		snprintf(m->pending, sizeof m->pending, "%s", g_say[m->subSel]);
		break;
	default:
		return;
	}
	m->open = 0;
	m->level = 0;
}

int CmdMenu_Update(CmdMenu *m, const PlayerInput *in) {
	if (!m->open) {
		if (in->menu) { m->open = 1; m->level = 0; }
		return m->open;
	}

	/* Start closes from anywhere: it is the button someone reaches for when
	 * they want out of a menu, and there is nothing else it does while one is
	 * up. */
	if (in->pause) { m->open = 0; m->level = 0; return 0; }

	int n = (m->level == 0) ? ROOT_COUNT : sub_count(m->subKind);
	if (n < 1) n = 1;
	int *sel = (m->level == 0) ? &m->rootSel : &m->subSel;

	if (in->navY) {
		*sel += in->navY;
		while (*sel < 0)  *sel += n;
		while (*sel >= n) *sel -= n;
	}

	if (in->cancel) {
		if (m->level == 1) m->level = 0;
		else m->open = 0;
		return m->open;
	}
	if (in->confirm) {
		if (m->level == 0) confirm_root(m);
		else               confirm_sub(m);
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

#define ROW_H    10
#define PANEL_W 124
#define PAD      6

static void row(const char *text, int x, int y, int selected) {
	if (selected) {
		Hud_DrawStringShadow(">", x - 8, y, 0xFFFF55FFu);
		Hud_DrawStringShadow(text, x, y, 0xFFFF55FFu);
	} else {
		Hud_DrawStringShadow(text, x, y, 0xC0C0C0FFu);
	}
}

void CmdMenu_Draw(const CmdMenu *m, int fbWidth, int efbHeight) {
	if (!m->open) return;

	char line[CMDMENU_TEXT_MAX];
	HudScreen sc = Hud_Begin2D(fbWidth, efbHeight);

	int rows = (m->level == 0) ? ROOT_COUNT : sub_count(m->subKind);
	if (rows > VISIBLE_ROWS) rows = VISIBLE_ROWS;
	if (rows < 1) rows = 1;

	int panelH = PAD * 2 + (ROW_H + 2) + rows * ROW_H + ROW_H;
	int px = (int)(sc.w / 2) - PANEL_W / 2;
	/* A little below centre: the perf overlay owns the top-left corner and its
	 * nine lines reach past the middle of a 480p frame. */
	int py = (int)(sc.h * 0.58f) - panelH / 2;

	/* Panels first: Hud_DrawString leaves the font bound and the TEV stage in
	 * modulate mode, so a flat quad after one would come out textured. */
	Hud_DrawPanel((float)px, (float)py, (float)PANEL_W, (float)panelH,
	              0x000000C0u);

	int x = px + PAD + 10, y = py + PAD;

	if (m->level == 0) {
		Hud_DrawStringShadow("COMMANDS", px + PAD, y, 0xFFFFFFFFu);
		y += ROW_H + 2;
		int i;
		for (i = 0; i < ROOT_COUNT; i++, y += ROW_H)
			row(g_root[i], x, y, i == m->rootSel);
	} else {
		int n = sub_count(m->subKind);
		/* Window the list around the cursor, the same scroll menu.c uses. */
		int first = m->subSel - VISIBLE_ROWS / 2;
		if (first > n - VISIBLE_ROWS) first = n - VISIBLE_ROWS;
		if (first < 0) first = 0;

		const char *title = (m->subKind == SUB_MAP)  ? "JOIN MAP"
		                  : (m->subKind == SUB_TEAM) ? "TEAM" : "SAY";
		Hud_DrawStringShadow(title, px + PAD, y, 0xFFFFFFFFu);
		if (n > VISIBLE_ROWS) {
			snprintf(line, sizeof line, "%d/%d", m->subSel + 1, n);
			Hud_DrawStringShadow(line,
			                     px + PANEL_W - PAD - Hud_StringWidth(line), y,
			                     0x808080FFu);
		}
		y += ROW_H + 2;

		int i;
		for (i = first; i < n && i < first + VISIBLE_ROWS; i++, y += ROW_H) {
			const char *text = line;
			switch (m->subKind) {
			case SUB_MAP: {
				int idx = nth_joinable(i);
				text = (idx >= 0) ? g_maps[idx].name : "?";
				break;
			}
			case SUB_TEAM:
				snprintf(line, sizeof line, "Team %d", i + 1);
				break;
			default:
				text = g_say[i];
				break;
			}
			row(text, x, y, i == m->subSel);
		}
	}

	Hud_DrawStringShadow("A pick  B back  Start close",
	                     px + PAD, y + 2, 0x808080FFu);
	Hud_End2D();
}

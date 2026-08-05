#ifndef MSW_CMDMENU_H
#define MSW_CMDMENU_H

#include <gccore.h>
#include "input.h"

/* The command palette: how someone on a GameCube actually starts a game.
 *
 * Joining a MegaSkywars match normally means clicking a 54-slot chest GUI, and
 * a client that implemented the C0D/C0E/C0F window protocol to do that would
 * still need a mouse. But `/join <map>`, `/team <n>`, `/start`, `/leave` and
 * `/kit` are all plain chat commands, so a client that can send C01 never has
 * to open a window at all. This is a D-pad menu that composes those five
 * strings, drawn with Hud_DrawString in the existing ortho pass -- no new GX
 * state, no free-text entry, and nothing that needs a keyboard.
 *
 * It doubles as the pause menu, because a pause menu is the same thing: a
 * list, a cursor and a confirm button. It is also where the settings live
 * (settings.h), which is what made it three levels deep rather than two.
 */

/* Non-chat outcomes -- things the palette asks main.c to do rather than
 * things it asks the server to do. */
enum {
	CMDMENU_ACT_NONE = 0,
	CMDMENU_ACT_DISCONNECT,
	CMDMENU_ACT_RECONNECT,
	CMDMENU_ACT_TOGGLE_PERF,
	CMDMENU_ACT_TOGGLE_TAGS,
};

/* Longest command this composes: "/join " plus a map key. */
#define CMDMENU_TEXT_MAX 48

/* Root, a submenu, and the controls page inside the settings submenu. */
#define CMDMENU_DEPTH 3

typedef struct {
	int open;
	/* A cursor stack rather than the rootSel/subSel pair this used to be:
	 * backing out of Controls has to land on the settings row it came from, not
	 * at the top of the list. depth 0 is the root, where kind[] is unused. */
	int depth;
	int sel[CMDMENU_DEPTH];
	int kind[CMDMENU_DEPTH];

	char pending[CMDMENU_TEXT_MAX];  /* a chat line to send, "" = none */
	int  pendingAction;              /* CMDMENU_ACT_*                  */
} CmdMenu;

void CmdMenu_Init(CmdMenu *m);

/* Feed it a frame of input. Returns 1 while the palette has focus, which is
 * the caller's cue to hand gameplay a cleared PlayerInput -- A and B are the
 * confirm/cancel buttons here and jump/sneak everywhere else, and both cannot
 * be true at once. */
int CmdMenu_Update(CmdMenu *m, const PlayerInput *in);

/* Draw the palette, in its own 2D pass. No-op while closed. */
void CmdMenu_Draw(const CmdMenu *m, int fbWidth, int efbHeight);

/* Take the composed command, or NULL when there is none. Clears it. */
const char *CmdMenu_TakeChat(CmdMenu *m);

/* Take the queued CMDMENU_ACT_*, or CMDMENU_ACT_NONE. Clears it. */
int CmdMenu_TakeAction(CmdMenu *m);

#endif

#ifndef MSW_INPUT_H
#define MSW_INPUT_H

#include <gccore.h>

/* PlayerInput -- every gameplay control, sampled once and read by everything
 * else.
 *
 * Before this existed, `Player_Look` and `Player_Tick` called `PAD_SubStickX`,
 * `PAD_StickX`, `PAD_ButtonsHeld` and `PAD_TriggerR` directly, and sneak was a
 * function-local `int` that never reached the struct. That is fine for one
 * player at one controller port and impossible for anything else: a scripted
 * test cannot drive the player, the inventory screen has to be gated by a
 * `frozen` flag threaded through the physics, and -- the reason this is the
 * keystone of the playable milestone -- there is nothing to turn into a packet.
 * The 20 Hz movement message the server wants is a function of exactly these
 * fields, so they have to exist as data before they can be sent.
 *
 * The fields are Minecraft's, not the pad's. `moveForward`/`moveStrafe` are
 * `MovementInputFromOptions`'s +/-1, `attackHeld`/`useHeld` are the two mouse
 * buttons, and the button map is an implementation detail of `Input_Sample` --
 * the only function in the codebase that may read `PAD_*` for gameplay.
 *
 * ---- the button map (all of it; there are no spare inputs) ----------------
 *   L         attack / dig        (analog or the click at the bottom)
 *   Y         use / place
 *   R         sprint              (analog or the click)
 *   A         jump                / confirm in a menu
 *   B         sneak               / cancel in a menu
 *   X         inventory screen
 *   D-pad L/R held hotbar slot    / menu left-right
 *   D-pad Up  command palette     (T23)
 *   D-pad Dn  drop item           (offline: the perf overlay toggle)
 *   Z         hold to show chat   (offline: the debug free-fly camera)
 *   Start     pause menu
 */

/* Minecraft.leftClickCounter: ten ticks of lockout after a click that hit
 * nothing, which is what stops a held attack button from machine-gunning the
 * server with swings. It gates attacks against blocks *and* entities, so it
 * lives here rather than in interact.c -- both consumers see one already-gated
 * `attackHeld`. */
#define INPUT_MISS_LOCKOUT 10

typedef struct {
	/* MovementInputFromOptions: digitised to exactly -1, 0 or +1. Keeping the
	 * digitisation (rather than passing the analog magnitude through) is what
	 * reproduces keyboard-exact walk speed, and it is the strongest single
	 * anti-rubberband measure in the codebase -- a stick that tops out at 0.8
	 * of full deflection walks at 0.8 of the speed the server predicts. */
	float moveForward, moveStrafe;

	/* Degrees to turn this rendered frame. Look runs at the video rate, not the
	 * tick rate, so this is not part of the 20 Hz state. */
	float dYaw, dPitch;

	u8 jump, sneak, sprintHeld;

	/* The two mouse buttons, already gated by the miss lockout. */
	u8 attackHeld, useHeld;
	/* ...and their press edges. Accumulated across rendered frames and consumed
	 * by one tick: at 60 Hz video and 20 Hz ticks two frames in three run no
	 * tick at all, so an edge that was not latched would be dropped a third of
	 * the time. */
	u8 attackEdge, useEdge;

	/* Held-slot scroll: InventoryPlayer.changeCurrentItem's direction, +1 for
	 * the slot to the left. There is deliberately no absolute counterpart --
	 * the only thing that ever says "slot 4" outright is the server's S09, and
	 * that arrives on the link, not on the pad. */
	s8 hotbarDelta;

	/* Edges, one press each. `navX`/`navY` are the D-pad as menu steps (+X
	 * right, +Y down) and are the same presses `hotbarDelta`, `menu` and
	 * `dropItem` come from -- whichever consumer has focus picks. */
	s8 navX, navY;
	u8 dropItem;    /* D-pad Down                                        */
	u8 menu;        /* D-pad Up: the command palette                     */
	u8 openInv;     /* X                                                 */
	u8 confirm;     /* A, as a menu button                               */
	u8 cancel;      /* B, as a menu button                               */
	u8 pause;       /* Start                                             */
	u8 debug;       /* Z press edge -- offline free-fly only             */

	u8 showChat;    /* Z held (level, not an edge)                       */

	/* ---- internals -------------------------------------------------------- */
	u32 prevHeld;      /* for the edge detection                            */
	u8  attackRaw;     /* ungated, so releasing clears the lockout          */
	int leftClickCounter;
} PlayerInput;

/* Zero everything, including the internals. Call once per session. */
void Input_Init(PlayerInput *in);

/* Read controller `chan` and fill `in`. The only gameplay PAD_* reader in the
 * codebase; call it once per rendered frame, right after PAD_ScanPads. Edges
 * are OR'd in rather than replaced, so a press survives until something
 * consumes it. */
void Input_Sample(PlayerInput *in, int chan);

/* Gate every gameplay control off, keeping the internals. This replaces the
 * `frozen` flag Player_Tick used to take: main.c hands the physics a cleared
 * input while the inventory screen is up, which suppresses control exactly as
 * before while gravity and friction still settle the player. */
void Input_Clear(PlayerInput *in);

/* Drop the accumulated edges. Called by the tick loop once a tick has read
 * them, so a press fires on exactly one tick however many frames it spans. */
void Input_ClearEdges(PlayerInput *in);

/* One 20 Hz tick of input-owned state: ages the miss lockout. */
void Input_Tick(PlayerInput *in);

/* Minecraft.clickMouse's else branch: the click hit nothing, so lock attacks
 * out for INPUT_MISS_LOCKOUT ticks. */
void Input_Miss(PlayerInput *in);

#endif

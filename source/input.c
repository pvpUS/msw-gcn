#include <string.h>

#include "input.h"
#include "pad.h"
#include "settings.h"

/* ---- controller tuning (not gameplay physics) --------------------------- */
#define STICK_DEADZONE   12   /* C-stick look deadzone (raw units)          */
#define MOVE_DEADZONE    30   /* main-stick push past this = a full +/-1    */
#define LOOK_SPEED       2.2f /* degrees per frame at full C-stick tilt     */
/* Analog-trigger thresholds. Attached to the *trigger*, not to the action it
 * happens to be bound to: L is the easier pull because L is under the finger
 * that mines, and that is a property of the pad rather than of digging. */
#define TRIGGER_L_PULL   60
#define TRIGGER_R_PULL  100

static float deadzone(int raw) {
	if (raw >  STICK_DEADZONE) return (float)(raw - STICK_DEADZONE) / (127 - STICK_DEADZONE);
	if (raw < -STICK_DEADZONE) return (float)(raw + STICK_DEADZONE) / (127 - STICK_DEADZONE);
	return 0.0f;
}

void Input_Init(PlayerInput *in) {
	memset(in, 0, sizeof(*in));
}

void Input_Clear(PlayerInput *in) {
	/* Everything the player controls goes to rest; the internals (the edge
	 * history and the miss lockout) are state, not input, and survive. */
	u32 prevHeld = in->prevHeld;
	u8  attackRaw = in->attackRaw;
	int lock = in->leftClickCounter;
	memset(in, 0, sizeof(*in));
	in->prevHeld = prevHeld;
	in->attackRaw = attackRaw;
	in->leftClickCounter = lock;
}

void Input_ClearEdges(PlayerInput *in) {
	in->attackEdge = in->useEdge = 0;
	in->hotbarDelta = 0;
	in->navX = in->navY = 0;
	in->dropItem = in->menu = in->openInv = 0;
	in->confirm = in->cancel = in->pause = in->debug = 0;
}

void Input_Tick(PlayerInput *in) {
	if (in->leftClickCounter > 0) in->leftClickCounter--;
}

void Input_Miss(PlayerInput *in) {
	in->leftClickCounter = INPUT_MISS_LOCKOUT;
}

/* An action's bound bit against a held/down mask. Unbound reads as 0, which is
 * the whole of what "None" has to mean. */
static int bound(u32 mask, int act) {
	u32 bit = Settings_ButtonMask(g_settings.bind[act]);
	return bit && (mask & bit);
}

void Input_Sample(PlayerInput *in, int chan) {
	/* Fold the analog triggers into the button mask before anything reads it.
	 * Two things fall out: a bind test is a single bit compare whatever it is
	 * bound to, and a firm pull raises the same press *edge* the click at the
	 * bottom of the travel does -- which the attack path used to have to
	 * reconstruct by hand from its own `attackRaw` history. */
	u32 held = Pad_ButtonsHeld(chan);
	if (Pad_TriggerL(chan) > TRIGGER_L_PULL) held |= MSW_BTN_L;
	if (Pad_TriggerR(chan) > TRIGGER_R_PULL) held |= MSW_BTN_R;
	u32 down = held & ~in->prevHeld;
	in->prevHeld = held;

	/* ---- movement ---------------------------------------------------------
	 * MovementInputFromOptions maps WASD to exactly +/-1 because keyboard
	 * input is binary. A GameCube stick cannot reach magnitude 1.0 -- full
	 * deflection tops out near raw 100 of a nominal 127 -- so normalising it
	 * would walk permanently slower than the server's own prediction and never
	 * satisfy the sprint test. Digitise instead. */
	int rawX = Pad_StickX(chan), rawY = Pad_StickY(chan);
	in->moveForward = (rawY >  MOVE_DEADZONE) ?  1.0f
	                : (rawY < -MOVE_DEADZONE) ? -1.0f : 0.0f;
	in->moveStrafe  = (rawX >  MOVE_DEADZONE) ?  1.0f
	                : (rawX < -MOVE_DEADZONE) ? -1.0f : 0.0f;

	/* Look rate scales with the sensitivity setting, 0.5x to 8x. Still degrees
	 * per *rendered frame* rather than per second (see input.h) -- the setting
	 * multiplies the rate, it does not change what the rate is measured
	 * against. */
	float look = LOOK_SPEED * Settings_LookScale();
	in->dYaw   = -deadzone(Pad_SubStickX(chan)) * look;
	in->dPitch =  deadzone(Pad_SubStickY(chan)) * look;

	in->jump       = (u8)(bound(held, SET_ACT_JUMP)   != 0);
	in->sneak      = (u8)(bound(held, SET_ACT_SNEAK)  != 0);
	in->sprintHeld = (u8)(bound(held, SET_ACT_SPRINT) != 0);

	/* Auto-sprint, synthesised as a held sprint button rather than as a flag
	 * player.c has to learn about. Everything that already qualifies a real
	 * hold still applies -- forward > 0, not sneaking, not using an item, and
	 * dropped on a horizontal collision -- so this is exactly "hold R for me",
	 * including the re-latch after walking into a wall that a keyboard player
	 * would have to double-tap for. */
	if (g_settings.autoSprint && in->moveForward > 0.0f) in->sprintHeld = 1;

	/* ---- the two mouse buttons -------------------------------------------
	 * The miss lockout is applied here so that neither interact.c nor combat.c
	 * has to know it exists -- but it is applied to the *view*, not to the raw
	 * state: sendClickBlockToController clears the counter the moment the button
	 * comes up, and reading the gated value for that test would clear it a tick
	 * after it was set. */
	int attackRaw  = bound(held, SET_ACT_ATTACK) != 0;
	int attackDown = bound(down, SET_ACT_ATTACK) != 0;
	in->attackRaw = (u8)attackRaw;
	if (!attackRaw) in->leftClickCounter = 0;

	int locked = in->leftClickCounter > 0;
	in->attackHeld = (u8)(locked ? 0 : attackRaw);
	if (attackDown && !locked) in->attackEdge = 1;

	in->useHeld = (u8)(bound(held, SET_ACT_USE) != 0);
	if (bound(down, SET_ACT_USE)) in->useEdge = 1;

	/* ---- edges ------------------------------------------------------------
	 * OR'd rather than assigned: a frame that runs no tick must not swallow the
	 * press, and Input_ClearEdges is what ends it.
	 *
	 * Menu navigation is read off the physical pad and is *not* rebindable. It
	 * is the same set of presses the bound actions come from -- out in the world
	 * D-pad Left both scrolls the hotbar and steps a cursor left, and nothing
	 * can see both at once -- but a remapping screen you can rebind yourself out
	 * of would be able to leave the pad in a state with no way back to it. So
	 * the D-pad steps a cursor, A confirms, B cancels and Start opens or closes
	 * the palette, always. */
	if (down & MSW_BTN_LEFT)  in->navX--;
	if (down & MSW_BTN_RIGHT) in->navX++;
	if (down & MSW_BTN_UP)    in->navY--;
	if (down & MSW_BTN_DOWN)  in->navY++;
	if (down & MSW_BTN_A)     in->confirm = 1;
	if (down & MSW_BTN_B)     in->cancel = 1;
	if (down & MSW_BTN_START) in->pause = 1;

	if (bound(down, SET_ACT_HOTBAR_PREV)) in->hotbarDelta++;
	if (bound(down, SET_ACT_HOTBAR_NEXT)) in->hotbarDelta--;
	if (bound(down, SET_ACT_MENU))        in->menu = 1;
	if (bound(down, SET_ACT_DROP))        in->dropItem = 1;
	if (bound(down, SET_ACT_INVENTORY))   in->openInv = 1;

	/* Chat is held; the offline free-fly camera hangs off the same button's
	 * press edge, so it follows the binding rather than staying on Z. */
	in->showChat = (u8)(bound(held, SET_ACT_CHAT) != 0);
	if (bound(down, SET_ACT_CHAT)) in->debug = 1;
}

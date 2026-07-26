#include <string.h>

#include "input.h"

/* ---- controller tuning (not gameplay physics) --------------------------- */
#define STICK_DEADZONE   12   /* C-stick look deadzone (raw units)          */
#define MOVE_DEADZONE    30   /* main-stick push past this = a full +/-1    */
#define LOOK_SPEED       2.2f /* degrees per frame at full C-stick tilt     */
#define TRIGGER_ATTACK   60   /* analog L past this counts as a click       */
#define TRIGGER_SPRINT  100   /* analog R past this counts as sprint        */

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

void Input_Sample(PlayerInput *in, int chan) {
	u32 held = PAD_ButtonsHeld(chan);
	u32 down = held & ~in->prevHeld;
	in->prevHeld = held;

	/* ---- movement ---------------------------------------------------------
	 * MovementInputFromOptions maps WASD to exactly +/-1 because keyboard
	 * input is binary. A GameCube stick cannot reach magnitude 1.0 -- full
	 * deflection tops out near raw 100 of a nominal 127 -- so normalising it
	 * would walk permanently slower than the server's own prediction and never
	 * satisfy the sprint test. Digitise instead. */
	int rawX = PAD_StickX(chan), rawY = PAD_StickY(chan);
	in->moveForward = (rawY >  MOVE_DEADZONE) ?  1.0f
	                : (rawY < -MOVE_DEADZONE) ? -1.0f : 0.0f;
	in->moveStrafe  = (rawX >  MOVE_DEADZONE) ?  1.0f
	                : (rawX < -MOVE_DEADZONE) ? -1.0f : 0.0f;

	in->dYaw   = -deadzone(PAD_SubStickX(chan)) * LOOK_SPEED;
	in->dPitch =  deadzone(PAD_SubStickY(chan)) * LOOK_SPEED;

	in->jump       = (held & PAD_BUTTON_A) != 0;
	in->sneak      = (held & PAD_BUTTON_B) != 0;
	in->sprintHeld = (held & PAD_TRIGGER_R) != 0 || PAD_TriggerR(chan) > TRIGGER_SPRINT;

	/* ---- the two mouse buttons -------------------------------------------
	 * L is analog, so either a firm pull or the digital click at the bottom of
	 * the travel counts. The miss lockout is applied here so that neither
	 * interact.c nor combat.c has to know it exists -- but it is applied to the
	 * *view*, not to the raw state: sendClickBlockToController clears the
	 * counter the moment the button comes up, and reading the gated value for
	 * that test would clear it a tick after it was set. */
	int attackRaw = (held & PAD_TRIGGER_L) != 0 || PAD_TriggerL(chan) > TRIGGER_ATTACK;
	int attackDown = attackRaw && !in->attackRaw;
	in->attackRaw = (u8)attackRaw;
	if (!attackRaw) in->leftClickCounter = 0;

	int locked = in->leftClickCounter > 0;
	in->attackHeld = (u8)(locked ? 0 : attackRaw);
	if (attackDown && !locked) in->attackEdge = 1;

	in->useHeld = (held & PAD_BUTTON_Y) != 0;
	if (down & PAD_BUTTON_Y) in->useEdge = 1;

	/* ---- edges ------------------------------------------------------------
	 * OR'd rather than assigned: a frame that runs no tick must not swallow the
	 * press, and Input_ClearEdges is what ends it. */
	if (down & PAD_BUTTON_LEFT)  { in->navX--; in->hotbarDelta++; }
	if (down & PAD_BUTTON_RIGHT) { in->navX++; in->hotbarDelta--; }
	if (down & PAD_BUTTON_UP)    { in->navY--; in->menu = 1; }
	if (down & PAD_BUTTON_DOWN)  { in->navY++; in->dropItem = 1; }
	if (down & PAD_BUTTON_X)      in->openInv = 1;
	if (down & PAD_BUTTON_A)      in->confirm = 1;
	if (down & PAD_BUTTON_B)      in->cancel = 1;
	if (down & PAD_BUTTON_START)  in->pause = 1;
	if (down & PAD_TRIGGER_Z)     in->debug = 1;

	in->showChat = (held & PAD_TRIGGER_Z) != 0;
}

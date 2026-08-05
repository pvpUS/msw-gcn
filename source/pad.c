#include <string.h>

#include "pad.h"
#include "gcadapter.h"

#ifdef HW_RVL
#include <wiiuse/wpad.h>
#endif

/* One frame's worth of controller state, in GameCube units whatever produced
 * it. Every backend fills one of these and the readers below just serve it. */
typedef struct {
	u32 held, down, prev;
	int stickX, stickY, subX, subY;
	int trigL, trigR;
	const char *source;
} PadState;

static PadState g_pad[4];

#ifdef HW_RVL

static u32 g_nativeMask;   /* PAD_ScanPads: which real ports answered */

/* ---- Classic Controller / Wii U Pro -------------------------------------
 * The last resort, and the only backend that is a genuine remap rather than a
 * GameCube pad wearing a different shell. The overlay is the obvious one --
 * the face buttons and D-pad keep their names, the analog shoulders keep
 * theirs, and the two sticks land on main and C.
 *
 * GameCube Z is the odd one out: the Classic has two of that button and the
 * GameCube has one, so ZL and ZR both produce Z rather than leaving a finger
 * with nothing to do. Minus has no GameCube counterpart and stays unbound;
 * Plus is Start, which is what opens the command palette. */
static u32 classic_to_gc(u32 w)
{
	u32 m = 0;
	if (w & WPAD_CLASSIC_BUTTON_A)      m |= PAD_BUTTON_A;
	if (w & WPAD_CLASSIC_BUTTON_B)      m |= PAD_BUTTON_B;
	if (w & WPAD_CLASSIC_BUTTON_X)      m |= PAD_BUTTON_X;
	if (w & WPAD_CLASSIC_BUTTON_Y)      m |= PAD_BUTTON_Y;
	if (w & WPAD_CLASSIC_BUTTON_FULL_L) m |= PAD_TRIGGER_L;
	if (w & WPAD_CLASSIC_BUTTON_FULL_R) m |= PAD_TRIGGER_R;
	if (w & WPAD_CLASSIC_BUTTON_ZL)     m |= PAD_TRIGGER_Z;
	if (w & WPAD_CLASSIC_BUTTON_ZR)     m |= PAD_TRIGGER_Z;
	if (w & WPAD_CLASSIC_BUTTON_UP)     m |= PAD_BUTTON_UP;
	if (w & WPAD_CLASSIC_BUTTON_DOWN)   m |= PAD_BUTTON_DOWN;
	if (w & WPAD_CLASSIC_BUTTON_LEFT)   m |= PAD_BUTTON_LEFT;
	if (w & WPAD_CLASSIC_BUTTON_RIGHT)  m |= PAD_BUTTON_RIGHT;
	if (w & WPAD_CLASSIC_BUTTON_PLUS)   m |= PAD_BUTTON_START;
	return m;
}

/* Scale a Classic stick to the GameCube's -127..127 using the controller's own
 * calibration, which is what makes one function work for both sticks: the left
 * one reports 6 bits and the right one 5, and neither is centred at its
 * midpoint. */
static int classic_axis(u8 pos, u8 center, u8 min, u8 max)
{
	int v = (int)pos - (int)center;
	int range = (v >= 0) ? (int)max - (int)center : (int)center - (int)min;

	if (range <= 0) return 0;
	v = v * 127 / range;
	if (v >  127) v =  127;
	if (v < -127) v = -127;
	return v;
}

static int read_classic(int chan, PadState *s)
{
	expansion_t exp;
	u32 type;

	if (WPAD_Probe(chan, &type) < 0) return 0;
	if (type != WPAD_EXP_CLASSIC) return 0;

	WPAD_Expansion(chan, &exp);
	if (exp.type != WPAD_EXP_CLASSIC) return 0;

	s->held = classic_to_gc(WPAD_ButtonsHeld(chan));
	s->stickX = classic_axis(exp.classic.ljs.pos.x, exp.classic.ljs.center.x,
	                         exp.classic.ljs.min.x, exp.classic.ljs.max.x);
	s->stickY = classic_axis(exp.classic.ljs.pos.y, exp.classic.ljs.center.y,
	                         exp.classic.ljs.min.y, exp.classic.ljs.max.y);
	s->subX   = classic_axis(exp.classic.rjs.pos.x, exp.classic.rjs.center.x,
	                         exp.classic.rjs.min.x, exp.classic.rjs.max.x);
	s->subY   = classic_axis(exp.classic.rjs.pos.y, exp.classic.rjs.center.y,
	                         exp.classic.rjs.min.y, exp.classic.rjs.max.y);
	s->trigL  = (int)(exp.classic.l_shoulder * 255.0f);
	s->trigR  = (int)(exp.classic.r_shoulder * 255.0f);
	s->source = "Classic Controller";
	return 1;
}

static int read_adapter(int chan, PadState *s)
{
	const GCAdapterPort *p = GCAdapter_Port(chan);

	if (!p) return 0;
	s->held   = p->buttons;
	s->stickX = p->stickX;
	s->stickY = p->stickY;
	s->subX   = p->substickX;
	s->subY   = p->substickY;
	s->trigL  = p->triggerL;
	s->trigR  = p->triggerR;
	s->source = "USB GameCube adapter";
	return 1;
}

#endif /* HW_RVL */

static int read_native(int chan, PadState *s)
{
#ifdef HW_RVL
	/* RVL-001 has the four ports; every later model does not, and there the
	 * scan simply never reports a controller. */
	if (!(g_nativeMask & (1 << chan))) return 0;
#endif
	s->held   = PAD_ButtonsHeld(chan);
	s->stickX = PAD_StickX(chan);
	s->stickY = PAD_StickY(chan);
	s->subX   = PAD_SubStickX(chan);
	s->subY   = PAD_SubStickY(chan);
	s->trigL  = PAD_TriggerL(chan);
	s->trigR  = PAD_TriggerR(chan);
	s->source = "GameCube port";
	return 1;
}

void Pad_Init(void)
{
	memset(g_pad, 0, sizeof(g_pad));
	PAD_Init();
#ifdef HW_RVL
	WPAD_Init();
	/* The fullest report mode, because it is the one that carries the six
	 * extension bytes a Classic Controller's sticks and shoulders live in. */
	WPAD_SetDataFormat(WPAD_CHAN_ALL, WPAD_FMT_BTNS_ACC_IR);
	GCAdapter_Init();
#endif
}

void Pad_Scan(void)
{
	int chan;

#ifdef HW_RVL
	g_nativeMask = PAD_ScanPads();
	WPAD_ScanPads();
	GCAdapter_Poll();
#else
	PAD_ScanPads();
#endif

	for (chan = 0; chan < 4; chan++) {
		PadState *s = &g_pad[chan];
		u32 prev = s->prev;
		int got;

		memset(s, 0, sizeof(*s));
		s->prev = prev;
		s->source = "none";

		/* First backend that has something connected wins the whole channel,
		 * rather than merging them: a stick has to come from one place, and a
		 * player who has both an adapter and a pad in a port should not have
		 * the two fight over the camera. The adapter goes first because it is
		 * the one an RVL-101 can actually use. */
#ifdef HW_RVL
		got = read_adapter(chan, s) || read_native(chan, s) || read_classic(chan, s);
#else
		got = read_native(chan, s);
#endif
		(void)got;

		s->down = s->held & ~prev;
		s->prev = s->held;
	}
}

u32 Pad_ButtonsHeld(int chan) { return g_pad[chan & 3].held; }
u32 Pad_ButtonsDown(int chan) { return g_pad[chan & 3].down; }
int Pad_StickX(int chan)      { return g_pad[chan & 3].stickX; }
int Pad_StickY(int chan)      { return g_pad[chan & 3].stickY; }
int Pad_SubStickX(int chan)   { return g_pad[chan & 3].subX; }
int Pad_SubStickY(int chan)   { return g_pad[chan & 3].subY; }
int Pad_TriggerL(int chan)    { return g_pad[chan & 3].trigL; }
int Pad_TriggerR(int chan)    { return g_pad[chan & 3].trigR; }

const char *Pad_SourceName(int chan)
{
	const char *s = g_pad[chan & 3].source;
	return s ? s : "none";
}

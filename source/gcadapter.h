#ifndef MSW_GCADAPTER_H
#define MSW_GCADAPTER_H

#include <gccore.h>

#ifdef HW_RVL

/* Nintendo's Wii U GameCube controller adapter, and the Mayflash clones that
 * speak the same protocol with their switch in "Wii U" position. Wii-only:
 * libogc's USB layer does not exist on the GameCube, and would have nothing to
 * talk to if it did.
 *
 * Reports four ports of genuine GameCube pad state, so pad.c can hand the game
 * exactly what a console would. See gcadapter.c for the wire format.
 */

typedef struct {
	u8 connected;
	u32 buttons;              /* already translated to PAD_* bits    */
	s8 stickX, stickY;        /* -127..127 about the captured origin */
	s8 substickX, substickY;
	u8 triggerL, triggerR;    /* 0..255                              */
} GCAdapterPort;

/* Start looking for an adapter. Never blocks: the search and every read after
 * it are asynchronous, so an adapter plugged in mid-game still works. */
void GCAdapter_Init(void);

/* Once per frame. Retries the open on a cadence while no adapter is present,
 * and publishes the latest packet into the port snapshots. */
void GCAdapter_Poll(void);

/* NULL when no adapter is attached or `chan` is out of range. */
const GCAdapterPort *GCAdapter_Port(int chan);

int GCAdapter_Present(void);

#endif /* HW_RVL */

#endif

#ifndef MSW_MENU_H
#define MSW_MENU_H

#include <gccore.h>
#include "maps.h"

/* Menu_Run's out-of-band results. Anything >= 0 is a map index. */
#define MENU_QUIT    (-1)
#define MENU_NETWORK (-2)   /* connect to the proxy and spectate (T11) */

/* Runs the map-selection menu using the libogc console on `xfb`.
 * `netTarget` is the proxy address the network entry will dial, shown so the
 * one machine-specific thing about multiplayer is visible before committing to
 * it; pass NULL to hide the entry entirely.
 *
 * Returns the chosen map index, MENU_NETWORK, or MENU_QUIT. */
int Menu_Run(const MapEntry *maps, int count, void *xfb, GXRModeObj *rmode,
             const char *netTarget);

#endif

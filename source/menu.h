#ifndef MSW_MENU_H
#define MSW_MENU_H

#include <gccore.h>
#include "maps.h"

/* Runs the map-selection menu using the libogc console on `xfb`.
 * Returns the chosen map index, or -1 if the user quit (Start). */
int Menu_Run(const MapEntry *maps, int count, void *xfb, GXRModeObj *rmode);

#endif

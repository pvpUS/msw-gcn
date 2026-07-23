#ifndef MSW_MAPS_H
#define MSW_MAPS_H

#include <stdint.h>

/* One embedded, compressed world. `data`..`end` is the raw .mworld blob
 * (bin2s emits both as address constants, so they are valid in the static
 * table initializer in maps_gen.h; the size is derived at runtime). */
typedef struct {
	const char    *name;
	const uint8_t *data;
	const uint8_t *end;
	uint32_t       blocks;
} MapEntry;

#endif

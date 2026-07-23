#include "lz.h"

#define MIN_MATCH 3

void LZ_Decompress(const u8 *src, u8 *dst, u32 dstlen) {
	u32 out = 0;
	u32 sp = 0;

	while (out < dstlen) {
		u8 flags = src[sp++];
		int bit;
		for (bit = 0; bit < 8 && out < dstlen; bit++) {
			if (flags & (1 << bit)) {
				u32 dist = ((u32)src[sp] << 8) | src[sp + 1];
				u32 len  = (u32)src[sp + 2] + MIN_MATCH;
				sp += 3;
				dist += 1;
				u32 from = out - dist;
				u32 k;
				for (k = 0; k < len; k++)
					dst[out + k] = dst[from + k];
				out += len;
			} else {
				dst[out++] = src[sp++];
			}
		}
	}
}

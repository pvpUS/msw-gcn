#ifndef MSW_LZ_H
#define MSW_LZ_H

#include <gctypes.h>

/*
 * LZSS decoder matching tools/compress_worlds.py.
 *
 * Token stream: repeating groups of one flag byte followed by up to 8 tokens.
 * Flag bit i (LSB first) selects token i: 0 = literal (1 byte, copied out),
 * 1 = match (u16 big-endian distance-1, then u8 length; copy length+3 bytes
 * from dstlen-distance, allowing overlap). Decoding stops once `dstlen` bytes
 * have been produced.
 */
void LZ_Decompress(const u8 *src, u8 *dst, u32 dstlen);

#endif

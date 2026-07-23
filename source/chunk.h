#ifndef MSW_CHUNK_H
#define MSW_CHUNK_H

#include <gccore.h>

#define CHUNK_X 16
#define CHUNK_Y 12
#define CHUNK_Z 16

typedef enum {
	BLOCK_AIR = 0,
	BLOCK_GRASS,
	BLOCK_DIRT,
	BLOCK_STONE,
} BlockType;

void Chunk_GenerateIsland(void);
void Chunk_InitGX(void);
void Chunk_Draw(void);

#endif

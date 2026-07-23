#include <math.h>
#include <stddef.h>
#include "chunk.h"

#define BLOCK_SIZE 8.0f

static u8 blocks[CHUNK_X][CHUNK_Y][CHUNK_Z];

// 6 faces, 4 verts each, as unit-cube corner offsets (0/1). Winding doesn't
// matter since we render with GX_CULL_NONE.
static const float faceVerts[6][4][3] = {
	{ {0,0,0}, {0,0,1}, {0,1,1}, {0,1,0} }, // -X
	{ {1,0,0}, {1,1,0}, {1,1,1}, {1,0,1} }, // +X
	{ {0,0,0}, {1,0,0}, {1,0,1}, {0,0,1} }, // -Y (bottom)
	{ {0,1,0}, {0,1,1}, {1,1,1}, {1,1,0} }, // +Y (top)
	{ {0,0,0}, {0,1,0}, {1,1,0}, {1,0,0} }, // -Z
	{ {0,0,1}, {1,0,1}, {1,1,1}, {0,1,1} }, // +Z
};

static const int faceNormal[6][3] = {
	{-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1},
};

typedef struct { u8 r,g,b,a; } RGBA;

static RGBA FaceColor(BlockType type, int face) {
	RGBA grassTop = {60, 170, 60, 255};
	RGBA dirt     = {121, 85, 58, 255};
	RGBA stone    = {128, 128, 128, 255};

	if (type == BLOCK_GRASS) return (face == 3) ? grassTop : dirt;
	if (type == BLOCK_DIRT)  return dirt;
	return stone;
}

static int IsSolid(int x, int y, int z) {
	if (x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z)
		return 0;
	return blocks[x][y][z] != BLOCK_AIR;
}

void Chunk_GenerateIsland(void) {
	float cx = (CHUNK_X - 1) / 2.0f;
	float cz = (CHUNK_Z - 1) / 2.0f;
	float radius = 6.5f;
	int baseY = 4;

	for (int x = 0; x < CHUNK_X; x++) {
		for (int z = 0; z < CHUNK_Z; z++) {
			float dx = x - cx;
			float dz = z - cz;
			float dist2 = dx * dx + dz * dz;
			float remain = radius * radius - dist2;

			if (remain < 0.0f) continue;

			int topOffset = (int)(sqrtf(remain) * 0.9f);
			int botOffset = (int)(sqrtf(remain) * 0.5f);
			int topY = baseY + topOffset;
			int botY = baseY - botOffset;

			if (topY >= CHUNK_Y) topY = CHUNK_Y - 1;
			if (botY < 0) botY = 0;

			for (int y = botY; y <= topY; y++) {
				if (y == topY)
					blocks[x][y][z] = BLOCK_GRASS;
				else if (topY - y <= 2)
					blocks[x][y][z] = BLOCK_DIRT;
				else
					blocks[x][y][z] = BLOCK_STONE;
			}
		}
	}
}

void Chunk_InitGX(void) {
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);

	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

	GX_SetNumChans(1);
	GX_SetNumTexGens(0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
}

void Chunk_Draw(void) {
	u16 faceCount = 0;

	for (int x = 0; x < CHUNK_X; x++)
		for (int y = 0; y < CHUNK_Y; y++)
			for (int z = 0; z < CHUNK_Z; z++) {
				if (blocks[x][y][z] == BLOCK_AIR) continue;
				for (int f = 0; f < 6; f++)
					if (!IsSolid(x + faceNormal[f][0], y + faceNormal[f][1], z + faceNormal[f][2]))
						faceCount++;
			}

	if (faceCount == 0) return;

	GX_Begin(GX_QUADS, GX_VTXFMT0, faceCount * 4);

	for (int x = 0; x < CHUNK_X; x++)
		for (int y = 0; y < CHUNK_Y; y++)
			for (int z = 0; z < CHUNK_Z; z++) {
				BlockType type = blocks[x][y][z];
				if (type == BLOCK_AIR) continue;

				// centre the chunk on the origin
				float ox = (x - CHUNK_X / 2.0f) * BLOCK_SIZE;
				float oy = (y - CHUNK_Y / 2.0f) * BLOCK_SIZE;
				float oz = (z - CHUNK_Z / 2.0f) * BLOCK_SIZE;

				for (int f = 0; f < 6; f++) {
					if (IsSolid(x + faceNormal[f][0], y + faceNormal[f][1], z + faceNormal[f][2]))
						continue;

					RGBA c = FaceColor(type, f);
					for (int v = 0; v < 4; v++) {
						GX_Position3f32(
							ox + faceVerts[f][v][0] * BLOCK_SIZE,
							oy + faceVerts[f][v][1] * BLOCK_SIZE,
							oz + faceVerts[f][v][2] * BLOCK_SIZE);
						GX_Color4u8(c.r, c.g, c.b, c.a);
					}
				}
			}

	GX_End();
}

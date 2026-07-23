#ifndef MSW_CAMERA_H
#define MSW_CAMERA_H

#include <gccore.h>

typedef struct {
	guVector pos;
	float yaw;   // degrees, rotation around world Y
	float pitch; // degrees, look up/down, clamped to [-89, 89]
} Camera;

void Camera_Init(Camera *cam, float x, float y, float z, float yaw, float pitch);
void Camera_Update(Camera *cam, int chan);
void Camera_GetViewMatrix(Camera *cam, Mtx v);

/* A single scripted vantage point for MODEL_TEST_MODE (main.c): x/y/z are in
 * Minecraft block units, spawn-relative, same convention as Player/World --
 * scale by WORLD_BLOCK_SIZE before handing to Camera_Init. See
 * tools/gen_model_gallery.py, which generates source/gallery_tour_gen.h. */
typedef struct {
	float x, y, z;
	float yaw, pitch;
	const char *label;
} CamKeyframe;

#endif

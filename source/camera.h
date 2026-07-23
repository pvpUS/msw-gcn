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

#endif

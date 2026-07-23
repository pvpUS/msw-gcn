#include <math.h>
#include "camera.h"

#define DEG2RAD 0.017453292519943295f
#define STICK_DEADZONE 12
#define MOVE_SPEED 3.0f
#define FLY_SPEED 3.0f
#define LOOK_SPEED 2.2f
#define PITCH_LIMIT 89.0f

static float ApplyDeadzone(int raw) {
	if (raw > STICK_DEADZONE) return (float)(raw - STICK_DEADZONE) / (127 - STICK_DEADZONE);
	if (raw < -STICK_DEADZONE) return (float)(raw + STICK_DEADZONE) / (127 - STICK_DEADZONE);
	return 0.0f;
}

void Camera_Init(Camera *cam, float x, float y, float z, float yaw, float pitch) {
	cam->pos.x = x;
	cam->pos.y = y;
	cam->pos.z = z;
	cam->yaw = yaw;
	cam->pitch = pitch;
}

void Camera_Update(Camera *cam, int chan) {
	float moveX = ApplyDeadzone(PAD_StickX(chan));
	float moveY = ApplyDeadzone(PAD_StickY(chan));
	float lookX = ApplyDeadzone(PAD_SubStickX(chan));
	float lookY = ApplyDeadzone(PAD_SubStickY(chan));

	cam->yaw   -= lookX * LOOK_SPEED;
	cam->pitch += lookY * LOOK_SPEED;

	if (cam->pitch > PITCH_LIMIT) cam->pitch = PITCH_LIMIT;
	if (cam->pitch < -PITCH_LIMIT) cam->pitch = -PITCH_LIMIT;

	float yawRad = cam->yaw * DEG2RAD;

	// flat forward/right vectors (pitch does not affect horizontal movement,
	// matching a Minecraft-style creative-flight feel)
	float forwardX = -sinf(yawRad);
	float forwardZ = -cosf(yawRad);
	float rightX = cosf(yawRad);
	float rightZ = -sinf(yawRad);

	cam->pos.x += (forwardX * moveY + rightX * moveX) * MOVE_SPEED;
	cam->pos.z += (forwardZ * moveY + rightZ * moveX) * MOVE_SPEED;

	u32 held = PAD_ButtonsHeld(chan);
	if (held & PAD_BUTTON_A) cam->pos.y += FLY_SPEED;
	if (held & PAD_BUTTON_B) cam->pos.y -= FLY_SPEED;
}

void Camera_GetViewMatrix(Camera *cam, Mtx v) {
	float yawRad = cam->yaw * DEG2RAD;
	float pitchRad = cam->pitch * DEG2RAD;

	guVector look;
	look.x = cam->pos.x - sinf(yawRad) * cosf(pitchRad);
	look.y = cam->pos.y + sinf(pitchRad);
	look.z = cam->pos.z - cosf(yawRad) * cosf(pitchRad);

	guVector up = {0.0f, 1.0f, 0.0f};

	guLookAt(v, &cam->pos, &up, &look);
}

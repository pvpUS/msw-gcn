#include "gcadapter.h"

#ifdef HW_RVL

#include <string.h>
#include <malloc.h>
#include <ogc/usb.h>

/* ---- the wire format ------------------------------------------------------
 * Nintendo's adapter (and any Mayflash switched to "Wii U" mode) is a plain
 * vendor-class USB device, not HID. Interface 0 carries two interrupt
 * endpoints. Writing a single 0x13 byte to the OUT endpoint starts it
 * streaming; from then on the IN endpoint produces a 37-byte packet at about
 * 1 kHz whether anything changed or not:
 *
 *   [0]        0x21, the report id
 *   [1 + 9*p]  port p status: the high nibble is 0 for an empty port, 1 for a
 *              wired pad and 2 for a Wavebird
 *   [2 + 9*p]  buttons, low
 *   [3 + 9*p]  buttons, high
 *   [4 + 9*p]  main stick X, Y                 (0..255)
 *   [6 + 9*p]  C-stick X, Y                    (0..255)
 *   [8 + 9*p]  analog L, R                     (0..255)
 *
 * Reads are asynchronous. A blocking USB_ReadIntrMsg in the render loop would
 * be fine while packets keep arriving and would wedge the whole game the moment
 * someone pulled the cable, which is not a trade worth making for a controller.
 */

#define GCA_VID         0x057E
#define GCA_PID         0x0337
#define GCA_EP_IN       0x81
#define GCA_EP_OUT      0x02
#define GCA_PAYLOAD     37
#define GCA_CMD_START   0x13

/* How often to go looking again while nothing is attached. At 60 Hz this is
 * about twice a second -- often enough that plugging in feels immediate, rare
 * enough that the USB enumeration is not run every single frame. */
#define GCA_RETRY_FRAMES 30

static s32 g_fd = -1;
static int g_retry;
static int g_present;

/* IOS DMAs into these, so they are 32-byte aligned and allocated once. */
static u8 *g_payload;
static u8 *g_cmd;

/* Written by the USB callback (an IPC thread), read by the game thread. The
 * race is deliberate and benign: the worst case is one frame reading half of
 * one packet and half of the next, which is a controller sample landing a
 * millisecond early. Locking a 37-byte copy against the render loop would cost
 * more than it protects. */
static volatile u8 g_snapshot[GCA_PAYLOAD];
static volatile int g_have;

static GCAdapterPort g_port[4];
static u8 g_originValid[4];
static u8 g_originSX[4], g_originSY[4], g_originCX[4], g_originCY[4];

static s32 gca_read_cb(s32 result, void *usr);

static void gca_close(void)
{
	if (g_fd >= 0) USB_CloseDevice(&g_fd);
	g_fd = -1;
	g_present = 0;
	g_have = 0;
	memset(g_port, 0, sizeof(g_port));
	memset(g_originValid, 0, sizeof(g_originValid));
}

/* Arm the next read. The callback re-arms, so this only happens once per open. */
static void gca_arm(void)
{
	if (g_fd < 0) return;
	if (USB_ReadIntrMsgAsync(g_fd, GCA_EP_IN, GCA_PAYLOAD, g_payload,
	                         gca_read_cb, NULL) < 0)
		gca_close();
}

static s32 gca_read_cb(s32 result, void *usr)
{
	(void)usr;
	if (result < 0) {
		/* Almost always the adapter being unplugged. Drop everything and let
		 * GCAdapter_Poll find it again if it comes back. */
		gca_close();
		return 0;
	}
	if (result >= GCA_PAYLOAD && g_payload[0] == 0x21) {
		memcpy((void *)g_snapshot, g_payload, GCA_PAYLOAD);
		g_have = 1;
	}
	gca_arm();
	return 0;
}

static int gca_open(void)
{
	usb_device_entry list[8];
	u8 count = 0;
	int i;

	/* Interface class 0 rather than USB_CLASS_HID: this device is vendor
	 * class, so asking for HID would never list it. */
	if (USB_GetDeviceList(list, 8, 0, &count) < 0) return 0;

	for (i = 0; i < count; i++) {
		if (list[i].vid != GCA_VID || list[i].pid != GCA_PID) continue;
		if (USB_OpenDevice(list[i].device_id, GCA_VID, GCA_PID, &g_fd) < 0) {
			g_fd = -1;
			continue;
		}

		/* IOS leaves the device unconfigured. A failure here is not fatal on
		 * every IOS revision, so try and carry on regardless. */
		USB_SetConfiguration(g_fd, 1);

		g_cmd[0] = GCA_CMD_START;
		if (USB_WriteIntrMsg(g_fd, GCA_EP_OUT, 1, g_cmd) < 0) {
			gca_close();
			continue;
		}

		g_present = 1;
		gca_arm();
		return 1;
	}
	return 0;
}

void GCAdapter_Init(void)
{
	if (!g_payload) g_payload = (u8 *)memalign(32, GCA_PAYLOAD + 32);
	if (!g_cmd)     g_cmd     = (u8 *)memalign(32, 32);
	if (!g_payload || !g_cmd) return;

	USB_Initialize();
	g_retry = 0;
	gca_open();
}

/* Translate one port's two button bytes into GameCube bits. Using the PAD_*
 * symbols rather than their values is what keeps this honest -- the game never
 * learns that these came off a USB packet. */
static u32 gca_buttons(u8 lo, u8 hi)
{
	u32 m = 0;
	if (lo & 0x01) m |= PAD_BUTTON_A;
	if (lo & 0x02) m |= PAD_BUTTON_B;
	if (lo & 0x04) m |= PAD_BUTTON_X;
	if (lo & 0x08) m |= PAD_BUTTON_Y;
	if (lo & 0x10) m |= PAD_BUTTON_LEFT;
	if (lo & 0x20) m |= PAD_BUTTON_RIGHT;
	if (lo & 0x40) m |= PAD_BUTTON_DOWN;
	if (lo & 0x80) m |= PAD_BUTTON_UP;
	if (hi & 0x01) m |= PAD_BUTTON_START;
	if (hi & 0x02) m |= PAD_TRIGGER_Z;
	if (hi & 0x04) m |= PAD_TRIGGER_R;
	if (hi & 0x08) m |= PAD_TRIGGER_L;
	return m;
}

static s8 gca_axis(u8 raw, u8 origin)
{
	int v = (int)raw - (int)origin;
	if (v >  127) v =  127;
	if (v < -127) v = -127;
	return (s8)v;
}

void GCAdapter_Poll(void)
{
	const u8 *p;
	int i;

	if (g_fd < 0) {
		if (++g_retry >= GCA_RETRY_FRAMES) {
			g_retry = 0;
			gca_open();
		}
		return;
	}
	if (!g_have) return;

	p = (const u8 *)g_snapshot;
	for (i = 0; i < 4; i++) {
		const u8 *s = p + 1 + 9 * i;
		int connected = (s[0] >> 4) != 0;

		if (!connected) {
			memset(&g_port[i], 0, sizeof(g_port[i]));
			g_originValid[i] = 0;
			continue;
		}

		/* A pad's neutral is not exactly 128, so the first packet after it
		 * appears becomes its centre -- the same thing PAD_Init does for the
		 * console's own ports, and without it every stick has a small
		 * permanent drift. */
		if (!g_originValid[i]) {
			g_originSX[i] = s[3]; g_originSY[i] = s[4];
			g_originCX[i] = s[5]; g_originCY[i] = s[6];
			g_originValid[i] = 1;
		}

		g_port[i].connected = 1;
		g_port[i].buttons   = gca_buttons(s[1], s[2]);
		g_port[i].stickX    = gca_axis(s[3], g_originSX[i]);
		g_port[i].stickY    = gca_axis(s[4], g_originSY[i]);
		g_port[i].substickX = gca_axis(s[5], g_originCX[i]);
		g_port[i].substickY = gca_axis(s[6], g_originCY[i]);
		g_port[i].triggerL  = s[7];
		g_port[i].triggerR  = s[8];
	}
}

const GCAdapterPort *GCAdapter_Port(int chan)
{
	if (chan < 0 || chan > 3 || g_fd < 0) return NULL;
	if (!g_port[chan].connected) return NULL;
	return &g_port[chan];
}

int GCAdapter_Present(void) { return g_present && g_fd >= 0; }

#endif /* HW_RVL */

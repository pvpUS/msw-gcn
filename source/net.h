#ifndef MSW_NET_H
#define MSW_NET_H

#include <gccore.h>
#include "gclink.h"

/* The console's end of the GCLink socket (see gclink.h for the wire format).
 *
 * **Single-threaded and non-blocking, deliberately.** There is no LWP thread
 * here and there should not be one: GX state is global and nothing in this
 * codebase takes a lock around it, so a second thread touching the network
 * while the main loop is mid-display-list is a class of bug that is very hard
 * to see and impossible to reproduce. Draining a socket costs microseconds
 * against a 16 ms frame; it belongs in the frame.
 *
 * Net_Poll drives everything -- the connect handshake, the receive
 * reassembly, the send queue and the keepalive timeout -- so calling it every
 * frame is not optional, it *is* the state machine.
 */

typedef enum {
	NET_DOWN = 0,    /* no interface: if_config failed or was never run   */
	NET_IDLE,        /* interface up, socket closed                       */
	NET_CONNECTING,  /* TCP handshake in flight                           */
	NET_HANDSHAKE,   /* connected, HELLO sent, waiting for the reply      */
	NET_READY,       /* HELLO exchanged; messages flow                    */
} NetState;

/* One complete inbound frame. `data` points into the receive buffer and stays
 * valid only until the next Net_Poll -- copy anything you keep. */
typedef struct {
	u8        type;   /* GC_S_*                              */
	u16       len;    /* payload bytes, excluding the type   */
	const u8 *data;   /* payload, or NULL when len == 0      */
} NetMsg;

/* Bring up the interface (DHCP). Blocks for up to `retries` attempts, so call
 * it once at boot with something on screen -- if_config is the one part of
 * this that cannot be made asynchronous. Returns 1 on success. Safe to call
 * again; a second call with the interface already up is a no-op. */
int Net_Init(int retries);

/* Dotted-quad addresses from the last successful Net_Init, or "" . */
const char *Net_LocalIp(void);
const char *Net_Gateway(void);

/* Open a socket to the proxy and start the handshake. Non-blocking: the
 * connect completes inside later Net_Poll calls. Re-calling while connected
 * tears the old link down first. */
void Net_Connect(const char *ip, u16 port);

/* Close the socket and go back to NET_IDLE. `why` is recorded for
 * Net_LastError; pass NULL for a clean local shutdown. */
void Net_Disconnect(const char *why);

/* Reconnect to the address Net_Connect last used, after `delayMs`. The
 * reconnect is automatic on an unexpected drop; this forces one. */
void Net_Reconnect(void);

/* Pump the link and yield one complete inbound frame per call. Returns 1 and
 * fills `out` while frames remain, 0 when the buffer is drained -- so the
 * caller loops until it returns 0. HELLO, DISCONNECT and PING are handled
 * here and never surface to the caller. */
int Net_Poll(NetMsg *out);

/* Queue an outbound frame. Returns 1 if it was queued, 0 if the link is not
 * NET_READY or the send buffer is full (which means the proxy has stopped
 * reading -- the link is already in trouble). `payload` may be NULL when
 * `len` is 0. */
int Net_Send(u8 type, const void *payload, u16 len);

/* ---- status, for the on-screen indicator ------------------------------- */

NetState    Net_GetState(void);
const char *Net_StateText(void);   /* "ready", "connecting", ...           */
const char *Net_LastError(void);   /* why the last link ended, or ""       */
u16         Net_RttMs(void);       /* round trip as measured by the proxy  */
u32         Net_BytesIn(void);
u32         Net_BytesOut(void);

#endif

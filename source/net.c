#include <string.h>
#include <errno.h>
#include <fcntl.h>          /* F_SETFL; lwIP uses the same values as newlib */
#include <gccore.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>

#include "net.h"
#include "blockmap_gen.h"   /* BLOCKMAP_HASH */

/* Receive buffer: one maximum frame, plus room for the partial next one that
 * arrived in the same TCP segment. Compacted (not a ring) so a whole frame is
 * always contiguous and can be handed to a caller as a plain pointer -- a ring
 * would have to either copy every wrapped frame out or expose a split buffer
 * to every handler, and neither is worth saving 8 KB here.
 *
 * Send buffer: a ring, because sends are small (MOVE is 30 bytes at 20 Hz) and
 * frequent, and a full one only happens when the proxy has stopped reading. */
#define RX_CAP (GCLINK_MAX_PAYLOAD + GCLINK_HEADER * 2)
#define TX_CAP 4096

/* How much a single drain may pull off the socket. One TCP receive window is
 * not enough: lwIP's is a couple of kilobytes, and a game with sixteen players
 * in it streams entity updates at tens of KB/s, so a drain that reads once and
 * stops falls further behind every frame until the keepalive it is starving
 * declares the link dead. This is ~40x the real rate -- generous enough never
 * to be the limit, bounded so a flooded socket cannot hold a frame open. */
#define RX_PER_DRAIN (64 * 1024)

/* Give the proxy this long to answer HELLO before assuming it is not one. */
#define HANDSHAKE_TIMEOUT_MS 5000
/* Wait this long after a drop before dialling again, so a proxy that is down
 * does not turn into a connect storm. */
#define RECONNECT_DELAY_MS   2000

static struct {
	int      inited;                 /* if_config succeeded              */
	s32      sock;                   /* -1 when closed                   */
	NetState state;

	char     ip[16], gateway[16], netmask[16];
	char     host[16];               /* proxy address for reconnects     */
	u16      port;
	char     lastError[64];

	u8       rx[RX_CAP];
	u32      rxHead;                 /* first byte not yet handed out    */
	u32      rxLen;                  /* bytes held from rxHead           */
	u32      rxBudget;               /* bytes this drain may still read  */

	u8       tx[TX_CAP];
	u32      txHead, txTail;         /* ring, txHead == txTail is empty  */

	u64      stateSince;             /* gettime() when `state` was entered */
	u64      lastPing;               /* gettime() of the last GC_S_PING    */
	u64      lastData;               /* gettime() of the last byte received*/
	u64      retryAt;                /* gettime() to redial at, 0 = never  */
	u16      rttMs;
	u32      bytesIn, bytesOut;
/* Designated so the one field that is not zero at rest stays correct however
 * the struct is reordered later; 0 is a legal socket descriptor, so "closed"
 * has to be -1 rather than the zero the rest gets by being static. */
} N = { .sock = -1, .state = NET_DOWN, .rxBudget = RX_PER_DRAIN };

/* ---- small helpers ------------------------------------------------------ */

static u32 ms_since(u64 t) {
	if (!t) return 0;
	return (u32)(ticks_to_millisecs(gettime() - t));
}

static void set_state(NetState s) {
	N.state = s;
	N.stateSince = gettime();
}

static void set_error(const char *why) {
	if (!why) { N.lastError[0] = '\0'; return; }
	strncpy(N.lastError, why, sizeof(N.lastError) - 1);
	N.lastError[sizeof(N.lastError) - 1] = '\0';
}

/* lwIP reports "nothing to do right now" as a negative errno, and the exact
 * one differs between calls (recv says EAGAIN, a connect in flight says
 * EALREADY). Everything else on a socket is fatal to the link. */
static int would_block(s32 r) {
	return r == -EAGAIN || r == -EWOULDBLOCK || r == -EINPROGRESS ||
	       r == -EALREADY;
}

/* Is the socket readable / writable right now?
 *
 * Every recv and send below is gated on this rather than on O_NONBLOCK. The
 * O_NONBLOCK request is still made, but libbba's lwIP is old and its exact
 * behaviour is not something to bet a frozen console on -- a zero-timeout
 * net_select answers the same question without depending on it, and costs
 * nothing at 60 Hz. */
static int sock_ready(int forWrite) {
	if (N.sock < 0) return 0;
	fd_set set;
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	FD_ZERO(&set);
	FD_SET(N.sock, &set);
	s32 r = forWrite ? net_select(N.sock + 1, NULL, &set, NULL, &tv)
	                 : net_select(N.sock + 1, &set, NULL, NULL, &tv);
	return r > 0 && FD_ISSET(N.sock, &set);
}

static void close_socket(void) {
	if (N.sock >= 0) net_close(N.sock);
	N.sock = -1;
	N.rxHead = N.rxLen = 0;
	N.rxBudget = RX_PER_DRAIN;
	N.txHead = N.txTail = 0;
	N.lastPing = 0;
	N.lastData = 0;
	N.rttMs = 0;
}

/* ---- outbound ----------------------------------------------------------- */

static u32 tx_used(void) { return N.txHead - N.txTail; }

static int tx_push(const u8 *p, u32 n) {
	if (tx_used() + n > TX_CAP) return 0;
	while (n--) N.tx[N.txHead++ % TX_CAP] = *p++;
	return 1;
}

/* Push whatever the socket will take, without blocking. A short write is
 * normal and simply leaves the rest queued for the next frame. */
static void tx_flush(void) {
	while (N.sock >= 0 && tx_used() && sock_ready(1)) {
		u32 tail = N.txTail % TX_CAP;
		u32 run  = TX_CAP - tail;              /* to the end of the ring */
		if (run > tx_used()) run = tx_used();
		s32 sent = net_send(N.sock, N.tx + tail, (s32)run, 0);
		if (sent > 0) { N.txTail += (u32)sent; N.bytesOut += (u32)sent; continue; }
		if (sent == 0 || would_block(sent)) return;
		Net_Disconnect("send failed");
		return;
	}
}

/* Frame and queue, bypassing the NET_READY gate -- the handshake has to send
 * HELLO before it is ready, and PONG has to answer during any state. */
static int send_raw(u8 type, const void *payload, u16 len) {
	if (N.sock < 0 || len > GCLINK_MAX_PAYLOAD) return 0;
	u8 hdr[GCLINK_HEADER];
	gc_put_u16(hdr, (u16)(len + 1));           /* length counts the type */
	gc_put_u8(hdr + 2, type);
	if (tx_used() + GCLINK_HEADER + len > TX_CAP) return 0;
	tx_push(hdr, GCLINK_HEADER);
	if (len) tx_push((const u8 *)payload, len);
	tx_flush();
	return 1;
}

int Net_Send(u8 type, const void *payload, u16 len) {
	if (N.state != NET_READY) return 0;
	return send_raw(type, payload, len);
}

/* ---- connect ------------------------------------------------------------ */

int Net_Init(int retries) {
	if (N.inited) return 1;
	/* if_config runs DHCP and blocks for up to `retries` attempts. There is no
	 * asynchronous form of it on this platform, which is why it belongs at
	 * boot with a status line on screen rather than anywhere near the frame
	 * loop. */
	s32 r = if_config(N.ip, N.netmask, N.gateway, TRUE, retries);
	if (r < 0) {
		set_error("DHCP failed");
		set_state(NET_DOWN);
		return 0;
	}
	N.inited = 1;
	set_state(NET_IDLE);
	return 1;
}

const char *Net_LocalIp(void) { return N.ip; }
const char *Net_Gateway(void) { return N.gateway; }

/* TCP is up: introduce ourselves and start the handshake clock. */
static void send_hello(void) {
	u8 pl[5];
	gc_put_u8(pl, GCLINK_VERSION);
	gc_put_u32(pl + 1, BLOCKMAP_HASH);
	send_raw(GC_C_HELLO, pl, sizeof(pl));
	set_state(NET_HANDSHAKE);
}

static void dial(void) {
	struct sockaddr_in a;

	close_socket();
	N.retryAt = 0;

	N.sock = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (N.sock == INVALID_SOCKET || N.sock < 0) {
		N.sock = -1;
		set_error("no socket");
		set_state(NET_IDLE);
		N.retryAt = gettime() + millisecs_to_ticks(RECONNECT_DELAY_MS);
		return;
	}
	/* Non-blocking from the start, so the connect itself cannot stall a
	 * frame: an unreachable proxy would otherwise hang the console for the
	 * full TCP timeout. */
	net_fcntl(N.sock, F_SETFL, O_NONBLOCK);

	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(N.port);
	a.sin_addr.s_addr = inet_addr(N.host);

	s32 r = net_connect(N.sock, (struct sockaddr *)&a, sizeof(a));
	if (r == 0 || r == -EISCONN) {
		/* The proxy is usually on the same machine, so this normally finishes
		 * here rather than going through EINPROGRESS. */
		send_hello();
		return;
	}
	if (would_block(r)) { set_state(NET_CONNECTING); return; }

	close_socket();
	set_error("connect refused");
	set_state(NET_IDLE);
	N.retryAt = gettime() + millisecs_to_ticks(RECONNECT_DELAY_MS);
}

void Net_Connect(const char *ip, u16 port) {
	if (!N.inited) { set_error("network down"); return; }
	strncpy(N.host, ip, sizeof(N.host) - 1);
	N.host[sizeof(N.host) - 1] = '\0';
	N.port = port;
	set_error(NULL);
	dial();
}

void Net_Reconnect(void) {
	if (N.host[0]) dial();
}

void Net_Disconnect(const char *why) {
	int wasUp = N.sock >= 0;
	close_socket();
	set_error(why);
	set_state(NET_IDLE);
	/* An unexpected drop redials; a deliberate one (why == NULL) stays down
	 * until something asks for a connection again. */
	N.retryAt = (why && wasUp && N.host[0])
	            ? gettime() + millisecs_to_ticks(RECONNECT_DELAY_MS) : 0;
}

/* A connect still in flight finishes when the socket becomes writable.
 *
 * Note what this deliberately does NOT do: call net_connect again. That is the
 * textbook completion test on modern stacks (EALREADY, then EISCONN), and on
 * libbba's lwIP it wedges the console outright -- the TCP handshake completes,
 * the proxy sees the connection, and the second net_connect never returns.
 * select's write set answers the same question and comes back. */
static void pump_connecting(void) {
	if (sock_ready(1)) { send_hello(); return; }
	if (ms_since(N.stateSince) > HANDSHAKE_TIMEOUT_MS) {
		close_socket();
		set_error("connect timed out");
		set_state(NET_IDLE);
		N.retryAt = gettime() + millisecs_to_ticks(RECONNECT_DELAY_MS);
	}
}

/* ---- inbound ------------------------------------------------------------ */

/* Slide the unconsumed remainder back to the front, but only when the tail has
 * actually run out of room.
 *
 * Consuming a message just advances rxHead, so a drain that hands out two
 * hundred small frames costs two hundred pointer bumps rather than two hundred
 * memmoves of the whole buffer -- which, at the rate a busy game streams
 * entity updates, is the difference between amortised O(1) and O(n^2) in the
 * hot path. The move that does happen is what makes the "valid until the next
 * Net_Poll" contract exactly true: a returned pointer stays good until the
 * next call, and no longer. */
static void rx_make_room(void) {
	if (N.rxHead == 0) return;
	if (N.rxHead + N.rxLen < RX_CAP) return;
	if (N.rxLen) memmove(N.rx, N.rx + N.rxHead, N.rxLen);
	N.rxHead = 0;
}

/* Read whatever the socket has, up to this drain's budget. Returns the bytes
 * taken in, so the caller can tell "nothing left" from "more to come". */
static u32 rx_fill(void) {
	u32 total = 0;
	while (N.sock >= 0 && N.rxBudget && sock_ready(0)) {
		rx_make_room();
		u32 room = RX_CAP - N.rxHead - N.rxLen;
		if (room > N.rxBudget) room = N.rxBudget;
		if (!room) break;
		s32 got = net_recv(N.sock, N.rx + N.rxHead + N.rxLen, (s32)room, 0);
		if (got > 0) {
			N.rxLen += (u32)got;
			N.bytesIn += (u32)got;
			N.rxBudget -= (u32)got;
			N.lastData = gettime();
			total += (u32)got;
			continue;
		}
		if (got == 0) { Net_Disconnect("proxy closed the link"); break; }
		if (would_block(got)) break;
		Net_Disconnect("recv failed");
		break;
	}
	return total;
}

/* HELLO / DISCONNECT / PING are the link's own business and never reach the
 * caller. Returns 1 if the frame was consumed here. */
static int handle_internal(const NetMsg *m) {
	switch (m->type) {
	case GC_S_HELLO: {
		if (m->len < 6) { Net_Disconnect("short HELLO"); return 1; }
		u8  ver    = gc_get_u8(m->data);
		u32 hash   = gc_get_u32(m->data + 1);
		u8  result = gc_get_u8(m->data + 5);
		if (result == GCLINK_HELLO_VERSION || ver != GCLINK_VERSION) {
			Net_Disconnect("GCLink version mismatch");
		} else if (result == GCLINK_HELLO_BLOCKMAP || hash != BLOCKMAP_HASH) {
			/* Loud on purpose. A stale blockmap on either side renders a whole
			 * map as the wrong blocks, and silently is the worst way to find
			 * that out. Re-run tools/gen_blockmap.py and rebuild both ends. */
			Net_Disconnect("blockmap hash mismatch");
		} else if (result == GCLINK_HELLO_BUSY) {
			Net_Disconnect("proxy already has a console");
		} else if (result != GCLINK_HELLO_OK) {
			Net_Disconnect("proxy rejected HELLO");
		} else {
			N.lastPing = gettime();   /* start the keepalive clock */
			set_state(NET_READY);
		}
		return 1;
	}
	case GC_S_DISCONNECT: {
		char why[64];
		u32 n = m->len < sizeof(why) - 1 ? m->len : sizeof(why) - 1;
		if (n) memcpy(why, m->data, n);
		why[n] = '\0';
		Net_Disconnect(n ? why : "proxy disconnected");
		return 1;
	}
	case GC_S_PING: {
		if (m->len < 4) return 1;
		N.lastPing = gettime();
		if (m->len >= 6) N.rttMs = gc_get_u16(m->data + 4);
		send_raw(GC_C_PONG, m->data, 4);   /* echo the token */
		return 1;
	}
	default:
		return 0;
	}
}

int Net_Poll(NetMsg *out) {
	/* Housekeeping on every call, not once per frame. It is a handful of
	 * compares plus a send that early-outs on an empty queue, and keying it to
	 * "the first call of a frame" meant a caller that stops draining early --
	 * T11 does, on a map change -- skipped the connect and keepalive machine
	 * for a whole frame. */
	if (N.state == NET_IDLE && N.retryAt && gettime() >= N.retryAt) dial();
	if (N.state == NET_CONNECTING) pump_connecting();
	tx_flush();
	if (N.state == NET_HANDSHAKE &&
	    ms_since(N.stateSince) > HANDSHAKE_TIMEOUT_MS) {
		Net_Disconnect("no HELLO from the proxy");
	}
	/* TCP will not notice a proxy that has stopped talking but not closed --
	 * a wedged process, a pulled cable on its side. The PING clock will.
	 *
	 * But bytes arriving are proof of life too, and they have to count. The
	 * Broadband Adapter's lwIP has a small receive window and delayed ACKs,
	 * which puts its practical ceiling around ten to fifteen KB/s; past that
	 * the proxy's send queue backs up and a PING can sit behind a second or
	 * more of entity updates. Keying liveness on the PING alone meant a busy
	 * game -- the exact case this link exists for -- tore its own session down
	 * every eight seconds while data was streaming in the whole time. Latency
	 * under a burst is a cost; dropping the link over it is a bug. */
	if (N.state == NET_READY &&
	    ms_since(N.lastPing) > GCLINK_PING_TIMEOUT_MS &&
	    ms_since(N.lastData) > GCLINK_PING_TIMEOUT_MS) {
		Net_Disconnect("proxy stopped responding");
	}

	for (;;) {
		while (N.rxLen >= GCLINK_HEADER) {
			const u8 *p = N.rx + N.rxHead;
			u16 flen = gc_get_u16(p);          /* type + payload */
			if (flen < 1 || flen > GCLINK_MAX_PAYLOAD + 1) {
				Net_Disconnect("bad frame length");
				N.rxBudget = RX_PER_DRAIN;
				return 0;
			}
			u32 total = GCLINK_HEADER + (u32)flen - 1;
			if (N.rxLen < total) break;        /* the rest is still in flight */

			NetMsg m;
			m.type = p[2];
			m.len  = (u16)(flen - 1);
			m.data = m.len ? p + GCLINK_HEADER : NULL;
			N.rxHead += total;
			N.rxLen  -= total;

			if (!handle_internal(&m)) {
				if (out) *out = m;
				return 1;
			}
			/* Consumed internally: look at the next one. A disconnect inside
			 * the handler resets the buffer, so re-check. */
			if (N.sock < 0) { N.rxBudget = RX_PER_DRAIN; return 0; }
		}

		/* Nothing complete left. Top the buffer up and look again rather than
		 * waiting for the next frame -- see RX_PER_DRAIN. */
		if (!rx_fill()) break;
	}

	/* The drain finished, so this is the end of a frame's worth of reading;
	 * give the next one its full budget back. */
	N.rxBudget = RX_PER_DRAIN;
	return 0;
}

/* ---- status ------------------------------------------------------------- */

NetState Net_GetState(void) { return N.state; }

const char *Net_StateText(void) {
	switch (N.state) {
	case NET_DOWN:       return "no network";
	case NET_IDLE:       return N.retryAt ? "retrying" : "offline";
	case NET_CONNECTING: return "connecting";
	case NET_HANDSHAKE:  return "handshake";
	case NET_READY:      return "ready";
	}
	return "?";
}

const char *Net_LastError(void) { return N.lastError; }
u16 Net_RttMs(void)    { return N.rttMs; }
u32 Net_BytesIn(void)  { return N.bytesIn; }
u32 Net_BytesOut(void) { return N.bytesOut; }

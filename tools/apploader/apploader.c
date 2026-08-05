/*---------------------------------------------------------------------------
 * A minimal GameCube apploader, so msw-gcn can ship as a bootable disc image
 * instead of only a .dol. Nothing here is derived from a retail apploader --
 * see the protocol below; it is small enough to write from the spec.
 *
 * The BS2 bootrom reads the 0x20-byte apploader header at disc offset 0x2440,
 * copies this image to 0x81200000, and calls its entry point with pointers to
 * three function slots. It then drives us:
 *
 *      init(report)                     -- once, before anything else
 *      while (main(&dst, &len, &off))   -- BS2 performs each DVD read itself
 *      entry = close()                  -- BS2 jumps to whatever we return
 *
 * The key point is that we never touch the DVD hardware: main() only *names*
 * the next chunk of disc to copy and where it goes, and the BS2 does the read
 * before calling main() again. So the whole job is to pull in the DOL header,
 * then walk its section table handing back one section per call.
 *
 * Runs with no libc, no crt0 and no zeroed .bss -- tools/mkiso.py pads the raw
 * image out to _end so our own .bss arrives on disc as zeros.
 *-------------------------------------------------------------------------*/

typedef unsigned char u8;
typedef unsigned int u32;

#define DOL_TEXT	7
#define DOL_DATA	11
#define DOL_SECTIONS	(DOL_TEXT + DOL_DATA)

/* The 0x100-byte header at the front of a .dol. */
typedef struct {
	u32 off[DOL_SECTIONS];
	u32 addr[DOL_SECTIONS];
	u32 size[DOL_SECTIONS];
	u32 bss_addr;
	u32 bss_size;
	u32 entry;
	u32 pad[7];
} dol_header;

/* Where the .dol sits on the disc. tools/mkiso.py only knows that once it has
 * laid the image out, so it finds the magic in the built binary and overwrites
 * the word after it. volatile keeps gcc from folding in the placeholder. */
static volatile u32 g_cfg[2] = { 0x4D535749 /* 'MSWI' */, 0xFFFFFFFF };

/* A transfer may not exceed 64 DVD sectors, so anything bigger is handed back
 * a chunk at a time. Retail apploaders split the same way. */
#define READ_CHUNK	(64 * 2048)

static dol_header g_dol __attribute__((aligned(32)));
static int g_step;
static u32 g_pos;
static void (*g_report)(const char *fmt, ...);

/*---------------------------------------------------------------------------
 * Cache maintenance, so what the loader brought in is really in memory before
 * we jump into it.
 *-------------------------------------------------------------------------*/
static void dc_flush(u32 addr, u32 len)
{
	u32 a = addr & ~31u;
	u32 end = (addr + len + 31) & ~31u;

	for (; a < end; a += 32)
		__asm__ volatile ("dcbf 0,%0" :: "r" (a) : "memory");
	__asm__ volatile ("sync");
}

static void ic_invalidate(u32 addr, u32 len)
{
	u32 a = addr & ~31u;
	u32 end = (addr + len + 31) & ~31u;

	for (; a < end; a += 32)
		__asm__ volatile ("icbi 0,%0" :: "r" (a) : "memory");
	__asm__ volatile ("sync ; isync");
}

/*---------------------------------------------------------------------------
 * The BS2 interface.
 *-------------------------------------------------------------------------*/
static void app_init(void (*report)(const char *fmt, ...))
{
	/* Kept for the record only. Dolphin's BS2 hands init() a pointer into
	 * its own scratch area rather than a real OSReport, so calling this
	 * would jump into data -- never do it. */
	g_report = report;
	g_step = 0;
	g_pos = 0;
}

static int app_main(void **dst, int *len, int *off)
{
	u32 base = g_cfg[1];
	u32 size, want;
	int i;

	if (g_step == 0) {
		/* Nothing is known yet, so the first read is the DOL header
		 * itself; the next call gets to look at the section table. */
		*dst = &g_dol;
		*len = sizeof(g_dol);
		*off = (int)base;
		g_step = 1;
		g_pos = 0;
		return 1;
	}

	while (g_step <= DOL_SECTIONS) {
		i = g_step - 1;
		/* DVD transfers move whole 32-byte units. elf2dol already pads
		 * sections out to 32, so this normally changes nothing. */
		size = (g_dol.size[i] + 31) & ~31u;
		if (g_pos >= size) {		/* section done (or empty) */
			g_step++;
			g_pos = 0;
			continue;
		}
		want = size - g_pos;
		if (want > READ_CHUNK)
			want = READ_CHUNK;
		*dst = (void *)(g_dol.addr[i] + g_pos);
		*len = (int)want;
		*off = (int)(base + g_dol.off[i] + g_pos);
		g_pos += want;
		return 1;
	}

	return 0;
}

static void *app_close(void)
{
	u8 *p;
	u32 n;
	int i;

	/* Push what was just loaded out to memory. dcbf, not dcbi: on a console
	 * the DVD DMA writes behind the cache and these lines are stale-clean,
	 * so either would do -- but a loader that emulates the drive off SD or
	 * USB (Nintendont, Swiss) copies with the CPU and leaves them dirty, and
	 * invalidating there throws the game away just before jumping into it. */
	for (i = 0; i < DOL_SECTIONS; i++)
		if (g_dol.size[i])
			dc_flush(g_dol.addr[i], g_dol.size[i]);

	/* A .dol carries no image for .bss, so clearing it is the loader's job. */
	p = (u8 *)g_dol.bss_addr;
	for (n = g_dol.bss_size; n; n--)
		*p++ = 0;
	dc_flush(g_dol.bss_addr, g_dol.bss_size);

	for (i = 0; i < DOL_TEXT; i++)
		if (g_dol.size[i])
			ic_invalidate(g_dol.addr[i], g_dol.size[i]);

	return (void *)g_dol.entry;
}

/* Entry point: r3/r4/r5 are the slots the BS2 wants the three above written to. */
void _start(void (**init)(void (*report)(const char *fmt, ...)),
	    int (**main)(void **dst, int *len, int *off),
	    void *(**close)(void))
{
	*init = app_init;
	*main = app_main;
	*close = app_close;
}

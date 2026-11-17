/*
 * Virtual stream file for EL71v45 (ELKA build) and S75v52 (NSG build), RADIO_PASS; interface in vstream.h.
 *
 * On EL71 the firmware does not read media through the Obs file-stream table the way S65 does: in the
 * pmb887x emulation (2026-11-15, radio/RE-EL71.txt) the media player opened the Obs file stream (FUN_a0d218ac,
 * table A8F5C300) but read the audio itself through the file system (fopen_ws A056E6B4 -> drive ops +2C,
 * fread A056E734 -> +34, lseek A056E628 -> +40, several handles at once, reads near the end for tags).
 * So the hook sits one level lower, the way smbd attaches its share (smb/src/smb_fs.c, ghidra-el71/fs-api.txt):
 * the methods of the drive that holds the elf (4: card, or 0:) and of its driver are replaced by a copy
 * whose path methods catch <elf folder>\radiostream.mp3 and whose handle methods catch the handles opened for
 * it. The real (small) file is opened as usual; reads/seeks/size of those handles are virtual:
 *   - one linear stream of bytes = MP3 frames from mp3pass.c (fill callback), size DATA_SIZE;
 *   - the last 64 KB delivered are kept, so a second handle or a seek back (format probe, re-open) reads the
 *     same bytes again; a jump forward starts the stream there; the last MB of the "file" is zeros (no tags).
 * Methods run in the caller's task (player, Obs, MMI): state changes under LockSched.
 *
 * D49 (after D30 hung the phone, RE-EL71.txt): vs_install() only checks the drive; the methods are swapped
 * in by vs_start() right before PlayFile (nothing of ours sits in the card's path while the elf only
 * connects), and an open of the file is virtual only while a stream is armed (vs_start .. vs_stop): other
 * openers at other times (media library, explorer) get the real 417-byte file. Calls on our handles are
 * recorded in the vs_ev ring (plain stores) for the MMI log. Size 0x0FF00000: size*8 still fits in 31 bits.
 */
#ifdef HOST_TEST
#include "vs_nsg_host.h"	/* tests/host: fake tables, SWIs */
#else
#include <swilib.h>
/* D73: the tables and calls from the function library (signatures/README.md 4E1-4E9; be_elka.c checks them) */
#define RAM_DRIVERS	((uint32_t)RamFsDrivers())	/* 84E8 */
#define RAM_DRIVES	((uint32_t)RamFsDrives())	/* 84E9 */
#define FW_MSG_NEW(m, to, id, sz)	((int)GBS_CreateMsg((void **)(m), to, id, sz))	/* 4E1 -> payload */
#define FW_MSG_POST(m)	GBS_PostMsg((void **)(m))	/* 4E2 */
#define FW_FS_ALLOC	FS_MemAlloc			/* 4E3 */
#endif
#include "vstream.h"

#define N_DRIVERS	3
#define N_DRIVES	5
#define N_OPS		33

#define DATA_SIZE	0x0FF00000u	/* 255 MB: 32k = 18 h, 320k = 1.8 h; then the player stops, main.c restarts */
#define TAIL_ZONE	0x100000u	/* reads here (ID3v1/APE tag search) get zeros */
#define HIST		65536u		/* power of 2 */
#define MAX_H		6

typedef int (*op_fn)(int, int, int, int, int, int, int, int);

typedef struct {		/* 0x10 */
	int used;
	int id;
	short first, last;	/* handle range */
	op_fn *ops;
} DRIVER;

typedef struct {		/* 0x18 */
	int used;
	uint16_t letter;
	short drive;
	int driver;
	uint32_t threshold;
	uint8_t threshold_state, pad[3];
	op_fn *ops;
} DRIVE;

volatile uint32_t vs_reads;
const char *vs_fail;
#ifdef RADIO_LOG
volatile uint32_t vs_seen, vs_detects, vs_nonzero;
VS_EV vs_ev[VS_EV_N];			/* calls on our handles (kind EV_G, slot = method index) */
volatile uint32_t vs_ev_wr, vs_ev_rd, vs_ev_lost;
volatile uint16_t vs_now;
uint32_t vs_table[16];
#endif
char vs_dir[128];		/* folder of the virtual file (be_elka.c: 1:\\FasoleyRadio, else the elf's) */

static DRIVE *host;
static DRIVER *host_driver;
static op_fn *orig;
static op_fn wrap_ops[N_OPS];
static int installed, probed, host_cepid, drv_no;
static volatile int draining;	/* D72: after vs_stop: open handles still served from fill_fn */
static char our_path[160];	/* lower case, '\' */
static int our_len;

static struct { int used; int h; uint32_t pos; } hs[MAX_H];
static volatile int callers;

static vs_fill_f fill_fn;
static void *fill_ctx;
static volatile int enabled;
static uint32_t hist_end;	/* stream bytes delivered so far (virtual position of the next fresh byte) */
static uint32_t hist_have;	/* how many of the last delivered bytes are in hist[] (<= HIST) */
static uint8_t hist[HIST];

#ifdef RADIO_LOG
#define EV_CAP	400			/* events per start (the player reads ~1/s; the probe a few) */
static uint32_t ev_n;

/* under LockSched or with plain stores only: called in the firmware's tasks */
static void ev(int slot, int h, uint32_t a, uint32_t b, uint32_t r)
{
	VS_EV *e;
	if (ev_n >= EV_CAP)
		return;
	if (vs_ev_wr - vs_ev_rd >= VS_EV_N) {
		vs_ev_lost++;
		return;
	}
	ev_n++;
	e = &vs_ev[vs_ev_wr % VS_EV_N];
	e->kind = EV_G;
	e->slot = (uint8_t)slot;
	e->t = vs_now;
	e->obj = (uint32_t)(h & 0xFFFF) | (uint32_t)GBS_GetCurCepid() << 16;	/* caller's process in the top half */
	e->a = a;
	e->b = b;
	e->r = r;
	vs_ev_wr++;
}
#else
static inline void ev(int slot, int h, uint32_t a, uint32_t b, uint32_t r)
{
	(void)slot, (void)h, (void)a, (void)b, (void)r;
}
#endif

static void Enter(void) { LockSched(); ++callers; UnlockSched(); }
static void Leave(void) { LockSched(); --callers; UnlockSched(); }

/* ---------- path / handle match ---------- */

static unsigned low(unsigned c)
{
	if (c == '/')
		return '\\';
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int ours_path(int path, int wpath)
{
	int i, n;

	if (!our_len)
		return 0;
	if (path) {
		const unsigned char *p = (const unsigned char *)path;
		for (n = 0; p[n]; n++)
			;
		if (n != our_len)
			return 0;
		for (i = 0; i < n; i++)
			if (low(p[i]) != (unsigned char)our_path[i])
				return 0;
		return 1;
	}
	if (wpath) {
		const uint16_t *b = ((const WSHDR *)wpath)->wsbody;
		if (!b || b[0] != our_len)
			return 0;
		for (i = 0; i < our_len; i++)
			if (low(b[i + 1]) != (unsigned char)our_path[i])
				return 0;
		return 1;
	}
	return 0;
}

int vs_live(void)
{
	int i, n = 0;
	for (i = 0; i < MAX_H; i++)
		n += hs[i].used;
	return n;
}

/* ---------- the virtual bytes ---------- */

static void bcopy_(uint8_t *d, const uint8_t *s, uint32_t n)
{
	while (n--)
		*d++ = *s++;
}

static void bzero_(uint8_t *d, uint32_t n)
{
	while (n--)
		*d++ = 0;
}

/* fresh stream bytes into buf, kept in the history */
static void fresh(uint8_t *buf, uint32_t n)
{
	uint32_t k, off;

	if ((enabled || draining) && fill_fn) {
		fill_fn(fill_ctx, buf, (int)n);
		vs_reads++;
		VS_DIAG(vs_nonzero++;)
	} else
		bzero_(buf, n);
	for (k = 0; k < n; ) {
		uint32_t c;
		off = (hist_end + k) & (HIST - 1);
		c = HIST - off;
		if (c > n - k)
			c = n - k;
		bcopy_(hist + off, buf + k, c);
		k += c;
	}
	hist_end += n;
	hist_have = hist_have + n > HIST ? HIST : hist_have + n;
}

/* n bytes at virtual position pos */
static void vread(uint32_t pos, uint8_t *buf, uint32_t n)
{
	uint32_t k;

	if (pos >= DATA_SIZE - TAIL_ZONE) {
		bzero_(buf, n);
		return;
	}
	if (pos > hist_end) {		/* jump forward: the stream goes on from there */
		hist_end = pos;
		hist_have = 0;
	}
	if (pos < hist_end) {
		if (pos < hist_end - hist_have) {	/* older than the history: what comes next is all we have */
			hist_end = pos;
			hist_have = 0;
		} else {
			while (n && pos < hist_end) {
				uint32_t off = pos & (HIST - 1);
				k = HIST - off;
				if (k > hist_end - pos)
					k = hist_end - pos;
				if (k > n)
					k = n;
				bcopy_(buf, hist + off, k);
				buf += k;
				pos += k;
				n -= k;
			}
		}
	}
	if (n)
		fresh(buf, n);
}

/* ---------- async replies (as the card driver: msg 0x1A2 from its process, smbd D59) ---------- */

static void Reply(int to, int cb, int err, int result)
{
	uint32_t *d = FW_FS_ALLOC(12), *p;
	int msg;

	if (!d)
		return;
	d[0] = (uint32_t)cb;
	d[1] = (uint32_t)(err & 0xFFFF);
	d[2] = (uint32_t)result;
	if (!(p = (uint32_t *)FW_MSG_NEW(&msg, to, 0x1A2, 12)))
		return;
	p[0] = 0;
	p[1] = (uint32_t)d;
	p[2] = 0;
	*(uint16_t *)(msg + 0xE) = (uint16_t)host_cepid;
	FW_MSG_POST(&msg);
}

static void set_err(int err, int code)
{
	if (err)
		*(uint16_t *)err = (uint16_t)code;
}

/* ---------- wrapped methods (8 args as the firmware passes them; arg 0: 0 sync, 1 async) ----------
 * D55: our file is served entirely here - own handle numbers after the card driver's range (as smbd D56:
 * the driver's last handle += MAX_H), no call into the card driver for it. D54 opened the real file with the
 * card's SYNCHRONOUS fopen from inside the player's (asynchronous) call and the phone froze right after
 * PlayFile; now nothing of ours waits for anything. Handle-method arguments (sync wrappers A056E5C8..):
 *   (s, h, err, cb)          +00 fclose, +08 fflush
 *   (s, h, x, err, cb)       +04 fstat (SWI 38D), +48 setfilesize, +54 truncate
 *   (s, h, x, y, err, cb)    +30, +34 fread, +4C, +64, +68 fwrite
 *   (s, h, off, org, err32, err, cb)  +40 lseek */

#define ARGS int s, int a, int b, int c, int d, int e, int f, int g
#define ORIG(n) orig[n](s, a, b, c, d, e, f, g)
#define E_FAIL	1

static short h_base;		/* our handles: h_base .. h_base + MAX_H - 1 */
static short saved_last;
static union { FSTATS fs; uint8_t raw[64]; } st_tmpl;	/* the real file's stats (taken before the hook) */

static int ours_h(int h)
{
	/* D70: the whole range is ours while hooked, open or not: the card driver knows only handles below h_base,
	 * so a late call on a closed handle of ours must never reach it */
	return installed && (short)h >= h_base && (short)h < h_base + MAX_H;
}

/* sync: set *err, return r; async: reply by message, return 0 (as the card's methods) */
/* D59: async answers are not posted inside the caller's call any more (D58 crashed the drm_file_system
 * process 0x4103 with a data abort in the message free A009432E right after our answers): they are queued
 * here and posted by vs_pump() from the MMI a tick later, as the card's own process answers after the call
 * returned. */
#define NQ	32
static struct { int to, cb, err, r; } q[NQ];
static volatile uint32_t q_wr, q_rd;

static volatile int pump_queued;
static void helper_pump(void)
{
	pump_queued = 0;
	vs_pump();
}

/* D61 hung the phone: the first async call (GetFileStats from process 0x4205, while the MMI sat in PlayFile)
 * was answered only by the MMI pump - which never ran. So only the DRM file system (0x4103, the D58 crash)
 * gets a deferred answer, posted by the elf's helper process (SUBPROC: never waits for the player or the
 * DRM); everyone else is answered at once as in D58 (worked). The MMI pump stays as a fallback. */
#define DRM_CEPID	0x4103
/* D69: the D58/D66 crash was our stack writes (D67), not the inline answer: the DRM is answered at once too, so
 * no answer waits for the helper (its queue also carries the network work, e.g. the close on Stop) */
#define DEFER_DRM	0

static void queue_reply(int cb, int err, int r)
{
	int to = GBS_GetCurCepid(), now = 0, kick = 0;
	if (to != DRM_CEPID || !DEFER_DRM) {
		Reply(to, cb, err, r);
		return;
	}
	LockSched();
	if (q_wr - q_rd < NQ) {
		q[q_wr % NQ].to = to;
		q[q_wr % NQ].cb = cb;
		q[q_wr % NQ].err = err;
		q[q_wr % NQ].r = r;
		q_wr++;
		if (!pump_queued)
			kick = pump_queued = 1;
	} else
		now = 1;
	UnlockSched();
	if (now)
		Reply(to, cb, err, r);
	if (kick)
		SUBPROC((void *)helper_pump);
}

int vs_pump(void)
{
	int n = 0;
	for (;;) {
		int to, cb, err, r;
		LockSched();
		if (q_rd == q_wr) {
			UnlockSched();
			return n;
		}
		to = q[q_rd % NQ].to;
		cb = q[q_rd % NQ].cb;
		err = q[q_rd % NQ].err;
		r = q[q_rd % NQ].r;
		q_rd++;
		UnlockSched();
		Reply(to, cb, err, r);
		n++;
	}
}

static int done(int s, int err, int cb, int code, int r)
{
	if (s) {
		queue_reply(cb, code, r);
		return 0;
	}
	set_err(err, code);
	return r;
}

/* +2C fopen(sync, path, wpath, flags, mode, err, cb) */
static int w_fopen(ARGS)
{
	int i, h = -1;

	if (!enabled || !ours_path(a, b))
		return ORIG(11);
	VS_DIAG(vs_seen++;)
	LockSched();
	for (i = 0; i < MAX_H && hs[i].used; i++)
		;
	if (i < MAX_H) {
		hs[i].used = 1;
		hs[i].h = h_base + i;
		hs[i].pos = 0;
		h = h_base + i;
		VS_DIAG(vs_detects++;)
	}
	ev(11, h, (uint32_t)c, (uint32_t)s, (uint32_t)i);
	UnlockSched();
	return done(s, e, f, h < 0 ? E_FAIL : 0, h);
}

/* +00 fclose(sync, h, err, cb) */
static int w_fclose(ARGS)
{
	int i;
	if (!ours_h(a))
		return ORIG(0);
	i = (short)a - h_base;
	LockSched();
	if (!hs[i].used) {		/* not open (closed twice / never opened) */
		ev(0, a, 0xFFFFFFFFu, (uint32_t)s, 1);
		UnlockSched();
		return done(s, b, c, E_FAIL, -1);
	}
	hs[i].used = 0;
	ev(0, a, hs[i].pos, (uint32_t)s, 0);
	UnlockSched();
	return done(s, b, c, 0, 0);
}

/* +08 fflush(sync, h, err, cb) */
static int w_fflush(ARGS)
{
	if (!ours_h(a))
		return ORIG(2);
	return done(s, b, c, 0, 0);
}

/* +34 fread(sync, h, buf, n, err, cb); +30 (slot 12) is the same read with a 16-bit count (FFS A0561F3C sends
 * the same request as its fread A0561FF0 with +10 = 0, result & 0xFFFF; MMC A054F818 msg 0xC): the EL71 player
 * opens radiostream.mp3 and reads through +30 first, asynchronously (D57 phone log: unnamed slot, -1 -> refused) */
static int read_at(int slot, ARGS)
{
	int i, n = c;
	uint32_t pos;

	if (slot == 12 && n > 0xFFFF)
		n = 0xFFFF;
	Enter();
	VS_DIAG(vs_seen++;)
	i = (short)a - h_base;
	if (n < 0)
		n = 0;
	LockSched();
	pos = hs[i].pos;
	if ((uint32_t)n > DATA_SIZE - pos)
		n = (int)(DATA_SIZE - pos);
	/* D70: not open = end of file. After Stop (vs_stop) the open handles keep getting the fill callback's bytes:
	 * mp3s_stop() makes it valid silent frames of the stream's format (D72), which the decoder takes at the
	 * real rate until the player's own stop and fclose. D69 served zeros (no frame: the decoder read on at once,
	 * hard hang); D71 end of file (the player rewound and retried 0-byte reads for ~4 s, phone frozen). */
	if (!hs[i].used)
		n = 0;
	vread(pos, (uint8_t *)b, (uint32_t)n);
	hs[i].pos = pos + (uint32_t)n;
	ev(slot, a, pos, (uint32_t)c | (uint32_t)s << 31, (uint32_t)n);
	UnlockSched();
	Leave();
	return done(s, d, e, 0, n);
}

static int w_fread(ARGS)
{
	return ours_h(a) ? read_at(13, s, a, b, c, d, e, f, g) : ORIG(13);
}

static int w_12(ARGS)
{
	return ours_h(a) ? read_at(12, s, a, b, c, d, e, f, g) : ORIG(12);
}

/* +40 lseek(sync, h, offset, origin, err32, err, cb) */
static int w_lseek(ARGS)
{
	int i;
	int64_t np;

	if (!ours_h(a))
		return ORIG(16);
	VS_DIAG(vs_seen++;)
	i = (short)a - h_base;
	LockSched();
	np = c == 1 ? (int64_t)hs[i].pos + b : c == 2 ? (int64_t)DATA_SIZE + b : (int64_t)b;
	if (np < 0)
		np = 0;
	if (np > DATA_SIZE)
		np = DATA_SIZE;
	hs[i].pos = (uint32_t)np;
	ev(16, a, (uint32_t)b, (uint32_t)c | (uint32_t)s << 31, (uint32_t)np);
	UnlockSched();
	if (!s)
		set_err(d, 0);	/* lseek(h, off, origin, err, err2): both are error words */
	return done(s, e, f, 0, (int)np);
}

static void fill_stats(FSTATS *st)
{
	if (!st)
		return;
	bcopy_((uint8_t *)st, st_tmpl.raw, sizeof(FSTATS));
	st->size = DATA_SIZE;
}

/* +04 fstat by handle (sync, h, FSTATS *, err, cb) */
static int w_fstat(ARGS)
{
	if (!ours_h(a))
		return ORIG(1);
	VS_DIAG(vs_seen++;)
	fill_stats((FSTATS *)b);
	LockSched();
	ev(1, a, (uint32_t)s, 0, 0);
	UnlockSched();
	return done(s, c, d, 0, 0);
}

/* +50 GetFileStats(sync, path, wpath, FSTATS *, err, cb): 0 = ok */
static int w_stats(ARGS)
{
	if (!enabled || !ours_path(a, b))
		return ORIG(20);
	VS_DIAG(vs_seen++;)
	fill_stats((FSTATS *)c);
	LockSched();
	ev(20, -1, (uint32_t)s, 0, 0);
	UnlockSched();
	return done(s, d, e, 0, 0);
}

/* the rest on our handles: writes, sizes, times - refused */
static int w_fail3(int n, ARGS)		/* (s, h, x, err, cb) */
{
	(void)n;
	ev(n, a, (uint32_t)s, 0, (uint32_t)-1);
	return done(s, c, d, E_FAIL, -1);
}
static int w_fail4(int n, ARGS)		/* (s, h, x, y, err, cb) */
{
	ev(n, a, (uint32_t)s, 0, (uint32_t)-1);
	return done(s, d, e, E_FAIL, -1);
}
#define FAIL3(n) static int w_##n(ARGS) { return ours_h(a) ? w_fail3(n, s, a, b, c, d, e, f, g) : ORIG(n); }
#define FAIL4(n) static int w_##n(ARGS) { return ours_h(a) ? w_fail4(n, s, a, b, c, d, e, f, g) : ORIG(n); }
FAIL3(18)	/* +48 setfilesize */
FAIL3(21)	/* +54 truncate */
FAIL4(19)	/* +4C setfiletime */
FAIL4(25)	/* +64 */
FAIL4(26)	/* +68 fwrite */

/* ---------- install ---------- */

/* EL71v45 or S75v52 (NSG, not ELKA: FasoleyRadio_NSG.elf). The file system (drive/driver tables 0x18/0x10,
 * methods, sync wrappers EL71 A056E5C8.. = S75 A025317C.., FS heap 4E3, card process 0x4100 answering 0x1A2) is
 * the same code; S75 is told by its FS_MemAlloc (library 4E3 = A0252008). */
static int nsg_fw(void)
{
#ifdef HOST_TEST
	return VS_FW_EL71V45;
#else
	return __sys_switab_addres[0x4E3] == (int)0xA0252008u ? VS_FW_S75V52 : VS_FW_EL71V45;
#endif
}

/* the drive entry of the elf's drive and its driver; 0 = not there or someone else's methods (under LockSched) */
static int find_host(void)
{
	DRIVER *drivers = (DRIVER *)RAM_DRIVERS;
	DRIVE *drives = (DRIVE *)RAM_DRIVES;
	int i;

	host = 0;
	for (i = 0; i < N_DRIVES; i++)
		if (drives[i].used && drives[i].drive == drv_no)
			host = &drives[i];
	host_driver = 0;
	if (host)
		for (i = 0; i < N_DRIVERS; i++)
			if (drivers[i].used && drivers[i].id == host->driver)
				host_driver = &drivers[i];
	return host && host_driver && host_driver->ops == host->ops && host->ops;
}

/* D49: only checks that the hook is possible; the methods are swapped in by vs_start (hook()) */
int vs_install(void)
{
	int ok;
	const char *p;

	if (installed || probed)
		return nsg_fw();
	if (RAM_DRIVERS - RAM_DRIVES != 0x78)	/* drivers right after the 5 drives (all NSG builds checked) */
		return VS_FW_NONE;
	if (vs_dir[0] < '0' || vs_dir[0] > '9' || vs_dir[1] != ':')
		return VS_FW_NONE;
	drv_no = vs_dir[0] - '0';
	our_len = 0;
	for (p = vs_dir; *p && our_len < (int)sizeof(our_path) - 20; p++)
		our_path[our_len++] = (char)low((unsigned char)*p);
	our_path[our_len++] = '\\';
	for (p = VS_NAME; *p; p++)
		our_path[our_len++] = (char)low((unsigned char)*p);
	our_path[our_len] = 0;

#ifndef HOST_TEST
	{	/* stats of the real file for fstat/GetFileStats of ours (its size replaced) */
		static char p[170];
		uint32_t err;
		sprintf(p, "%s\\%s", vs_dir, VS_NAME);
		GetFileStats(p, &st_tmpl.fs, &err);
	}
#endif
	LockSched();
	ok = find_host();
#ifdef RADIO_LOG
	if (ok) {
		vs_table[0] = (uint32_t)host;
		vs_table[1] = (uint32_t)host->ops;
		vs_table[2] = (uint32_t)host_driver;
		vs_table[3] = (uint32_t)host->driver;
		vs_table[4] = (uint32_t)host_driver->first << 16 | (uint16_t)host_driver->last;
		vs_table[5] = (uint32_t)host->ops[11];
		vs_table[6] = (uint32_t)host->ops[13];
		vs_table[7] = (uint32_t)host->ops[16];
	}
#endif
	UnlockSched();
	if (!ok) {
		vs_fail = "Drive methods already taken";
		our_len = 0;
		return VS_FW_NONE;
	}
	host_cepid = drv_no == 4 ? 0x4100 : 0x4101;
	probed = 1;
	return nsg_fw();
}

/* swap the drive's methods for ours (once; they stay until vs_uninstall at exit) */
static int hook(void)
{
	DRIVER *drivers = (DRIVER *)RAM_DRIVERS;
	int i;

	if (installed)
		return 1;
	if (!probed)
		return 0;
	LockSched();
	if (!find_host()) {
		UnlockSched();
		vs_fail = "Drive methods already taken";
		return 0;
	}
	h_base = (short)(host_driver->last + 1);
	for (i = 0; i < N_DRIVERS; i++)		/* our handles must be nobody's */
		if (drivers[i].used && &drivers[i] != host_driver && drivers[i].last >= h_base &&
		    drivers[i].first < h_base + MAX_H) {
			UnlockSched();
			vs_fail = "No free file numbers";
			return 0;
		}
	orig = host->ops;
	for (i = 0; i < N_OPS; i++)
		wrap_ops[i] = orig[i];
	wrap_ops[0] = w_fclose;
	wrap_ops[1] = w_fstat;
	wrap_ops[2] = w_fflush;
	wrap_ops[11] = w_fopen;
	wrap_ops[12] = w_12;
	wrap_ops[13] = w_fread;
	wrap_ops[16] = w_lseek;
	wrap_ops[18] = w_18;
	wrap_ops[19] = w_19;
	wrap_ops[20] = w_stats;
	wrap_ops[21] = w_21;
	wrap_ops[25] = w_25;
	wrap_ops[26] = w_26;
	saved_last = host_driver->last;
	host_driver->last = (short)(h_base + MAX_H - 1);
	host_driver->ops = wrap_ops;
	host->ops = wrap_ops;
	installed = 1;
	UnlockSched();
	return 1;
}

int vs_hooked(void)
{
	return installed;
}

/* only when vs_live() == 0; callers still inside our methods are waited for by vs_busy() */
int vs_uninstall(void)
{
	if (!installed)
		return 1;
	LockSched();
	if (host->ops != wrap_ops || host_driver->ops != wrap_ops) {
		UnlockSched();
		return 0;		/* another elf wrapped over us: its copy calls our methods */
	}
	host->ops = orig;
	host_driver->ops = orig;
	if (host_driver->last == h_base + MAX_H - 1)
		host_driver->last = saved_last;
	installed = 0;
	UnlockSched();
	return 1;
}

int vs_busy(void)
{
	return callers + (int)(q_wr - q_rd);
}

void vs_stop(void)
{
	LockSched();
	draining = enabled;	/* handles already open read on (silent frames) until closed */
	enabled = 0;		/* new opens of the file get the real one */
	UnlockSched();
}

void vs_start(vs_fill_f fill, void *ctx, int rate, int ch)
{
	(void)rate;
	(void)ch;
	hook();
	VS_DIAG(ev_n = 0;)
	LockSched();
	enabled = 0;
	draining = 0;
	fill_fn = fill;
	fill_ctx = ctx;
	hist_end = 0;
	hist_have = 0;
	enabled = 1;
	UnlockSched();
}

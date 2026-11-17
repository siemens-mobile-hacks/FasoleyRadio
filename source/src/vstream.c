/*
 * Virtual WAV stream over the Obs file stream (see vstream.h). From s65-pcmplayer/src/port/vstream.c:
 * the ring is replaced by a fill callback, the addresses are chosen by firmware (S65v58 / SK65v50).
 * Object layout and slot meanings: s65-mp3/src/fw.h, s65-mp3/src/vwav.c.
 */
#include <stdint.h>
#include <stddef.h>
#include "vstream.h"

/* Obs file stream object (0xEC bytes) */
#define OBJ_CHUNK	0xC0
#define OBJ_SIZE	0xC4
#define OBJ_POS		0xCC
#define OBJ_EOF		0xD0
#define OBJ_PATH	0xB4
#define OBJ_VT		0xE8
#define FLD(o, off, t)	(*(t *)((char *)(o) + (off)))

#define VT_SLOTS	13
enum { VT_DESTROY, VT_READ, VT_WRITE, VT_TELL, VT_SEEK, VT_GETC, VT_RELEASE, VT_PUSH, VT_GET,
	VT_SIZE, VT_NOTIFY, VT_REMAIN, VT_ONE };

typedef int (*vt_obj_f)(void *obj);
typedef int (*vt_rw_f)(void *buf, int len, void *obj);
typedef int (*vt_seek_f)(void *obj, int pos, int whence);
typedef int (*vt_get_f)(void *obj, void **ptr, int *len, int *eof);

typedef struct {
	uint32_t vt_flash;	/* flash image of the table */
	uint32_t vt_ram;	/* global table, copied from flash at boot */
	uint32_t vt_destroy;	/* stock slot 0 (nobody patches it) */
	uint32_t obs_alloc, obs_free;
} FWADDR;

#ifdef HOST_TEST
#include "vs_host.h"
#else
#include <swilib.h>
/* s65-mp3/src/fw.h: S65v58 and SK65v50 (same code, other addresses; checked in flashes/SK65v50.bin and the
 * phone dump sk65-sw50-issued.bin: table A009B170 -> copy record A00953B4 -> A8FDF000, obs_alloc/free code
 * identical to S65's) */
/* D73: the RAM table and Obs_MemAlloc/Free come from the function library (84E7, 4E5, 4E6; be_sg.c checks
 * them; S65v58 A87E4D64 A097B868 A097B874, SK65v50 A8FE4DBC A030B148 A030B154); the flash table and the stock
 * destroy slot still tell the phone */
static FWADDR fw_list[] = {
	{ 0xA009B454, 0, 0xA09A9F64, 0, 0 },	/* S65v58 */
	{ 0xA009B170, 0, 0xA0363D24, 0, 0 },	/* SK65v50 */
};
#endif

#define GET_SZ		0x4000	/* bytes per get: as s65-mp3 (stock: 0x2000); half the calls of the player */
#define DATA_SIZE	0x7FF00000u	/* "endless" file; the elf stops playback itself */
#define MAX_OBJ		4

volatile uint32_t vs_reads;
const char *vs_fail;
#ifdef RADIO_LOG
volatile uint32_t vs_seen, vs_detects, vs_nonzero;
VS_EV vs_ev[VS_EV_N];
volatile uint32_t vs_ev_wr, vs_ev_rd, vs_ev_lost;
volatile uint16_t vs_now;
uint32_t vs_table[16];
static uint32_t others_logged;

/* slot calls logged per stream start (then only the counters): 44.1 kHz stereo is ~60 calls/s, too much log */
#define EV_PER_START	150
static int ev_budget = EV_PER_START;

static void ev(int kind, int slot, void *obj, uint32_t a, uint32_t b, uint32_t r)
{
	uint32_t w = vs_ev_wr;
	VS_EV *e;

	if ((kind == EV_S || kind == EV_STOCK) && --ev_budget < 0)
		return;

	if (w - vs_ev_rd >= VS_EV_N) {
		vs_ev_lost++;
		return;
	}
	e = &vs_ev[w % VS_EV_N];
	e->kind = (uint8_t)kind;
	e->slot = (uint8_t)slot;
	e->t = vs_now;
	e->obj = (uint32_t)obj;
	e->a = a;
	e->b = b;
	e->r = r;
	vs_ev_wr = w + 1;
}
#else
static inline void ev(int kind, int slot, void *obj, uint32_t a, uint32_t b, uint32_t r)
{
	(void)kind, (void)slot, (void)obj, (void)a, (void)b, (void)r;
}
#endif

static const FWADDR *fw;
static uint32_t prev[VT_SLOTS];		/* table before us (s65-mp3 det_* or stock) */
static uint32_t ours[VT_SLOTS];		/* per-object table of VS_NAME objects */
static uint8_t hdr[VS_HDR];
static void *live[MAX_OBJ];
static int installed;
static vs_fill_f fill_fn;
static void *fill_ctx;
static volatile int enabled;

#define GT	((volatile uint32_t *)fw->vt_ram)
#ifdef HOST_TEST
#define obs_alloc(n)	(((void *(*)(int))fw->obs_alloc)(n))
#define obs_free(p)	(((void (*)(void *))fw->obs_free)(p))
#else
#define obs_alloc(n)	Obs_MemAlloc(n)
#define obs_free(p)	Obs_MemFree(p)
#endif

static void vcpy(void *d, const void *s, uint32_t n)
{
	uint8_t *dd = d;
	const uint8_t *ss = s;
	while (n--)
		*dd++ = *ss++;
}

static void vzero(void *d, uint32_t n)
{
	uint8_t *dd = d;
	while (n--)
		*dd++ = 0;
}

static int is_ours_name(const char *p)
{
	static const char n[] = VS_NAME;
	int lp = 0, ln = sizeof(n) - 1, i;
	char c;

	if (!p)
		return 0;
	while (p[lp])
		lp++;
	if (lp < ln)
		return 0;
	for (i = 0; i < ln; i++) {
		c = p[lp - ln + i];
		if (c >= 'A' && c <= 'Z')
			c += 32;
		if (c != n[i])
			return 0;
	}
	return lp == ln || p[lp - ln - 1] == '\\' || p[lp - ln - 1] == '/';
}

static int track(void *obj)
{
	int i, f = -1;
	for (i = 0; i < MAX_OBJ; i++) {
		if (live[i] == obj)
			return 1;
		if (!live[i] && f < 0)
			f = i;
	}
	if (f < 0)
		return 0;
	live[f] = obj;
	return 1;
}

static void untrack(void *obj)
{
	int i;
	for (i = 0; i < MAX_OBJ; i++)
		if (live[i] == obj)
			live[i] = 0;
}

int vs_live(void)
{
	int i, n = 0;
	for (i = 0; i < MAX_OBJ; i++)
		n += live[i] != 0;
	return n;
}

/* object of VS_NAME seen for the first time through the global table */
static int detect(void *obj)
{
	VS_DIAG(vs_seen++;)
	if (FLD(obj, OBJ_VT, uint32_t) != fw->vt_ram)
		return 0;	/* ours already, or another table (s65-mp3's own, stock flash) */
	{
		uint32_t pp = FLD(obj, OBJ_PATH, uint32_t);	/* a sane pointer before reading the name */
#ifdef HOST_TEST
		if (!fill_fn || !pp || !is_ours_name((const char *)pp))
#else
		if (!fill_fn || pp < 0xA0000000u || pp >= 0xB0000000u || !is_ours_name((const char *)pp))
#endif
		{
#ifdef RADIO_LOG
			if (others_logged < 60) {
				others_logged++;
				ev(EV_OTHER, 0, obj, pp, FLD(obj, OBJ_SIZE, uint32_t), (uint32_t)fill_fn);
			}
#endif
			return 0;
		}
	}
	if (!track(obj)) {
		ev(EV_TRACKFAIL, 0, obj, 0, 0, 0);
		return 0;
	}
	VS_DIAG(vs_detects++;)
	ev(EV_TAKEN, 0, obj, FLD(obj, OBJ_PATH, uint32_t), FLD(obj, OBJ_SIZE, uint32_t), FLD(obj, OBJ_POS, uint32_t));
	FLD(obj, OBJ_VT, uint32_t *) = ours;
	FLD(obj, OBJ_SIZE, uint32_t) = VS_HDR + DATA_SIZE;
	FLD(obj, OBJ_POS, uint32_t) = 0;
	FLD(obj, OBJ_EOF, uint32_t) = 0;
	return 1;
}

/* virtual bytes at obj+0xCC: header, then PCM from the callback (silence when stopped) */
static int vread(void *obj, uint8_t *buf, int len)
{
	uint32_t pos = FLD(obj, OBJ_POS, uint32_t), size = VS_HDR + DATA_SIZE, k;
	int done = 0;

	track(obj);	/* a clone (memcpy of the object) gets tracked on first use */
	if (len <= 0 || pos >= size)
		return 0;
	if ((uint32_t)len > size - pos)
		len = size - pos;
	if (pos < VS_HDR) {
		k = VS_HDR - pos;
		if (k > (uint32_t)len)
			k = len;
		vcpy(buf, hdr + pos, k);
		done = k;
	}
	if (done < len) {
		if (enabled && fill_fn) {
			fill_fn(fill_ctx, buf + done, len - done);
			vs_reads++;
#ifdef RADIO_LOG
			for (k = done; k < (uint32_t)len; k += 4)
				if (buf[k] | buf[k + 1]) {
					vs_nonzero++;
					break;
				}
#endif
		} else
			vzero(buf + done, len - done);
		done = len;
	}
	FLD(obj, OBJ_POS, uint32_t) = pos + done;
	return done;
}

/* ---------- per-object slots ---------- */

static int s_destroy(void *obj)
{
	ev(EV_S, VT_DESTROY, obj, 0, 0, 0);
	untrack(obj);
	FLD(obj, OBJ_VT, uint32_t) = fw->vt_ram;
	return ((vt_obj_f)prev[VT_DESTROY])(obj);
}

static int s_read(void *buf, int len, void *obj)
{
	uint32_t pos = FLD(obj, OBJ_POS, uint32_t);
	int r = vread(obj, buf, len);
	ev(EV_S, VT_READ, obj, (uint32_t)len, pos, (uint32_t)r);
	return r;
}

static int s_tell(void *obj)
{
	ev(EV_S, VT_TELL, obj, 0, 0, FLD(obj, OBJ_POS, uint32_t));
	return FLD(obj, OBJ_POS, int);
}

static int s_seek(void *obj, int pos, int whence)
{
	int size = VS_HDR + DATA_SIZE, np;

	np = whence == 1 ? FLD(obj, OBJ_POS, int) + pos : whence == 2 ? size + pos : pos;
	if (np < 0)
		np = 0;
	if (np > size)
		np = size;
	FLD(obj, OBJ_POS, int) = np;
	FLD(obj, OBJ_EOF, int) = 0;
	ev(EV_S, VT_SEEK, obj, (uint32_t)pos, (uint32_t)whence, (uint32_t)np);
	return 1;
}

static int s_getc(void *obj)
{
	ev(EV_S, VT_GETC, obj, FLD(obj, OBJ_POS, uint32_t), 0, 0);
	uint8_t c;
	return vread(obj, &c, 1) == 1 ? c : -1;
}

static int s_get(void *obj, void **ptr, int *len, int *eof)
{
	uint8_t *chunk = obs_alloc(GET_SZ);
	uint32_t pos = FLD(obj, OBJ_POS, uint32_t);
	int n;

	FLD(obj, OBJ_CHUNK, void *) = chunk;
	n = chunk ? vread(obj, chunk, GET_SZ) : 0;
	*ptr = chunk;
	*len = n;
	*eof = 0;
	FLD(obj, OBJ_EOF, int) = 0;
	if (n == 0) {
		if (chunk)
			obs_free(chunk);
		FLD(obj, OBJ_CHUNK, void *) = 0;
		*ptr = 0;
	}
	ev(EV_S, VT_GET, obj, (uint32_t)chunk, pos, (uint32_t)n);
	return 0;
}

static int s_size(void *obj)
{
	ev(EV_S, VT_SIZE, obj, 0, 0, FLD(obj, OBJ_SIZE, uint32_t));
	return FLD(obj, OBJ_SIZE, int);
}

/* the stock slots of our objects (write, release, push, notify, remaining, one): logged, then as before */
typedef int (*f4_t)(int, int, int, int);
#define THUNK(n) static int t_##n(int a, int b, int c, int d) { \
	int r = ((f4_t)prev[n])(a, b, c, d); \
	ev(EV_STOCK, n, 0, (uint32_t)a, (uint32_t)b, (uint32_t)r); \
	return r; }
THUNK(VT_WRITE) THUNK(VT_RELEASE) THUNK(VT_PUSH) THUNK(VT_NOTIFY) THUNK(VT_REMAIN) THUNK(VT_ONE)

/* ---------- global table slots ---------- */

static int g_read(void *buf, int len, void *obj)
{
	if (detect(obj))
		return s_read(buf, len, obj);
	return ((vt_rw_f)prev[VT_READ])(buf, len, obj);
}

static int g_tell(void *obj)
{
	if (detect(obj))
		return s_tell(obj);
	return ((vt_obj_f)prev[VT_TELL])(obj);
}

static int g_seek(void *obj, int pos, int whence)
{
	if (detect(obj))
		return s_seek(obj, pos, whence);
	return ((vt_seek_f)prev[VT_SEEK])(obj, pos, whence);
}

static int g_getc(void *obj)
{
	if (detect(obj))
		return s_getc(obj);
	return ((vt_obj_f)prev[VT_GETC])(obj);
}

static int g_get(void *obj, void **ptr, int *len, int *eof)
{
	if (detect(obj))
		return s_get(obj, ptr, len, eof);
	return ((vt_get_f)prev[VT_GET])(obj, ptr, len, eof);
}

static int g_size(void *obj)
{
	if (detect(obj))
		return s_size(obj);
	return ((vt_obj_f)prev[VT_SIZE])(obj);
}

static const uint32_t gslot[VT_SLOTS] = {
	[VT_READ] = (uint32_t)g_read, [VT_TELL] = (uint32_t)g_tell, [VT_SEEK] = (uint32_t)g_seek,
	[VT_GETC] = (uint32_t)g_getc, [VT_GET] = (uint32_t)g_get, [VT_SIZE] = (uint32_t)g_size,
};

int vs_install(void)
{
	unsigned i, f;

	if (installed)
		return (int)(fw - fw_list) + 1;
#ifndef HOST_TEST
	for (f = 0; f < sizeof(fw_list) / sizeof(fw_list[0]); f++) {
		const uint32_t *lib = (const uint32_t *)__sys_switab_addres;	/* what the calls use (D80) */
		fw_list[f].vt_ram = (uint32_t)RamObsFileStreamMethods();
		fw_list[f].obs_alloc = lib[0x4E5];	/* for the code check below */
		fw_list[f].obs_free = lib[0x4E6];
	}
#endif
	/* flash first: the RAM address of the other firmware may not exist on this phone */
	for (f = 0; f < sizeof(fw_list) / sizeof(fw_list[0]); f++)
		if (*(volatile uint32_t *)fw_list[f].vt_flash == fw_list[f].vt_destroy &&
		    ((volatile uint32_t *)fw_list[f].vt_ram)[VT_DESTROY] == fw_list[f].vt_destroy)
			break;
	if (f == sizeof(fw_list) / sizeof(fw_list[0]))
		return VS_FW_NONE;
	fw = &fw_list[f];
#ifdef RADIO_LOG
	for (i = 0; i < VT_SLOTS; i++)
		vs_table[i] = GT[i];
#endif
#ifndef HOST_TEST
	{	/* the code we call / chain to must be the one we know (byte-identical on S65v58 and SK65v50) */
		static const uint8_t alloc_code[12] = { 0x00, 0x10, 0xa0, 0xe1, 0x14, 0x00, 0x9f, 0xe5, 0x60, 0xff, 0xff, 0xea };
		static const uint8_t destroy_code[12] = { 0xf8, 0x45, 0x2d, 0xe9, 0x00, 0xa0, 0xb0, 0xe1, 0x2c, 0x00, 0x00, 0x0a };
		const volatile uint8_t *a = (const volatile uint8_t *)fw->obs_alloc, *d = (const volatile uint8_t *)fw->vt_destroy;
		for (i = 0; i < 12; i++)
			if (a[i] != alloc_code[i] || d[i] != destroy_code[i]) {
				vs_fail = "Unknown Obs code";
				return VS_FW_NONE;
			}
	}
	for (i = 0; i < VT_SLOTS; i++)	/* every slot: firmware flash (stock or a VKP's code) */
		if (GT[i] < 0xA0000000u || GT[i] >= 0xA2000000u) {
			vs_fail = "Obs table taken by another elf";
			return VS_FW_NONE;
		}
#endif
	for (i = 0; i < VT_SLOTS; i++)
		prev[i] = GT[i];
#ifdef RADIO_LOG
	for (i = 0; i < VT_SLOTS; i++)
		vs_table[i] = prev[i];
#endif
	for (i = 0; i < VT_SLOTS; i++)
		ours[i] = prev[i];
	ours[VT_DESTROY] = (uint32_t)s_destroy;
	ours[VT_READ] = (uint32_t)s_read;
	ours[VT_TELL] = (uint32_t)s_tell;
	ours[VT_SEEK] = (uint32_t)s_seek;
	ours[VT_GETC] = (uint32_t)s_getc;
	ours[VT_GET] = (uint32_t)s_get;
	ours[VT_SIZE] = (uint32_t)s_size;
	ours[VT_WRITE] = (uint32_t)t_VT_WRITE;
	ours[VT_RELEASE] = (uint32_t)t_VT_RELEASE;
	ours[VT_PUSH] = (uint32_t)t_VT_PUSH;
	ours[VT_NOTIFY] = (uint32_t)t_VT_NOTIFY;
	ours[VT_REMAIN] = (uint32_t)t_VT_REMAIN;
	ours[VT_ONE] = (uint32_t)t_VT_ONE;
	for (i = 0; i < VT_SLOTS; i++)
		if (gslot[i])
			GT[i] = gslot[i];
	installed = 1;
	return (int)f + 1;
}

int vs_busy(void)
{
	return 0;
}

int vs_uninstall(void)
{
	int i;

	if (!installed)
		return 1;
	for (i = 0; i < VT_SLOTS; i++)
		if (gslot[i] && GT[i] == gslot[i])
			GT[i] = prev[i];
	installed = 0;
	return 1;
}

void vs_stop(void)
{
	enabled = 0;
}

int vs_bits = 16;

void vs_start(vs_fill_f fill, void *ctx, int rate, int ch)
{
	uint32_t v;

	enabled = 0;
	fill_fn = fill;
	fill_ctx = ctx;
	VS_DIAG(ev_budget = EV_PER_START;)
	vcpy(hdr, "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0", 22);
	v = 36 + DATA_SIZE;
	vcpy(hdr + 4, &v, 4);
	hdr[22] = ch;
	hdr[23] = 0;
	v = rate;
	vcpy(hdr + 24, &v, 4);
	v = rate * ch * (vs_bits / 8);
	vcpy(hdr + 28, &v, 4);
	hdr[32] = ch * (vs_bits / 8);
	hdr[33] = 0;
	hdr[34] = vs_bits;
	hdr[35] = 0;
	vcpy(hdr + 36, "data", 4);
	v = DATA_SIZE;
	vcpy(hdr + 40, &v, 4);
	enabled = 1;
}

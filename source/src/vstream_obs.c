/*
 * EL71v45 (ELKA build, RADIO_OBS, D129): the stream played through the firmware's Obs API the way its own
 * players do, no PlayFile and no file system hook. Interface: vstream.h (vs_*) + obs_* (be_elka.c).
 *
 * An Obs object (Obs_CreateObject: uid in = GetExtUid_ws("mp3"), uid out 0x34, prio 2, as the media player
 * MediaCSM_CallObjectServer A07A6804 / SetOBSParamsForSounds A07A6308) gets a MEMORY input
 * (Obs_SetInput_Memory 27A, A0CD948C) on a small buffer of ours. Its stream object (A0D21CF0, 0x100 bytes) reaches
 * every method through its own table pointer +0xFC (wrappers A0CDFAFC..A0CDFDCC: +00 close, +04 read(dst, n, obj),
 * +0C tell, +10 seek, +14 getc, +20 direct(obj, &ptr, &len, &eof), +24 size, +2C remaining ...; the memory
 * table is the RW copy of A12E5B04 at A8F5C348). Our object gets a copy of that table with read / getc / direct /
 * close of ours; nothing else in the firmware changes. The object is found from the Obs handle the way
 * Obs_SetInput_Memory stores it (A0CDC2F4 / A0CD9D04): slot = *(obs+8)+0x64 [0] = {graph, index},
 * stream = *(*(graph+0xC) + index*0x14 + 0x20); every step range-checked, the object must be the memory type
 * (+0x68 == 1) holding our buffer (+0xC4) and length (+0xD4), else no playback (logged).
 * +0xD4 (length) is then set to DATA_SIZE: the wrappers (seek from the end, size +24, remaining +2C) see an
 * endless file; reads never end (silent frames when the ring is dry, mp3pass.c); the last MB reads zeros (tag
 * search). +0xD8 is the wrappers' position in that virtual file: reads at it are served from the history of
 * what was delivered (format probe / seek back) or fresh from the ring, as vstream_nsg.c did.
 * The methods run in the Obs task: plain stores, LockSched around the shared state.
 */
#include <swilib.h>
#include "vstream.h"
#include "app.h"
#include "fwlib.h"

#define DATA_SIZE	0x0FF00000u
#define TAIL_ZONE	0x100000u
#define HIST		65536u		/* power of 2 */
#define NCHUNK		4
#define CHUNK_MAX	8192
#define CHUNK_DEF	4096
#define MSG_RADIO_OBS	0xDEC5		/* our Obs object's messages (msg_callback) */

#define T_CLOSE		0
#define T_SEEK		4	/* D143 file input: seek(obj, off, whence) -> 1, sets +0xD8 itself (A0D21620) */
#define T_READ		1
#define T_GETC		5
#define T_DIRECT	8
#define T_N		18		/* 0x48 bytes: memory table A8F5C348 - file table A8F5C300 */

typedef int (*mfn)(int, int, int, int);

static int ram(uint32_t p)
{
	return p >= 0xA8000000u && p < 0xB0000000u && !(p & 3);
}

volatile uint32_t vs_reads;
const char *vs_fail;
#ifdef RADIO_LOG
volatile uint32_t vs_seen, vs_detects, vs_nonzero;
VS_EV vs_ev[VS_EV_N];
volatile uint32_t vs_ev_wr, vs_ev_rd, vs_ev_lost;
volatile uint16_t vs_now;
uint32_t vs_table[16];
#define TAB(i, v)	(vs_table[i] = (v))
#else
#define TAB(i, v)	((void)0)
#endif
char vs_dir[128];

static vs_fill_f fill_fn;
static void *fill_ctx;
static volatile int enabled;
static uint32_t hist_end, hist_have;
static uint8_t hist[HIST];
static uint8_t chunk[NCHUNK][CHUNK_MAX];
static int chunk_i;
static volatile int callers;

static HObj obs;			/* our Obs object, 0 = none */
static uint8_t *sobj;			/* its stream object while open (our methods in it) */
static volatile int sobj_open;
static mfn orig_t[T_N];
/* D151: the memory input buffer and our method table live in the HEAP, not in the elf: on stop the table's
 * entries become firmware functions (give_back), so an object (or a clone of it we never saw) that outlives
 * the elf reaches no elf address. Freed on ConvDestroyed; leaked when it never comes. */
#define DUMMY_LEN	417
static uint8_t *dummy;			/* the memory input given to Obs_SetInput_Memory (417 zero bytes) */
static mfn *cur_t;			/* our table of the current object, NULL = none installed */
/* D154: "return 0" taken from the object's own memory table (slot 10, A0D21CC8 on EL71v45: mov r0,#0; bx lr),
 * checked by its code words: no firmware address in the elf (was A00EBF00 / A00A3EB4 until D153) */
#define T_RET0		10
static mfn fw_ret0;
#define NCLONE		16
static uint8_t *clones[NCLONE];		/* D151: clones of our stream object seen calling us (restored on stop) */
#ifdef RADIO_LOG
static volatile uint32_t hdr_ok, hdr_bad, gap_fill, jumps, nbad;
static uint32_t bad_pos[4];
static uint8_t bad_hdr[4][4];
#endif
static uint8_t scratch[1024];
#define GAP_FILL_MAX	0x40000u	/* a read ahead of what was delivered: the bytes between are generated (as a file) */
static int stopping, destroyed, prepared, ostarted, stop_ticks;
static volatile int at_eof;
static volatile int preparing;		/* D141: Obs_Prepare .. PrepareCon: short input (the 44.1 kHz probe reads to the end) */
#define PREP_LEN	0x100000u	/* the input's length while preparing */
#define PREP_REAL	65536u		/* real stream bytes while preparing; zeros after (the ring is not eaten) */
static uint32_t *orig_tab;		/* D137: the object's own method table (given back on stop) */
#ifdef RADIO_LOG
volatile uint32_t obs_calls[T_N];	/* method calls by slot (log once a second) */
static volatile uint32_t direct_bytes, read_bytes;
#endif

static void bcopy_(uint8_t *d, const uint8_t *s, uint32_t n) { while (n--) *d++ = *s++; }
static void bzero_(uint8_t *d, uint32_t n) { while (n--) *d++ = 0; }

static void fresh(uint8_t *buf, uint32_t n)
{
	uint32_t k, off;

	if (dec)	/* D151: preparing: real frames up to PREP_REAL, then whole silent frames (aligned, ring kept) */
		dec->hold = preparing && hist_end >= PREP_REAL;
	if (enabled && fill_fn) {
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

/* n bytes at the virtual position pos (as vstream_nsg.c) */
static void vread(uint32_t pos, uint8_t *buf, uint32_t n)
{
	uint32_t k;

	/* D151: the virtual file is ONE contiguous byte stream of whole frames. D141..D150 served silent frames by
	 * position past PREP_REAL while preparing: the real frame crossing 64 KB was cut and the frames after it were
	 * off the parser's frame grid; after PrepareCon the ring went on in the middle of a frame again. The PV MP3
	 * parser (A11FBF6C) sends END OF TRACK at the first bad header (RetrieveTrackData A11990AC -> A1198ED8):
	 * 22 kHz streams never read 64 KB while preparing, 44.1 kHz ones did. Now: silent frames come from mp3pass
	 * (hold) at a frame boundary, and a read ahead of the delivered data generates the bytes between. */
	if (pos >= DATA_SIZE - TAIL_ZONE) {
		bzero_(buf, n);
		return;
	}
	if (pos > hist_end) {
		uint32_t gap = pos - hist_end;
		if (gap <= GAP_FILL_MAX) {
			while (hist_end < pos) {
				k = pos - hist_end;
				if (k > sizeof(scratch))
					k = sizeof(scratch);
				fresh(scratch, k);
			}
			VS_DIAG(gap_fill += gap;)
		} else {
			hist_end = pos;
			hist_have = 0;
			VS_DIAG(jumps++;)
		}
	}
	if (pos < hist_end) {
		if (pos < hist_end - hist_have) {
			hist_end = pos;		/* older than the history: the stream restarts here (logged) */
			hist_have = 0;
			VS_DIAG(jumps++;)
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

/* ---------- our methods (Obs task) ---------- */

/* D146: any object with our table is ours: the firmware clones stream objects (slot +34 copies all 0x100
 * bytes incl. +0xFC), each clone keeps its own position +0xD8 */
#ifdef RADIO_LOG
static volatile uint32_t clone_calls;
#endif
static int is_ours(int o)
{
	int i, fr = -1;
	if (!o || !cur_t || *(mfn **)((uint8_t *)o + 0xFC) != cur_t)
		return 0;
	if ((uint8_t *)o != sobj) {
		VS_DIAG(clone_calls++;)
		for (i = 0; i < NCLONE; i++) {
			if (clones[i] == (uint8_t *)o)
				return 1;
			if (!clones[i] && fr < 0)
				fr = i;
		}
		if (fr >= 0)
			clones[fr] = (uint8_t *)o;
	}
	return 1;
}

#ifdef RADIO_LOG
/* D151: a 4-byte read is the parser's frame header read (A11FBF6C): count valid / bad ones */
static void check_hdr(const uint8_t *b, uint32_t pos)
{
	int ok = b[0] == 0xFF && (b[1] & 0xE0) == 0xE0 && (b[2] & 0xF0) != 0xF0 && (b[2] & 0x0C) != 0x0C;
	if (ok && dec && dec->sil_len > 0)
		ok = (b[1] & 0xFE) == (dec->sil[1] & 0xFE) && (b[2] & 0x0C) == (dec->sil[2] & 0x0C);
	if (ok) {
		hdr_ok++;
		return;
	}
	if (nbad < 4) {
		bad_pos[nbad] = pos;
		bcopy_(bad_hdr[nbad], b, 4);
	}
	nbad++;
	hdr_bad++;
}
#endif

static uint32_t pos_of(int o)
{
	return *(uint32_t *)((uint8_t *)o + 0xD8);
}

/* +04 read(dst, n, obj) -> n; the wrapper (A0CDFB44) adds the result to +0xD8 */
static int m_read(int dst, int n, int o, int x)
{
	if (!is_ours(o))
		return orig_t[T_READ](dst, n, o, x);
	LockSched();
	callers++;
	VS_DIAG(obs_calls[T_READ]++;)
	if (at_eof)
		n = 0;
	if (0 && preparing && n > 0) {		/* D141 cap: off since D150 (the cap became the clip length) */
		uint32_t pos = pos_of(o);
		n = pos >= PREP_LEN ? 0 : (uint32_t)n > PREP_LEN - pos ? (int)(PREP_LEN - pos) : n;
	}
	if (n > 0) {
		vread(pos_of(o), (uint8_t *)dst, (uint32_t)n);
#ifdef RADIO_LOG
		read_bytes += (uint32_t)n;
		if (n == 4)
			check_hdr((uint8_t *)dst, pos_of(o));
#endif
	}
	callers--;
	UnlockSched();
	return n > 0 ? n : 0;
}

/* +14 getc(obj): the byte at +0xD8 (no advance, as A0D21B3C) */
static int m_getc(int o, int a, int b, int c)
{
	uint8_t v;
	if (!is_ours(o))
		return orig_t[T_GETC] ? orig_t[T_GETC](o, a, b, c) : -1;
	LockSched();
	callers++;
	VS_DIAG(obs_calls[T_GETC]++;)
	if (at_eof) {
		callers--;
		UnlockSched();
		return -1;
	}
	vread(pos_of(o), &v, 1);
	callers--;
	UnlockSched();
	return v;
}

/* +20 direct(obj, &ptr, &len, &eof): a chunk of ours at +0xD8; the wrapper (A0CDFC64) adds *len to +0xD8. The
 * memory method's whole-buffer answer (+0xE8 == 0) and end flag are never given: chunks of +0xE8 (the size the
 * decoder asked for through A0CDFDCC) or CHUNK_DEF, from 4 rotating buffers (the previous chunk stays valid). */
static int m_direct(int o, int pp, int pl, int pe)
{
	uint32_t n;
	uint8_t *b;
	if (!is_ours(o))
		return orig_t[T_DIRECT](o, pp, pl, pe);
	LockSched();
	callers++;
	VS_DIAG(obs_calls[T_DIRECT]++;)
	if (at_eof) {
		if (pp)
			*(uint8_t **)pp = chunk[chunk_i];
		if (pl)
			*(int *)pl = 0;
		if (pe)
			*(int *)pe = 1;
		callers--;
		UnlockSched();
		return 0;
	}
	n = *(uint32_t *)((uint8_t *)o + 0xE8);
	if (!n || n > CHUNK_MAX)
		n = CHUNK_DEF;
	b = chunk[chunk_i];
	chunk_i = (chunk_i + 1) % NCHUNK;
	vread(pos_of(o), b, n);
	VS_DIAG(direct_bytes += n;)
	if (pp)
		*(uint8_t **)pp = b;
	if (pl)
		*(int *)pl = (int)n;
	if (pe)
		*(int *)pe = 0;
	callers--;
	UnlockSched();
	return 0;
}

/* +00 close(obj): the firmware frees the object (A0D21A78 -> A0CDFAF0); no call of ours reaches it afterwards */
static int m_close(int o, int a, int b, int c)
{
	int r;
	if (is_ours(o)) {
		int i;
		LockSched();
		VS_DIAG(obs_calls[T_CLOSE]++;)
		if ((uint8_t *)o == sobj) {
			sobj = NULL;
			sobj_open = 0;
		}
		for (i = 0; i < NCLONE; i++)
			if (clones[i] == (uint8_t *)o)
				clones[i] = NULL;
		UnlockSched();
	}
	r = orig_t[T_CLOSE](o, a, b, c);
	return r;
}

/* D143: the input is a real 417-byte file (Obs file input: the firmware's MP3 path for files, which played
 * 320k with PlayFile; memory input failed 44.1 kHz streams with PrepareCon 121 = "download the whole clip"). */
#define OBS_DIR		"1:\\FasoleyRadio"
#define OBS_FILE	OBS_DIR "\\radiostream.mp3"

static uint32_t file_len;

/* D145: the file holds valid MP3 frames of the stream's format (silent frames from mp3pass): with zeros the
 * firmware's own probe at Obs_SetInput_File (before our methods) failed and PrepareCon said 6 at once (D144). */
static __attribute__((unused)) int make_file(void)
{
	unsigned err;
	int h, i;
	if (!isdir(OBS_DIR, &err))
		_mkdir(OBS_DIR, &err);
	h = _open(OBS_FILE, A_ReadWrite + A_BIN + A_Create + A_Truncate, P_READ + P_WRITE, &err);
	if (h < 0)
		return 0;
	file_len = 0;
	if (dec && dec->sil_len > 0) {
		for (i = 0; i < 24; i++)
			if (_write(h, dec->sil, dec->sil_len, &err) == dec->sil_len)
				file_len += (uint32_t)dec->sil_len;
	} else if (_write(h, dummy, DUMMY_LEN, &err) == DUMMY_LEN)
		file_len = DUMMY_LEN;
	_close(h, &err);
	return file_len != 0;
}

/* +10 seek(obj, off, whence): the file method seeks the file and stores +0xD8 (the wrapper A0CDFBA4 leaves
 * +0xD8 alone for file objects); ours only moves the virtual position. */
static __attribute__((unused)) int m_seek(int o, int off, int whence, int x)
{
	uint32_t p;
	if (!is_ours(o))
		return orig_t[T_SEEK](o, off, whence, x);
	LockSched();
	VS_DIAG(obs_calls[T_SEEK]++;)
	whence = (short)whence;
	p = whence == 0 ? 0 : whence == 1 ? *(uint32_t *)((uint8_t *)o + 0xD8) : *(uint32_t *)((uint8_t *)o + 0xD4);
	p += (uint32_t)off;
	*(uint32_t *)((uint8_t *)o + 0xD8) = p;
	*(uint32_t *)((uint8_t *)o + 0xE0) = 0;
	UnlockSched();
	return 1;
}

/* D137: on stop the object becomes the firmware's own memory stream again, read to its end: its own method
 * table, our 417-byte buffer (+0xC4 never changed), length and position 417. A read of 0 at EOF made the
 * decoder spin in D136 with our table; from here no firmware call reaches the elf (exit needs no close). */
#ifdef RADIO_LOG
static int given_clones;
#endif
static void give_obj(uint8_t *o)
{
	*(uint32_t *)(o + 0xD4) = file_len;
	*(uint32_t *)(o + 0xD8) = file_len;
	*(uint32_t **)(o + 0xFC) = orig_tab;
}

/* D151: every object of ours (the stream object and each clone seen) gets its own table and end of data; then
 * our heap table itself is rewritten to firmware functions (read -> 0, getc -> -1, the rest the memory
 * methods), for a clone we never saw. From here nothing in the firmware points into the elf. LockSched held. */
static void give_back(void)
{
	int i;
	if (!cur_t || !orig_tab)
		return;
	VS_DIAG(given_clones = 0;)
	if (sobj && sobj_open)
		give_obj(sobj);
	for (i = 0; i < NCLONE; i++)
		if (clones[i]) {
			if (ram((uint32_t)clones[i]) && *(mfn **)(clones[i] + 0xFC) == cur_t) {
				give_obj(clones[i]);
				VS_DIAG(given_clones++;)
			}
			clones[i] = NULL;
		}
	for (i = 0; i < T_N; i++)
		cur_t[i] = orig_t[i];
	cur_t[T_READ] = fw_ret0;	/* read -> 0 = end of data */
	cur_t[T_GETC] = fw_ret0;	/* getc -> 0 (no data anyway) */
	cur_t = NULL;
	sobj = NULL;
	sobj_open = 0;
}

/* ---------- finding our stream object (read only; checked) ---------- */

static int code(uint32_t p)
{
	return (p >= 0xA0000000u && p < 0xA2000000u) || (p >= 0xA8000000u && p < 0xB0000000u);
}

static const char *find_sobj(HObj h, uint8_t **out)
{
	uint32_t o = (uint32_t)h, p8, slots, graph, idx, ents, so, tbl;
	int i;

	if (!ram(o))
		return "obs handle not in RAM";
	p8 = *(uint32_t *)(o + 8);
	if (!ram(p8))
		return "obs+8 not in RAM";
	if (*(uint32_t *)(p8 + 8) < 1 || *(uint32_t *)(p8 + 8) > 16)
		return "obs slot count bad";
	slots = *(uint32_t *)(p8 + 0x64);
	if (!ram(slots))
		return "slot table not in RAM";
	graph = *(uint32_t *)slots;
	idx = *(uint32_t *)(slots + 4);
	if (!ram(graph) || idx > 16)
		return "slot 0 bad";
	ents = *(uint32_t *)(graph + 0xC);
	if (!ram(ents))
		return "graph entries not in RAM";
	so = *(uint32_t *)(ents + idx * 0x14 + 0x20);
	TAB(0, o);
	TAB(1, p8);
	TAB(2, slots);
	TAB(3, graph);
	TAB(4, idx);
	TAB(5, so);
	if (!ram(so))
		return "stream object not in RAM";
	TAB(7, *(uint8_t *)(so + 0x68));
	if (*(uint8_t *)(so + 0x68) != 1)
		return "stream object not memory type";
	if (*(uint32_t *)(so + 0xC4) != (uint32_t)dummy || *(uint32_t *)(so + 0xD4) != DUMMY_LEN)
		return "stream object not on our buffer";
	tbl = *(uint32_t *)(so + 0xFC);
	TAB(6, tbl);
	if (!ram(tbl))
		return "method table not in RAM";
	{
		uint32_t f = ((uint32_t *)tbl)[T_RET0];
		if (!f || (f & 3) || !code(f) || ((uint32_t *)f)[0] != 0xE3A00000u || ((uint32_t *)f)[1] != 0xE12FFF1Eu)
			return "no return-0 method in the table";
		fw_ret0 = (mfn)f;
	}
	for (i = 0; i < T_N; i++) {
		uint32_t f = ((uint32_t *)tbl)[i];
		if (f && !code(f))
			return "method table entry bad";
	}
	if (!((uint32_t *)tbl)[T_CLOSE] || !((uint32_t *)tbl)[T_READ] || !((uint32_t *)tbl)[T_DIRECT] ||
	    !((uint32_t *)tbl)[T_GETC])
		return "method table entry missing";
	TAB(7, ((uint32_t *)tbl)[T_READ]);
	*out = (uint8_t *)so;
	return 0;
}

/* ---------- events (MMI, Obs_TranslateMessageGBS A0CDE10C -> A0CDDE60: table ends at type 0x66) ---------- */

static int pending_ev;		/* OBS_PE_* for be_on_msg */
static mfn *obs_tab;		/* the heap table of obs (cur_t until given back) */

static void destroy(const char *why)
{
	int r;
	(void)why;
	if (!obs || destroyed)
		return;
	destroyed = 1;
	stop_ticks = 0;
	vs_stop();		/* D153: silent frames from the next frame boundary, no EOF */
	r = Obs_DestroyObject(obs);
	rlog("obs: destroy %X (%s) -> %d", (unsigned)obs, why, r);
	(void)r;
}

static int ev_prepare(HObj o, int st)
{
	rlog("obs ev PrepareCon obj %X status %d (calls read %u seek %u direct %u close %u, clones %u)", (unsigned)o,
	     (short)st, (unsigned)obs_calls[T_READ], (unsigned)obs_calls[T_SEEK], (unsigned)obs_calls[T_DIRECT],
	     (unsigned)obs_calls[T_CLOSE], (unsigned)clone_calls);
	if (o != obs || stopping)
		return 0;
	prepared = 1;
	LockSched();
	preparing = 0;		/* D151: real frames again from the next frame boundary (fresh -> hold 0) */
	if (dec)
		dec->hold = 0;
	UnlockSched();
	rlog("obs: prepared: headers ok %u bad %u, gap fill %u, jumps %u, delivered %u", (unsigned)hdr_ok,
	     (unsigned)hdr_bad, (unsigned)gap_fill, (unsigned)jumps, (unsigned)hist_end);
	if ((short)st > 0) {
		pending_ev = OBS_PE_ERROR;
		destroy("prepare error");	/* D143: else it stays (exit waited forever in D142) */
		return 0;
	}
	st = Obs_Start(obs);
	rlog("obs: Obs_Start -> %d", st);
	return 0;
}

static int ev_status(HObj o, int mode, int err)
{
	rlog("obs ev StatusChanged obj %X mode %d err %d", (unsigned)o, mode, (short)err);
	if (o != obs || stopping)
		return 0;
	if ((short)err > 0)
		pending_ev = OBS_PE_ERROR;
	else if (!ostarted) {
		ostarted = 1;
		pending_ev = OBS_PE_STARTED;
	}
	return 0;
}

static int ev_error(HObj o, int err)
{
	rlog("obs ev Error obj %X err %d", (unsigned)o, (short)err);
	if (o == obs && !stopping) {
		pending_ev = OBS_PE_ERROR;
		destroy("error");	/* D149: else it stays and Exit waits forever (D148) */
	}
	return 0;
}

static int ev_stopind(HObj o, int st)
{
	rlog("obs ev StopInd obj %X %d", (unsigned)o, (short)st);
	if (o != obs)
		return 0;
	if (!stopping)
		pending_ev = OBS_PE_DONE;
	destroy("stopped");	/* D149: always (a firmware stop left the object, Exit waited) */
	return 0;
}

static int ev_stopcon(HObj o, int st)
{
	rlog("obs ev StopCon obj %X %d", (unsigned)o, (short)st);
	if (o == obs)
		destroy("stop confirmed");
	return 0;
}

static int ev_destroyed(HObj o, int x)
{
	rlog("obs ev ConvDestroyed obj %X %X", (unsigned)o, (unsigned)x);
	if (o == obs) {
		LockSched();
		give_back();		/* D153: only now (the object is gone; clones too) */
		UnlockSched();
		rlog("obs: methods given back (clones %d)", given_clones);
		obs = 0;
		destroyed = 0;
		if (!cur_t) {		/* D151: given back before: nothing refers to them any more */
			if (obs_tab)
				mfree(obs_tab);
			if (dummy)
				mfree(dummy);
		}
		obs_tab = NULL;
		dummy = NULL;
	}
	return 0;
}

static int ev_other(HObj o, int ev, int payload, int x)
{
	(void)x;
	rlog("obs ev %X obj %X payload %X", ev, (unsigned)o, (unsigned)payload);
	return 0;
}

static const OBSevent obs_handlers[] = {
	{ OBS_EV_PrepareCon, (void *)ev_prepare },
	{ OBS_EV_StatusChanged, (void *)ev_status },
	{ OBS_EV_Error, (void *)ev_error },
	{ OBS_EV_StopInd, (void *)ev_stopind },
	{ OBS_EV_StopCon, (void *)ev_stopcon },
	{ OBS_EV_ConvDestroyed, (void *)ev_destroyed },
	{ OBS_EV_NO_MSG, (void *)ev_other },
	{ OBS_EV_EndList, 0 },
};

/* MMI: a message of ours -> OBS_PE_* (0 = not ours / nothing for the player) */
int obs_on_msg(GBS_MSG *msg)
{
	if ((msg->msg & 0xFFFF) != MSG_RADIO_OBS)
		return -1;
	pending_ev = 0;
	Obs_TranslateMessageGBS(msg, obs_handlers);
	return pending_ev;
}

/* ---------- player API (MMI) ---------- */

int obs_play(int vol)
{
	uint32_t err = 0;
	WSHDR *ext;
	int uid, r;
	const char *why;

	if (obs) {		/* D138: the old one until its ConvDestroyed */
		rlog("obs: previous object %X still there - not starting", (unsigned)obs);
		return -1;
	}
	if (sobj_open || cur_t) {
		rlog("obs: previous stream object still open - not starting");
		return -1;
	}
	/* D151: heap buffer + table (a forgotten object keeps its own: leaked, never freed under it) */
	dummy = malloc(DUMMY_LEN);
	obs_tab = malloc(T_N * sizeof(mfn));
	if (!dummy || !obs_tab) {
		rlog("obs: no memory for the input buffer / table");
		if (dummy)
			mfree(dummy);
		if (obs_tab)
			mfree(obs_tab);
		dummy = NULL;
		obs_tab = NULL;
		return -1;
	}
	bzero_(dummy, DUMMY_LEN);
#ifdef RADIO_LOG
	hdr_ok = hdr_bad = gap_fill = jumps = nbad = 0;
	clone_calls = 0;
#endif
	{
		int i;
		for (i = 0; i < NCLONE; i++)
			clones[i] = NULL;
	}
	obs = 0;		/* destroyed (its ConvDestroyed may come later: ignored, not ours any more) */
	ext = AllocWS(8);
	str_2ws(ext, "mp3", 7);
	uid = GetExtUid_ws(ext);
	FreeWS(ext);
	stopping = destroyed = prepared = ostarted = stop_ticks = 0;
	at_eof = 0;
	preparing = 0;
	chunk_i = 0;
	obs = Obs_CreateObject(uid, 0x34, 2, MSG_RADIO_OBS, 1, 0, &err);
	rlog("obs: Obs_CreateObject(uid %X, 34, 2, %X, 1, 0) -> %X err %X", uid, MSG_RADIO_OBS, (unsigned)obs,
	     (unsigned)err);
	if (!obs || (short)err > 0) {
		obs = 0;
		return -1;
	}
	/* D147: memory input again: with file input (D144-D146) the firmware read the file itself through the FS
	 * (0 calls into the stream object) and PrepareCon said 6 */
	file_len = DUMMY_LEN;
	r = Obs_SetInput_Memory(obs, 0, (char *)dummy, DUMMY_LEN);
	rlog("obs: Obs_SetInput_Memory(%X, %d) -> %d", (unsigned)dummy, DUMMY_LEN, r);
	if (r) {
		destroy("input refused");
		obs = 0;
		return -1;
	}
	LockSched();
	why = find_sobj(obs, &sobj);
	if (!why) {
		uint32_t *t = *(uint32_t **)(sobj + 0xFC);
		int i;
		orig_tab = t;
		for (i = 0; i < T_N; i++)
			obs_tab[i] = orig_t[i] = (mfn)t[i];
		obs_tab[T_CLOSE] = m_close;
		obs_tab[T_READ] = m_read;
		obs_tab[T_GETC] = m_getc;
		obs_tab[T_DIRECT] = (mfn)m_direct;
		hist_end = hist_have = 0;
		*(uint32_t *)(sobj + 0xD4) = DATA_SIZE;	/* D150: before Prepare: the MP3 parser reads the size once at Init (A11FBB2C) */
		preparing = 1;
		cur_t = obs_tab;
		*(mfn **)(sobj + 0xFC) = cur_t;
		sobj_open = 1;
		VS_DIAG(vs_detects++;)
	} else
		sobj = NULL;
	UnlockSched();
	rlog("obs: obj %X +8 %X slots %X graph %X idx %d stream %X table %X read %X -> %s", (unsigned)vs_table[0],
	     (unsigned)vs_table[1], (unsigned)vs_table[2], (unsigned)vs_table[3], (int)vs_table[4],
	     (unsigned)vs_table[5], (unsigned)vs_table[6], (unsigned)vs_table[7], why ? why : "ours");
	if (why) {
		vs_fail = "Obs: stream object not found";
		destroy(why);
		obs = 0;		/* our buffer is only the silent frame: nothing of ours is called */
		return -1;
	}
	{
		int mp = Obs_Mam_SetPurpose(obs, 0x16), sp = Obs_Sound_SetPurpose(obs, 0x16);
		int sv = Obs_Sound_SetVolumeEx(obs, (char)vol, 1);
		rlog("obs: Mam_SetPurpose -> %d, Sound_SetPurpose -> %d, SetVolumeEx(%d) -> %d", mp, sp, vol, sv);
		(void)mp, (void)sp, (void)sv;
	}
	r = Obs_Prepare(obs);
	rlog("obs: Obs_Prepare -> %d", r);
	if ((short)r > 0) {
		obs_stop();
		return -1;
	}
	return 1;
}

void obs_stop(void)
{
	if (!obs || stopping)
		return;
	stopping = 1;
	stop_ticks = 0;
	/* D153: no EOF and no give_back here: the object is destroyed while it still gets valid (silent) frames;
	 * D137-D152 gave EOF first and PV took ~24 s (or never) to destroy. Methods go back at ConvDestroyed. */
	/* D138: no Obs_Stop (its StopCon came 26 s late in D137 and the next object then got no events): the Java
	 * player (jnf_mediaplayer A0CFE71C / A0CFE774) closes memory-input objects with Obs_DestroyObject only.
	 * Our methods are taken off first (give_back in destroy), then the object is destroyed; a new one only
	 * after its ConvDestroyed (vs_live). */
	destroy(prepared ? "stop" : "stop before prepared");
}

void obs_volume(int v)
{
	if (obs && !stopping) {
		int r = Obs_Sound_SetVolumeEx(obs, (char)v, 1);
		rlog("volume %d: Obs_Sound_SetVolumeEx -> %d", v, r);
		(void)r;
	}
}

/* MMI tick: ConvDestroyed never came -> forget the object after 10 s (logged) */
void obs_tick(void)
{
	if (obs && destroyed && ++stop_ticks >= 100) {
		LockSched();
		give_back();		/* D153: our table -> firmware code only, safe to unload */
		UnlockSched();
		rlog("obs: no ConvDestroyed for %X in 10 s - forgotten (its buffer %X + table %X stay allocated)",
		     (unsigned)obs, (unsigned)dummy, (unsigned)obs_tab);
		obs = 0;
		destroyed = 0;
		dummy = NULL;		/* D151: leaked on purpose: the firmware object may still use them */
		obs_tab = NULL;
	}
}

#ifdef RADIO_LOG
void obs_second(void)
{
	static uint32_t l_reads, l_bytes, l_ok, l_bad, l_frames, l_nbad;
	uint32_t rd = obs_calls[T_READ], by = read_bytes + direct_bytes, ok = hdr_ok, bad = hdr_bad;
	uint32_t fr = dec ? dec->frames : 0, i;

	if (!obs && !sobj_open && !cur_t)
		return;
	/* D151: per second: reads, bytes, frame headers the parser read (ok/bad), real frames taken from the ring */
	rlog("obs: obj %X stream %s%s%s, +%u reads +%u B, hdr +%u ok +%u bad, +%u frames (gaps %u), clones %u, "
	     "gapfill %u jumps %u, pos %u, calls close %u getc %u direct %u", (unsigned)obs,
	     sobj_open ? "open" : "closed", cur_t ? " ours" : "", dec && dec->hold ? " hold" : "",
	     (unsigned)(rd - l_reads), (unsigned)(by - l_bytes), (unsigned)(ok - l_ok), (unsigned)(bad - l_bad),
	     (unsigned)(fr - l_frames), dec ? (unsigned)dec->gaps : 0u, (unsigned)clone_calls, (unsigned)gap_fill,
	     (unsigned)jumps, (unsigned)hist_end, (unsigned)obs_calls[T_CLOSE], (unsigned)obs_calls[T_GETC],
	     (unsigned)obs_calls[T_DIRECT]);
	for (i = l_nbad; i < nbad && i < 4; i++)
		rlog("obs: bad header #%u at %u: %02X %02X %02X %02X", (unsigned)i, (unsigned)bad_pos[i], bad_hdr[i][0],
		     bad_hdr[i][1], bad_hdr[i][2], bad_hdr[i][3]);
	l_nbad = nbad;
	l_reads = rd;
	l_bytes = by;
	l_ok = ok;
	l_bad = bad;
	l_frames = fr;
}
#endif

/* ---------- vs_* (vstream.h) ---------- */

int vs_install(void) { return VS_FW_EL71V45; }
int vs_uninstall(void) { return 1; }
int vs_busy(void) { return callers; }
int vs_hooked(void) { return 1; }
int vs_pump(void) { return 0; }
int vs_bits;

/* the object of ours is alive: firmware calls into our methods are possible (exit waits for 0) */
int vs_live(void)
{
	return sobj_open || cur_t || obs;	/* D138: until ConvDestroyed; D151: or our table is in use */
}

void vs_stop(void)
{
	LockSched();
	if (dec)		/* D153: valid silent frames (whole frames) instead of EOF until the object is gone */
		dec->hold = 1;
	UnlockSched();
}

void vs_start(vs_fill_f fill, void *ctx, int rate, int ch)
{
	(void)rate;
	(void)ch;
	LockSched();
	fill_fn = fill;
	fill_ctx = ctx;
	hist_end = hist_have = 0;
	enabled = 1;
	UnlockSched();
}

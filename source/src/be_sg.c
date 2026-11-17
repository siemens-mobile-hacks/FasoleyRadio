/*
 * Sound backend of FasoleyRadio.elf (S65v58 / SK65v50, SGOLD): the firmware player plays the virtual WAV
 * <elf folder>\radiostream.wav (patch 10931); the elf hooks the Obs file stream table (vstream.c, as
 * s65-pcmplayer) and decodes MP3 from the network ring with Helix in the Obs task when the player asks for
 * PCM (mp3s.c, as the s65-mp3 VKP).
 */
#include "app.h"
#include "fwlib.h"

const char be_fw_any[] = "S65v58 / SK65v50";
const int be_idle_volkeys = 0;
int be_idle_ui_on_top(void) { return 0; }
void be_idle_guard(int h, int at_start) { (void)h; (void)at_start; }
#ifdef RADIO_LOG
const int be_sync_log = 0;
#endif
const int be_vol_min = 1, be_vol_max = 5, be_vol_def = 3;
const char be_emu_ext[] = "pcm";
/* 64 KB: 4 s of 128k; the decode, not the buffer, is the limit here */
const int be_ring_kb[] = { 32, 64, 128, 256 };
const int be_nring = sizeof(be_ring_kb) / sizeof(be_ring_kb[0]), be_ring_def = 64;

/* output formats (numbers = radio.cfg "fmt" values; the setting cycles in fmt_next order: most PCM to least).
 * SK65 (D41 on the phone): with the stock DSP only 8 kHz sounds; with 0b_ v2 22.05k stereo (88 KB/s of PCM)
 * stutters while 8 kHz mono (16 KB/s) is clean. D63 on SK65: the CPU is the limit too (s65-mp3 from flash
 * wheezes at 128k+; RP 128k as 11 kHz plays), so its default is 22 kHz mono: 44.1/48k streams with the
 * synthesis at half rate (44 KB/s of PCM), 22.05k streams as поток моно. */
static const struct { const char *name; int mono, rate, bits; } fmts[] = {
	{ "as stream", 0, 0, 16 },
	{ "8kHz 16bit", 1, 8000, 16 },
	{ "8kHz 8bit", 1, 8000, 8 },
	{ "stream mono", 1, 0, 16 },
	{ "11kHz mono", 1, 11025, 16 },
	{ "22kHz mono", 1, 22050, 16 },
};
#define NFMT	((int)(sizeof(fmts) / sizeof(fmts[0])))
static const uint8_t fmt_next[NFMT] = { 3, 2, 0, 5, 1, 4 };	/* 0 -> 3 -> 5 -> 4 -> 1 -> 2 -> 0 */

static int fill(void *ctx, uint8_t *buf, int len)
{
	return mp3s_read((MP3S *)ctx, buf, len);
}

int be_install(void)
{
#ifdef RADIO_EMU
	return VS_FW_S65V58;
#else
	/* D73: Obs_MemAlloc, Obs_MemFree, RamObsFileStreamMethods (vstream.c) from the library */
	static const uint16_t need[] = { 0x4E5, 0x4E6, 0x84E7 };
	int bad;
	if (!lib_check(need, sizeof(need) / sizeof(need[0]), &bad)) {
		rlog("library: no entry %X", bad);
		vs_fail = LIB_MISSING;
		return VS_FW_NONE;
	}
	return vs_install();
#endif
}

const char *be_fw_name(void)
{
	return fw == VS_FW_SK65V50 ? "SK65v50" : "S65v58";
}

#ifdef RADIO_LOG
void be_log_install(void)
{
	int i;
	for (i = 0; i < 13; i += 4)
		rlog("table[%d..] %08X %08X %08X %08X", i, (unsigned)vs_table[i], (unsigned)vs_table[i + 1],
		     (unsigned)vs_table[i + 2], (unsigned)vs_table[i + 3]);
}

void be_log_path(char *p)
{
	sprintf(p, "%s\\radio-log.txt", elf_dir);
}
#endif

/* SK65v50 audio DSP image (A0B41FF2, see s65-mp3/release-sk65/0b_*): first word 0x0030 = replaced by the
 * 10931-patched one (0b_ v2: full-rate PCM), 0x0026 = stock (only 8 kHz plays) */
static int sk65_dsp_patched(void)
{
#ifdef RADIO_EMU
	return 0;
#else
	return *(volatile uint16_t *)0xA0B41FF2u == 0x0030;
#endif
}

#ifdef RADIO_LOG
static uint32_t os_ticks(void)
{
	return GetSessionAge();
}
#endif

/* no "fmt" in radio.cfg: SK65v50 22 kHz mono with the patched DSP, else 8 kHz 16 bit (the stock one plays
 * only 8 kHz, D30 on the phone), S65v58 as the stream */
void be_setup(void)
{
#ifdef RADIO_LOG
	dec->clock = os_ticks;		/* decode time in the per-second log */
#endif
	if (cfg.fmt < 0 || cfg.fmt >= NFMT)
		cfg.fmt = fw != VS_FW_SK65V50 ? 0 : sk65_dsp_patched() ? 5 : 1;
	rlog("output format %d (%s)%s", cfg.fmt, fmts[cfg.fmt].name,
	     fw == VS_FW_SK65V50 ? (sk65_dsp_patched() ? ", SK65 DSP 0b_ patched" : ", SK65 DSP stock") : "");
}

void be_make_stream_file(void)
{
	static const unsigned char w[44] = { 'R', 'I', 'F', 'F', 36, 0, 0, 0, 'W', 'A', 'V', 'E',
		'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0, 0x44, 0xAC, 0, 0, 0x10, 0xB1, 2, 0,
		4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0 };
	static union { FSTATS fs; char pad[64]; } u;	/* SGOLD firmware writes 40 bytes, the SDK struct is 28 */
	static char p[160];
	unsigned err;
	int h;

	sprintf(p, "%s\\%s", elf_dir, VS_NAME);
	if (GetFileStats(p, &u.fs, &err) == 0 && u.fs.size >= VS_HDR)
		return;
	h = _open(p, A_ReadWrite + A_BIN + A_Create + A_Truncate, P_READ + P_WRITE, &err);
	if (h < 0)
		return;
	_write(h, (void *)w, sizeof(w), &err);	/* never reaches the player: vstream serves the bytes */
	_close(h, &err);
}

uint32_t be_take_bps(void)
{
	return (uint32_t)dec->out_rate * dec->out_ch * (dec->out_bits == 8 ? 1 : 2);
}

uint32_t be_pcm_bps(void)
{
	return be_take_bps();
}

void be_start_stream(uint32_t resume)
{
	dec->widen = 0;	/* 1 = 44.1 kHz stereo WAV for any stream (tried for SK65 silence: not the cause) */
	dec->mono = fmts[cfg.fmt].mono;
	dec->mono_rate = fmts[cfg.fmt].rate;
	dec->mono_bits = fmts[cfg.fmt].bits;
	mp3s_start(dec, rate, chans, resume);
	vs_bits = dec->out_bits;
	vs_start(fill, dec, dec->out_rate, dec->out_ch);
}

int be_ready(void)
{
	return 1;
}

int be_play_file(const char *name, int vol)
{
	PLAYFILE_OPT opt;
	WSHDR *wfolder = AllocWS(128), *wfile = AllocWS(32);
	int h;

	str_2ws(wfolder, elf_dir, 127);
	str_2ws(wfile, name, 31);
	zeromem(&opt, sizeof(opt));
	opt.repeat_num = 1;
	opt.volume = vol;
	opt.unk = 0xA002;		/* as StandartPlay() (s65-pcmplayer, s65-player) */
	opt.time_between_play = 0x1F4;
	opt.unk5 = 1;
	h = PlayFile(0xC, wfolder, wfile, GBS_GetCurCepid(), MSG_PLAYFILE_REPORT, &opt);
	FreeWS(wfolder);
	FreeWS(wfile);
	return h;
}

void be_volume(int h, int v)
{
#ifndef RADIO_EMU
	PlayMelody_ChangeVolume(h, v);
#else
	(void)h;
	(void)v;
#endif
}

/* s65-player: silent after a restart until a volume change; a real change (v-1, v) is the sure kick */
void be_on_started(int h)
{
#ifndef RADIO_EMU
	PlayMelody_ChangeVolume(h, volume > 1 ? volume - 1 : volume + 1);
	PlayMelody_ChangeVolume(h, volume);
#else
	(void)h;
#endif
}

static void test_tick(void);

void be_tick(void)
{
	test_tick();
}

#ifdef RADIO_LOG
/* PCM served / of it silence padded / decode time per frame (this second) */
void be_second(void)
{
	static uint32_t o0, r0, f0, t0, h0[4];
	uint32_t o, r, f, tk, h[4], df, us;
	int i;

	if (!dec || pstate != P_PLAY)
		return;
	o = dec->out_bytes;
	r = dec->real_bytes;
	f = dec->frames;
	tk = dec->dec_ticks;
	df = f - f0;
	us = df ? (uint32_t)((uint64_t)(tk - t0) * OS_TICK_PERIOD_US / df) : 0;
	for (i = 0; i < 4; i++) {
		h[i] = dec->dec_hist[i] - h0[i];
		h0[i] = dec->dec_hist[i];
	}
	/* ticks: frames whose decode took 0/1/2/3+ OS ticks (~4.6 ms): CPU time vs preemption */
	rlog("pcm: served %u real %u pad %u B/s, need %u B/s, %u frames, decode %u us/frame = %u%% CPU, "
	     "ticks %u/%u/%u/%u",
	     (unsigned)(o - o0), (unsigned)(r - r0), (unsigned)((o - o0) - (r - r0)), (unsigned)be_pcm_bps(),
	     (unsigned)df, (unsigned)us, (unsigned)((uint64_t)(tk - t0) * OS_TICK_PERIOD_US / 10000),
	     (unsigned)h[0], (unsigned)h[1], (unsigned)h[2], (unsigned)h[3]);
	o0 = o;
	r0 = r;
	f0 = f;
	t0 = tk;
}
#endif

void be_close(void)
{
	be_test_stop();
}

void be_quit_poll(void)
{
}

#ifdef RADIO_LOG
const char *be_slot_name(int slot)
{
	static const char *const sn[] = { "destroy", "read", "write", "tell", "seek", "getc", "release", "push",
		"get", "size", "notify", "remain", "one" };
	return slot < (int)(sizeof(sn) / sizeof(sn[0])) ? sn[slot] : "?";
}
#endif

const char *be_msg(int what)
{
	switch (what) {
	case BM_NO_FW: return "S65v58 / SK65v50 only";
	case BM_NO_SOUND: return "No sound: not S65v58 / SK65v50";
	case BM_NOT_PLAYING: return "Player silent: needs patch 10931";
	default: return "Player does not read the stream (patch 10931?)";
	}
}

int be_nfmt(void) { return NFMT; }
const char *be_fmt_name(int f) { return f >= 0 && f < NFMT ? fmts[f].name : "?"; }
int be_fmt_next(int f) { return f >= 0 && f < NFMT ? fmt_next[f] : 0; }

void be_fmt_desc(char *b)
{
	b[0] = 0;
	if (pstate == P_PLAY && dec && (dec->out_rate != rate || dec->out_ch != chans || dec->out_bits != 16))
		sprintf(b, "%d.%dkHz%s%s", dec->out_rate / 1000, dec->out_rate % 1000 / 100, dec->out_ch == 1 ? " mono" : "",
			dec->out_bits == 8 ? " 8bit" : "");
}

/* ------------------------------------------------------------------ sound test (results in the log) */
#ifdef RADIO_LOG
/* SK65: the stream gets 2 chunks and the player never asks again. The test plays real WAV files (no hook)
 * through the same PlayFile, one format each, 3 s apart, with every report in the log: which formats does
 * the firmware (+ the 10931 port) play from PlayFile? The screen shows the test running. */
static const struct { int rate, ch, bits; } tests[] = {
	{ 8000, 1, 8 }, { 8000, 1, 16 }, { 22050, 1, 16 }, { 44100, 2, 16 },
};
#define NTEST	((int)(sizeof(tests) / sizeof(tests[0])))
#define TEST_FILE	"radiotest.wav"
#define TEST_SEC	2
static int test_on, test_i, test_ticks, test_h = -1;

/* <elf folder>\radiotest.wav: TEST_SEC s of a 500 Hz triangle tone */
static int make_test_file(int rate, int ch, int bits)
{
	static uint8_t b[2048];
	static char p[160];
	uint32_t frames = (uint32_t)rate * TEST_SEC, blk = (uint32_t)ch * bits / 8, data = frames * blk, v, f = 0;
	unsigned err;
	int h, n, period = rate / 500;

	sprintf(p, "%s\\%s", elf_dir, TEST_FILE);
	h = _open(p, A_ReadWrite + A_BIN + A_Create + A_Truncate, P_READ + P_WRITE, &err);
	if (h < 0)
		return 0;
	memcpy(b, "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0", 22);
	v = 36 + data;
	memcpy(b + 4, &v, 4);
	b[22] = ch;
	b[23] = 0;
	v = rate;
	memcpy(b + 24, &v, 4);
	v = rate * blk;
	memcpy(b + 28, &v, 4);
	b[32] = blk;
	b[33] = 0;
	b[34] = bits;
	b[35] = 0;
	memcpy(b + 36, "data", 4);
	memcpy(b + 40, &data, 4);
	_write(h, b, 44, &err);
	while (f < frames) {
		for (n = 0; n + (int)blk <= (int)sizeof(b) && f < frames; f++) {
			int ph = (int)(f % period), y = (ph < period / 2 ? ph : period - ph) * 32000 / (period / 2) - 16000, c;
			for (c = 0; c < ch; c++) {
				if (bits == 8)
					b[n++] = (uint8_t)((y >> 8) + 128);
				else {
					b[n++] = (uint8_t)y;
					b[n++] = (uint8_t)(y >> 8);
				}
			}
		}
		_write(h, b, n, &err);
	}
	_close(h, &err);
	return 1;
}

static void test_stop_one(void)
{
	if (test_h >= 0) {
		rlog("test %d: stop handle %d", test_i + 1, test_h);
		PlayMelody_StopPlayback(test_h);
	}
	test_h = -1;
}

static void test_begin(int i)
{
	test_i = i;
	test_ticks = 0;
	if (!make_test_file(tests[i].rate, tests[i].ch, tests[i].bits)) {
		rlog("test %d: cannot write %s", i + 1, TEST_FILE);
		return;
	}
	test_h = be_play_file(TEST_FILE, volume);
	rlog("test %d: %d Hz %d ch %d bit, PlayFile -> handle %d", i + 1, tests[i].rate, tests[i].ch, tests[i].bits,
	     test_h);
}

int be_has_test(void) { return 1; }
int be_test_on(void) { return test_on; }

void be_test_start(void)
{
	if (test_on)
		return;
	stop_all();
	rlog("sound test: %d real WAV files, %d s each, PlayFile as the radio (vol %d)", NTEST, TEST_SEC, volume);
	test_on = 1;
	test_begin(0);
}

void be_test_stop(void)
{
	if (!test_on)
		return;
	test_stop_one();
	test_on = 0;
}

static void test_tick(void)
{
	if (!test_on)
		return;
	if (++test_ticks < (TEST_SEC + 2) * 10)
		return;
	test_stop_one();
	if (test_i + 1 < NTEST) {
		test_begin(test_i + 1);
		return;
	}
	test_on = 0;
	rlog("sound test done");
	note = "Sound test in the log: which tests (1-4) were heard?";
}

void be_test_line(char *b)
{
	sprintf(b, "Test %d/%d: %dHz %s %dbit", test_i + 1, NTEST, tests[test_i].rate, tests[test_i].ch == 2 ? "st" : "m",
		tests[test_i].bits);
}
#else
static void test_tick(void) {}
int be_has_test(void) { return 0; }
int be_test_on(void) { return 0; }
void be_test_start(void) {}
void be_test_stop(void) {}
void be_test_line(char *b) { b[0] = 0; }
#endif

/* D123: the PlayFile reports are handled by app.c */
int be_on_msg(struct GBS_MSG *msg)
{
	(void)msg;
	return -1;
}

void be_stop_play(int h)
{
	PlayMelody_StopPlayback(h);
}

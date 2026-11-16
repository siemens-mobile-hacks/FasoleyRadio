/*
 * Sound backend of FasoleyRadio_ELKA.elf (EL71v45): the firmware plays MP3 itself. The virtual file
 * <elf folder>\radiostream.mp3 is served through the wrapped methods of its drive (vstream_nsg.c); its bytes
 * are the stream's own frames from the network ring (mp3pass.c), silent frames where there is no data.
 * Notes: RE-EL71.txt.
 */
#include "app.h"
#include "fwlib.h"

const char be_fw_any[] = "EL71v45";
const int be_sync_log = 1;
const int be_vol_min = 0, be_vol_max = 15, be_vol_def = 8;
const char be_emu_ext[] = "mp3";
/* RAM is plenty (D72): 512 KB = ~13 s of 320k, 32 s of 128k against BT hiccups */
const int be_ring_kb[] = { 64, 128, 256, 512, 1024 };
const int be_nring = sizeof(be_ring_kb) / sizeof(be_ring_kb[0]), be_ring_def = 512;

static int fill(void *ctx, uint8_t *buf, int len)
{
	return mp3s_read((MP3S *)ctx, buf, len);
}

int be_install(void)
{
#ifdef RADIO_EMU
	return VS_FW_EL71V45;
#else
	/* D73: GBS_CreateMsg, GBS_PostMsg, FS_MemAlloc, PlayMelody_GetVolume/ChangeVolume, RamFsDrivers,
	 * RamFsDrives (vstream_nsg.c, be_volume), RamPdpDrivers (net_elka.c), GetPlayObjById + Obs_Sound_SetVolumeEx
	 * (be_volume, D80) from the library */
	static const uint16_t need[] = { 0x225, 0x24E, 0x26C, 0x4E1, 0x4E2, 0x4E3, 0x4E4, 0x84E8, 0x84E9, 0x8471 };
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
	return "EL71v45";
}

void be_log_install(void)
{
	rlog("drive %X ops %X driver %X id %d handles %X fopen %X fread %X lseek %X (hooked at PlayFile)",
	     (unsigned)vs_table[0], (unsigned)vs_table[1], (unsigned)vs_table[2], (int)vs_table[3],
	     (unsigned)vs_table[4], (unsigned)vs_table[5], (unsigned)vs_table[6], (unsigned)vs_table[7]);
}

/* 0:\ (flash), never the card whose methods we wrap (D55) */
void be_log_path(char *p)
{
	strcpy(p, "0:\\radio-log.txt");
}

void be_setup(void)
{
}

/* one silent MPEG1 128k 44.1 kHz frame (417 bytes): what plays if the hook is not there. Before the hook
 * (it makes our file read-only). */
void be_make_stream_file(void)
{
	static union { FSTATS fs; char pad[64]; } u;
	static char p[160];
	static uint8_t fr[417];
	unsigned err;
	int h;

	sprintf(p, "%s\\%s", elf_dir, VS_NAME);
	if (GetFileStats(p, &u.fs, &err) == 0 && u.fs.size == 417)
		return;
	h = _open(p, A_ReadWrite + A_BIN + A_Create + A_Truncate, P_READ + P_WRITE, &err);
	if (h < 0)
		return;
	fr[0] = 0xFF;
	fr[1] = 0xFB;
	fr[2] = 0x90;
	_write(h, fr, sizeof(fr), &err);
	_close(h, &err);
}

uint32_t be_take_bps(void)
{
	return stream_bps();
}

uint32_t be_pcm_bps(void)
{
	return (uint32_t)rate * chans * 2;
}

void be_start_stream(uint32_t resume)
{
	mp3s_start(dec, rate, chans, resume);
	vs_start(fill, dec, rate, chans);
}

/* D59: every OS tick (~4.6 ms) while the drive methods are ours: post the queued answers to the firmware's
 * asynchronous calls (vstream_nsg.c) */
static GBSTMR pump_tmr;
static void pump(GBSTMR *t)
{
	(void)t;
	vs_pump();
	if (log_sync)		/* until playback is stable: each hook event to the log at once */
		log_events();
	GBS_StartTimerProc(&pump_tmr, 1, pump);
}

int be_ready(void)
{
	rlog("hook: drive methods %s (%s)", vs_hooked() ? "ours" : "NOT ours", vs_fail ? vs_fail : "ok");
	if (vs_hooked() && !IsTimerProc(&pump_tmr))
		GBS_StartTimerProc(&pump_tmr, 1, pump);
	return vs_hooked();
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
	opt.volume = vol < 1 ? 1 : vol;	/* PLAYFILE_OPT NSG: 1-15 (0 is set on the Obs at start) */
	opt.unk6 = 1;			/* as sieelf alarm_ring / MySMSYS for NEWSGOLD */
	opt.unk7 = 1;
	opt.unk9 = 2;
	h = PlayFile(0x10, wfolder, wfile, GBS_GetCurCepid(), MSG_PLAYFILE_REPORT, &opt);
	FreeWS(wfolder);
	FreeWS(wfile);
	return h;
}

/* Volume 0..15 (D80). The play object's own volume (PlayMelody_SetVolume, SWI 225, with PLAY_VOLUME as
 * PlayMelody_GetVolume 4E4 reads it - D69: a 2-argument call muted the stream) is a level 0..5 that the firmware
 * turns into the Obs volume 0,1,4,8,12,15 (SoundAE_VolumeTranslationToObs FUN_a0595cc4); the media player sets the
 * Obs volume 0..15 itself (MediaCSM_SetOBSParamsForSounds: Obs_Sound_SetVolumeEx(obs, vol, 1)). So: the nearest
 * level first (kept if the firmware sets the volume again), then the exact value on the play object's Obs
 * (GetPlayObjById 24E, +0x3D0 = its Obs handle as GetPlayObjPosition takes it; Obs_Sound_SetVolumeEx 26C). */
#ifndef RADIO_EMU
static int obs_of(int h)
{
	char *po = GetPlayObjById(h);
	return po ? *(int *)(po + 0x3D0) : 0;
}
#endif

void be_volume(int h, int v)
{
#ifndef RADIO_EMU
	static const uint8_t lvl[16] = { 0, 1, 1, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5 };
	PLAY_VOLUME pv;
	int obs;
	zeromem(&pv, sizeof(pv));
	if (!PlayMelody_GetVolume(h, &pv))
		return;
	pv.level = lvl[v & 15];
	PlayMelody_SetVolume(h, pv);
	obs = obs_of(h);
	if (obs)
		Obs_Sound_SetVolumeEx((HObj)obs, (char)v, 1);
	rlog("volume %d (level %d, obs %X)", v, pv.level, (unsigned)obs);
#else
	(void)h;
	(void)v;
#endif
}

void be_on_started(int h)
{
	be_volume(h, volume);	/* the exact Obs volume once the player runs (PlayFile gave the nearest level) */
}

/* D49 (after D30 hung the phone with the last second of the log lost): every log line is written to the file
 * at once until the stream has played 10 s; then once a second */
void be_tick(void)
{
	static int n;
	n = pstate == P_PLAY && started ? n + 1 : 0;
	log_sync = n <= 100;
}

void be_second(void)
{
}

void be_close(void)
{
	if (IsTimerProc(&pump_tmr))
		GBS_StopTimer(&pump_tmr);
	vs_pump();
	log_sync = 1;		/* D70: every exit line written at once */
}

void be_quit_poll(void)
{
	vs_pump();
}

/* drive methods (vstream_nsg.c): obj = handle; fopen a flags b async r slot/err; fread a pos b len|async<<31 r n;
 * lseek a off b origin|async<<31 r pos; fclose a pos */
const char *be_slot_name(int slot)
{
	static const char *const sn[] = { "fclose", "fstat", "fflush", "+0C", "+10", "+14", "+18", "+1C", "+20",
		"+24", "+28", "fopen", "read30", "fread", "+38", "+3C", "lseek", "+44", "setsize", "settime", "stats",
		"truncate", "+58", "+5C", "+60", "+64", "fwrite" };
	return slot < (int)(sizeof(sn) / sizeof(sn[0])) ? sn[slot] : "?";
}

const char *be_msg(int what)
{
	switch (what) {
	case BM_NO_FW: return "EL71v45 only";
	case BM_NO_SOUND: return "No sound: not EL71v45";
	case BM_NOT_PLAYING: return "Player does not play the stream";
	default: return "Player does not read the stream";
	}
}

int be_nfmt(void) { return 0; }
const char *be_fmt_name(int f) { (void)f; return ""; }
int be_fmt_next(int f) { return f; }
void be_fmt_desc(char *b) { b[0] = 0; }

int be_has_test(void) { return 0; }
void be_test_start(void) {}
void be_test_stop(void) {}
int be_test_on(void) { return 0; }
void be_test_line(char *b) { b[0] = 0; }

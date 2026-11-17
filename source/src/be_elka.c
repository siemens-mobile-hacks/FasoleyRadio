/*
 * Sound backend of FasoleyRadio_ELKA.elf (EL71v45) and FasoleyRadio_NSG.elf (S75v52, same file system and
 * player code, vstream_nsg.c): the firmware plays MP3 itself. The virtual file
 * <elf folder>\radiostream.mp3 is served through the wrapped methods of its drive (vstream_nsg.c); its bytes
 * are the stream's own frames from the network ring (mp3pass.c), silent frames where there is no data.
 * Notes: RE-EL71.txt.
 * D123: FasoleyRadio_ELKA.elf (RADIO_OBS) plays through an Obs object of its own instead (vstream_obs.c): no
 * PlayFile, no virtual file, no drive hook, no idle-handle guard. The PlayFile/drive way stays for
 * FasoleyRadio_NSG.elf and the not-built-by-default target radio_ELKA_FS (until the phone confirms Obs).
 */
#include "app.h"
#include "fwlib.h"

#define CACHE_DIR	"1:\\FasoleyRadio"

const char be_fw_any[] = "EL71v45 / S75v52";
const int be_idle_volkeys = 1;

/* D121. The idle screen's key handler (A067222E, via A05C4134) calls PlayMelody_StopPlayback(*(u16 *)(
 * GetMissedEventsPtr() + 0xA4)) on key-down/long press of almost every key (not 0x0E, 0x0F, 0x98..0x9B). That word
 * is only written with the handle of the idle's own PlaySound(0x25, ..) (A05BD122) and never cleared, so after the
 * tone it is stale; StopPlayback looks it up as a play object id (A0636918 -> GetPlayObjById A0635360), an SAE_E
 * handle (A05965EC -> A0632D4C) and an SAE_S/STI handle (A05A5604 -> A05A5574): a reused number stops our stream
 * from outside (D90/D112: AuMa asserts). 0xFFFF is rejected by all three (SAE_E and SAE_S test it first; no play
 * object has it), and is the firmware's own "no handle" (e.g. A0763BD2). So while our stream lives the word is set
 * to 0xFFFF: at once at our PlayFile start and when it equals our handle, else after it stayed the same 10 s (the
 * idle's tone is a short beep; a fresh one can still be stopped by a key meanwhile). EL71v45 only (S75's struct
 * not checked). MMI only (tick / start), as the idle code. */
void be_idle_guard(int h, int at_start)
{
#if !defined(RADIO_EMU) && !defined(RADIO_OBS)
	static uint16_t last;
	static int age;
	volatile uint16_t *p;
	char *me;
	uint16_t w;

	if (fw != VS_FW_EL71V45 || !(me = GetMissedEventsPtr()))
		return;
	p = (volatile uint16_t *)(me + 0xA4);
	w = *p;
	if (w == 0xFFFF) {
		age = 0;
		return;
	}
	if (w != last) {
		last = w;
		age = 0;
	}
	if (at_start || (h >= 0 && w == (uint16_t)h) || ++age >= 100) {
		*p = 0xFFFF;
		rlog("idle sound handle %X set to FFFF (%s)", w, at_start ? "our PlayFile" : age >= 100 ? "stale 10 s" :
		     "same as ours");
		age = 0;
	}
#else
	(void)h;
	(void)at_start;
#endif
}

int be_idle_ui_on_top(void)
{
#ifndef RADIO_EMU	/* elfloader3: not implemented (-1) */
	return IsIdleUiOnTop() == 1;	/* SWI C5 = A066ECAA, what the firmware's idle code asks */
#else
	return 0;
#endif
}
#ifdef RADIO_LOG
const int be_sync_log = 1;
#endif
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
#elif defined(RADIO_OBS)
	/* Obs_CreateObject/DestroyObject/Start/GetInfo/Stop/Prepare/TranslateMessageGBS/SetVolumeEx/Mam_SetPurpose/
	 * Sound_SetPurpose/SetInput_Memory, GetExtUid_ws, AddKeybMsgHook_end; the stream object's layout was read in
	 * EL71v45 (vstream_obs.c): its Obs_SetInput_Memory (27A) must be A0CD948C */
	static const uint16_t need[] = { 0x259, 0x25A, 0x25E, 0x267, 0x268, 0x264, 0x26C, 0x26B, 0x26E, 0x27A, 0x1D0,
		0x12C };
	int bad;
	if (!lib_check(need, sizeof(need) / sizeof(need[0]), &bad)) {
		rlog("library: no entry %X", bad);
		vs_fail = LIB_MISSING;
		return VS_FW_NONE;
	}
	if (__sys_switab_addres[0x27A] != (int)0xA0CD948Cu) {
		rlog("Obs_SetInput_Memory at %X: not EL71v45", (unsigned)__sys_switab_addres[0x27A]);
		vs_fail = "Not EL71v45";
		return VS_FW_NONE;
	}
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
	/* D92: the virtual file on the Cache volume 1: (FFS), not on the card/0: the media DB (mediadbae_process)
	 * indexes 4:\* and 0:\* (FUN_a0735018) and opened radiostream.mp3 there through our methods while a stream
	 * played (D90 phone: crash in mediadbae_process); 1: holds only firmware caches. card=1 in radio.cfg or 1: not
	 * hookable -> the elf's folder as before. */
	if (!cfg.card) {
		int f;
		strcpy(vs_dir, CACHE_DIR);
		if ((f = vs_install()) != VS_FW_NONE)
			return f;
		rlog("%s: %s - the elf's folder instead", vs_dir, vs_fail ? vs_fail : "no hook");
		vs_fail = 0;
	}
	strcpy(vs_dir, elf_dir);
	return vs_install();
#endif
}

const char *be_fw_name(void)
{
	return fw == VS_FW_S75V52 ? "S75v52" : "EL71v45";
}

#ifdef RADIO_LOG
void be_log_install(void)
{
#ifdef RADIO_OBS
	rlog("player: own Obs object, memory input with our stream methods (D130+)");
	return;
#endif
	rlog("drive %X ops %X driver %X id %d handles %X fopen %X fread %X lseek %X (hooked at PlayFile)",
	     (unsigned)vs_table[0], (unsigned)vs_table[1], (unsigned)vs_table[2], (int)vs_table[3],
	     (unsigned)vs_table[4], (unsigned)vs_table[5], (unsigned)vs_table[6], (unsigned)vs_table[7]);
}

/* 0:\ (flash), never the card whose methods we wrap (D55) */
void be_log_path(char *p)
{
	strcpy(p, "0:\\radio-log.txt");
}
#endif

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

#ifdef RADIO_OBS
	/* no file; D169: remove what older builds left (D143-D146 file input, FS builds): a .mp3 the media DB sees */
	_unlink("1:\\FasoleyRadio\\radiostream.mp3", &err);
	_rmdir("1:\\FasoleyRadio", &err);
	sprintf(p, "%s\\%s", elf_dir, VS_NAME);
	_unlink(p, &err);
	sprintf(p, "%s\\%s", CACHE_DIR, VS_NAME);
	_unlink(p, &err);
	return;
#endif
#ifndef RADIO_EMU
	if (!cfg.card) {
		sprintf(p, "%s\\%s", elf_dir, VS_NAME);	/* D92: the old copy on the card: no more media DB entry */
		_unlink(p, &err);
		if (!isdir(CACHE_DIR, &err))
			_mkdir(CACHE_DIR, &err);
		strcpy(vs_dir, CACHE_DIR);
	} else
#endif
		strcpy(vs_dir, elf_dir);
	sprintf(p, "%s\\%s", vs_dir, VS_NAME);
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
#ifdef RADIO_LOG
	if (log_sync)		/* until playback is stable: each hook event to the log at once */
		log_events();
#endif
	GBS_StartTimerProc(&pump_tmr, 1, pump);
}

int be_ready(void)
{
#ifdef RADIO_OBS
	return 1;
#endif
	rlog("hook: drive methods %s (%s)", vs_hooked() ? "ours" : "NOT ours", vs_fail ? vs_fail : "ok");
	if (vs_hooked() && !IsTimerProc(&pump_tmr))
		GBS_StartTimerProc(&pump_tmr, 1, pump);
	return vs_hooked();
}

int be_play_file(const char *name, int vol)
{
#ifdef RADIO_OBS
	(void)name;
	return obs_play(vol);
#else
	PLAYFILE_OPT opt;
	WSHDR *wfolder = AllocWS(128), *wfile = AllocWS(32);
	int h;

	str_2ws(wfolder, vs_dir, 127);
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
#endif
}

int be_on_msg(struct GBS_MSG *msg)
{
#ifdef RADIO_OBS
	return obs_on_msg(msg);
#else
	(void)msg;
	return -1;
#endif
}

void be_stop_play(int h)
{
#ifdef RADIO_OBS
	(void)h;
	obs_stop();
#else
	PlayMelody_StopPlayback(h);	/* EL71: only posts the stop to the player (A0636918) */
#endif
}

/* Volume 0..15 (D80). The play object's own volume (PlayMelody_SetVolume, SWI 225, with PLAY_VOLUME as
 * PlayMelody_GetVolume 4E4 reads it - D69: a 2-argument call muted the stream) is a level 0..5 that the firmware
 * turns into the Obs volume 0,1,4,8,12,15 (SoundAE_VolumeTranslationToObs FUN_a0595cc4); the media player sets the
 * Obs volume 0..15 itself (MediaCSM_SetOBSParamsForSounds: Obs_Sound_SetVolumeEx(obs, vol, 1)). So: the nearest
 * level first (kept if the firmware sets the volume again), then the exact value on the play object's Obs
 * (GetPlayObjById 24E, +0x3D0 = its Obs handle as GetPlayObjPosition takes it; Obs_Sound_SetVolumeEx 26C). */
#if !defined(RADIO_EMU) && !defined(RADIO_OBS)
static int obs_of(int h)
{
	char *po = GetPlayObjById(h);
	return po ? *(int *)(po + 0x3D0) : 0;
}
#endif

void be_volume(int h, int v)
{
#if defined(RADIO_OBS) && !defined(RADIO_EMU)
	(void)h;
	obs_volume(v);
#elif !defined(RADIO_EMU)
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
#ifdef RADIO_LOG
	static int n;
	n = pstate == P_PLAY && started ? n + 1 : 0;
	log_sync = n <= 100;
#endif
#if defined(RADIO_OBS) && !defined(RADIO_EMU)
	obs_tick();
#endif
}

#ifdef RADIO_LOG
void be_second(void)
{
}
#endif

void be_close(void)
{
	if (IsTimerProc(&pump_tmr))
		GBS_StopTimer(&pump_tmr);
	vs_pump();
#ifdef RADIO_LOG
	log_sync = 1;		/* D70: every exit line written at once */
#endif
}

void be_quit_poll(void)
{
	vs_pump();
}

/* drive methods (vstream_nsg.c): obj = handle; fopen a flags b async r slot/err; fread a pos b len|async<<31 r n;
 * lseek a off b origin|async<<31 r pos; fclose a pos */
#ifdef RADIO_LOG
const char *be_slot_name(int slot)
{
	static const char *const sn[] = { "fclose", "fstat", "fflush", "+0C", "+10", "+14", "+18", "+1C", "+20",
		"+24", "+28", "fopen", "read30", "fread", "+38", "+3C", "lseek", "+44", "setsize", "settime", "stats",
		"truncate", "+58", "+5C", "+60", "+64", "fwrite" };
	return slot < (int)(sizeof(sn) / sizeof(sn[0])) ? sn[slot] : "?";
}
#endif

const char *be_msg(int what)
{
	switch (what) {
	case BM_NO_FW: return "EL71v45 / S75v52 only";
	case BM_NO_SOUND: return "No sound: not EL71v45 / S75v52";
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

/*
 * Sound backend: how the firmware player gets the stream. One per elf, no #ifdef in the core:
 *   be_elka.c (FasoleyRadio_ELKA.elf, EL71v45): the firmware decodes MP3 itself from the virtual file
 *     radiostream.mp3 served through the wrapped drive methods (mp3pass.c + vstream_nsg.c).
 *   be_sg.c   (FasoleyRadio.elf, S65v58 / SK65v50): Helix decodes in the Obs task, the player gets a
 *     virtual 16-bit WAV through the hooked Obs stream table (mp3s.c + vstream.c), patch 10931.
 * Both serve the same mp3s_* / vs_* interface (mp3s.h, vstream.h) to the core (app.c).
 */
#ifndef RADIO_BACKEND_H
#define RADIO_BACKEND_H
#include <stdint.h>

/* volume range of the phone (D80): SG 1..5 (PlayMelody_ChangeVolume); EL71 0..15 = the Obs sound volume the
 * firmware's media player steps through (its PlayMelody level 0..5 maps to only 0,1,4,8,12,15) */
extern const int be_vol_min, be_vol_max, be_vol_def;
#define VOL_MAX		be_vol_max
#define VOL_MIN		be_vol_min

extern const char be_fw_any[];		/* "EL71v45" / "S65v58 / SK65v50": the phones it plays on */
extern const char be_emu_ext[];		/* emulator: what the taking timer writes (mp3 / pcm) */
extern const int be_ring_kb[];		/* buffer sizes offered in the settings, KB */
extern const int be_nring, be_ring_def;	/* how many; the default one (KB) */
extern const int be_sync_log;		/* every log line to the file at once while starting/stopping (ELKA, D49) */

int be_install(void);			/* hook the player's path; VS_FW_* (emulator: the phone it plays) */
const char *be_fw_name(void);		/* the phone found */
void be_log_install(void);		/* what the hook found, to the log */
void be_log_path(char *p);		/* where the log file goes */
void be_setup(void);			/* the decoder exists: per-phone defaults (SG: output format) */
void be_make_stream_file(void);		/* the real file behind the virtual one */
uint32_t be_take_bps(void);		/* bytes/s the player takes (WAV PCM / the MP3 itself) */
uint32_t be_pcm_bps(void);		/* bytes/s of decoded PCM (gap seconds) */
void be_start_stream(uint32_t resume_bytes);	/* mp3s_start + vs_start for the format found */
int be_ready(void);			/* the hook is in place right now (ELKA: drive methods) */
int be_play_file(const char *name, int vol);	/* PlayFile on <elf folder>\name, handle or < 0 */
void be_volume(int h, int vol);		/* playing handle: volume VOL_MIN..VOL_MAX */
void be_on_started(int h);		/* the player reported STARTED */
void be_tick(void);			/* MMI, every 0.1 s */
void be_second(void);			/* MMI, once a second (per-second log) */
void be_close(void);			/* CSM closes: stop own timers */
void be_quit_poll(void);		/* MMI while waiting for the firmware to let go (exit) */
const char *be_slot_name(int slot);	/* hook event slot -> name, for the log */
const char *be_msg(int what);		/* user texts below (cp1251) */
enum { BM_NO_FW, BM_NO_SOUND, BM_NOT_PLAYING, BM_NOT_READING };

/* output format (SG; ELKA: none, be_nfmt() == 0) */
int be_nfmt(void);
const char *be_fmt_name(int f);
int be_fmt_next(int f);			/* order the setting cycles in */
void be_fmt_desc(char *b);		/* what the player gets now ("22кГц моно"), "" = the stream itself */

/* sound test (SG: real WAV files through PlayFile, results in the log; ELKA: none) */
int be_has_test(void);
void be_test_start(void);
void be_test_stop(void);
int be_test_on(void);
void be_test_line(char *b);		/* the screen's state line while the test runs */

#endif

/*
 * Fasoley Radio core: what app.c (player state machine, CSM, log), cfg.c (radio.cfg, stations.txt),
 * screen.c (own main screen) and menus.c (native menus) share. The phone-specific sound path is behind
 * backend.h.
 */
#ifndef RADIO_APP_H
#define RADIO_APP_H
#include <swilib.h>
#include <stdint.h>
#include "ring.h"
#include "mp3s.h"
#include "radio.h"
#include "vstream.h"
#include "backend.h"

#define APP_NAME	"Fasoley Radio"

typedef struct { CSM_RAM csm; int gui_id; } MAIN_CSM;

enum { P_IDLE, P_WAIT_OLD, P_BUFFER, P_PLAY, P_CALL };

/* ------------------------------------------------------------------ stations, radio.cfg (cfg.c) */
#define MAX_ST		32
#define ST_NAME		48
#define ST_URL		200
typedef struct { char name[ST_NAME], url[ST_URL]; } STATION;
extern STATION stations[MAX_ST];
extern int nst;

typedef struct {
	int vol;		/* volume at start, VOL_MIN..VOL_MAX */
	int ring_kb;		/* network buffer */
	int start_sec;		/* buffered before the player starts */
	int resume_sec;		/* buffered again after running dry */
	int retry;		/* reconnect by itself when the stream breaks */
	int log;		/* radio-log.txt */
	int def;		/* default station (shown at start), -1 = the first */
	int autoplay;		/* play the default station at start */
	int fmt;		/* SG output format (be_fmt_name) */
} CFG;
extern CFG cfg;
void cfg_load(void);
void cfg_save(void);
void stations_load(void);
int stations_save(void);		/* 0 = could not write */
int station_set(int i, const char *name, const char *url);	/* i == nst: add; 0 = full */
void station_del(int i);

/* ------------------------------------------------------------------ log (app.c) */
extern char log_line[400];
extern int log_n, log_sync;
void log_head(void);
void log_commit(int n);
void log_flush(void);
void log_apply(void);			/* cfg.log changed */
void log_events(void);			/* the hook's events since the last call */
#define rlog(...) do { log_head(); log_commit(sprintf(log_line + log_n, __VA_ARGS__)); } while (0)

/* ------------------------------------------------------------------ player (app.c) */
extern char elf_dir[128];
extern MAIN_CSM *main_csm;
extern MP3S *dec;
extern int fw, pstate, station, handle, started, volume, kbps, rate, chans;
extern const char *note;		/* last problem (cp1251), shown when stopped */
extern uint32_t rx_bps;			/* network bytes/s (smoothed) */
extern int rx_secs;
extern uint32_t start_need;		/* bytes the player starts at (0 = not known yet) */
extern int app_started;			/* past the start (net ring, decoder, hook) */
int open_pending(void);			/* the connection to the new station is not opened yet */

void play_station(int st);
void stop_all(void);
void set_volume(int v);
void minimize(void);
void app_exit(void);
void apply_retry(void);			/* cfg.retry changed */
uint32_t stream_bps(void);		/* MP3 bytes/s of the stream (128k when not known) */

/* ------------------------------------------------------------------ screens */
int screen_create(MAIN_CSM *csm);	/* the main screen (screen.c), GUI id */
void screen_tick(void);			/* MMI, every 0.1 s: redraw when something shown changed */
void screen_redraw(void);		/* at once (after a key) */
void screen_free(void);			/* the CSM closes */
extern int screen_st;			/* station shown (chosen with left/right) */
void menu_open(void);			/* native menus (menus.c), from a key handler of ours only */
void stations_open(void);
void menus_close(void);
void menus_sync(void);			/* MMI tick: rows changed (the playing station) */

/* UTF-8 (station names typed in, most ICY titles) or 8-bit (cp1251) text -> ws (appended) */
void text_ws(WSHDR *ws, const char *s);
/* ws -> UTF-8, at most n-1 bytes */
void ws_utf8(const WSHDR *ws, char *out, int n);

#endif

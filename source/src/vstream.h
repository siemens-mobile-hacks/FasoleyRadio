/*
 * Virtual WAV stream (copied from s65-pcmplayer/src/port/vstream.c): the firmware player (with patch
 * 10931) plays the file VS_NAME; its contents = 44-byte WAV header + PCM that the fill callback gives,
 * called in the firmware's Obs task. Hook = the global Obs file stream table in RAM (S65v58 A87E4D64,
 * SK65v50 A8FE4DBC; the same slots as the s65-mp3 VKP); other files go to whatever was in the table
 * before (s65-mp3 or stock).
 */
#ifndef RADIO_VSTREAM_H
#define RADIO_VSTREAM_H
#include <stdint.h>

#ifdef RADIO_PASS
/* EL71v45 (vstream_nsg.c): <elf folder>\VS_NAME, served through the file system methods of its drive */
#define VS_NAME		"radiostream.mp3"
int vs_hooked(void);		/* the drive methods are ours (swapped in by vs_start, D49) */
int vs_pump(void);		/* MMI: post the queued answers to asynchronous calls (D59) */
#else
#define VS_NAME		"radiostream.wav"	/* matched at the end of the Obs path, any case */
#endif
#define VS_HDR		44

typedef int (*vs_fill_f)(void *ctx, uint8_t *buf, int len);	/* must give len bytes */

enum { VS_FW_NONE, VS_FW_S65V58, VS_FW_SK65V50, VS_FW_EL71V45 };

int vs_install(void);		/* VS_FW_* of the phone; VS_FW_NONE = not possible */
int vs_uninstall(void);		/* only when vs_live() == 0; 0 = cannot (someone hooked over us): stay loaded */
int vs_busy(void);		/* firmware callers inside our methods now */
extern int vs_bits;	/* SG: bits of the next WAV header (16, or 8 = unsigned) */
void vs_start(vs_fill_f fill, void *ctx, int rate, int ch);	/* format of the next stream */
void vs_stop(void);		/* reads give silence */
int vs_live(void);		/* firmware stream objects of VS_NAME still alive */
extern volatile uint32_t vs_reads;	/* calls that delivered PCM (activity) */
/* diagnostics (menu row "Диаг"): calls through the hooked global slots, objects taken as ours */
extern volatile uint32_t vs_seen, vs_detects;
extern volatile uint32_t vs_nonzero;	/* reads whose data was not all zero (real sound reached the player) */
extern const char *vs_fail;		/* why vs_install gave VS_FW_NONE on a known phone (cp1251), or 0 */

/* event log of the hook: written in the firmware's tasks (no calls, plain stores), taken and written to the log
 * file by the MMI once a second (main.c). Full ring = events dropped and counted, never grows. */
enum { EV_G, EV_S, EV_TAKEN, EV_OTHER, EV_TRACKFAIL, EV_STOCK };
typedef struct {
	uint8_t kind, slot;
	uint16_t t;			/* vs_now when it happened (0.1 s) */
	uint32_t obj, a, b, r;
} VS_EV;
#define VS_EV_N		256		/* 5 KB */
extern VS_EV vs_ev[VS_EV_N];
extern volatile uint32_t vs_ev_wr, vs_ev_rd, vs_ev_lost;
extern volatile uint16_t vs_now;	/* set by the MMI tick */
extern uint32_t vs_table[16];		/* the global table as found at install */

#endif

/* MP3 bytes from the network ring -> 16-bit PCM on demand (Helix). mp3s_read runs in the firmware's
 * Obs task (called from the virtual WAV stream, vstream.c), as the s65-mp3 VKP decodes: no PCM ring,
 * the network ring is the buffer. No firmware calls.
 * RADIO_PASS (ELKA build, mp3pass.c): the same interface, but mp3s_read gives whole MP3 frames from the
 * ring (the firmware decodes them itself) and a silent frame of the same format where there is no data. */
#ifndef RADIO_MP3S_H
#define RADIO_MP3S_H
#include <stdint.h>
#include "ring.h"

#ifdef RADIO_PASS
#define MP3_MAXFRAME	1448	/* MPEG1 320 kbps 32 kHz + padding */

typedef struct {
	RING *net;
	int rate, ch;			/* format of the stream started (frames of another one are dropped) */
	volatile int running;		/* 0 = silent frames, ring untouched */
	volatile int waiting;		/* ran dry: silent frames until net holds resume_at bytes */
	uint32_t resume_at;
	volatile uint32_t gaps;		/* times the ring ran dry while playing */
	volatile uint32_t gap_bytes;	/* PCM bytes (decoded size) of the silent frames served while playing */
	volatile uint32_t frames, errors;	/* frames passed; bytes skipped to find a frame */
	volatile uint32_t out_bytes;	/* MP3 bytes served */
	volatile int fmt_changed;	/* a frame with another rate/channels came */
	volatile int bitrate;		/* kbps of the last frame */
	int fpos, flen;			/* frame being served: fr[fpos .. flen) */
	int sil_len, sil_pcm;		/* silent frame: length, its PCM bytes */
	int xing;			/* D140: a Xing frame goes first (frame count: no duration scan on prepare) */
	volatile int hold;		/* D151: silent frames from the next frame boundary on, ring untouched (Obs prepare) */
	uint8_t fr[MP3_MAXFRAME];
	uint8_t sil[MP3_MAXFRAME];
} MP3S;
#else
#include "mp3dec.h"

#define MP3S_IN		4096

typedef struct {
	RING *net;
	HMP3Decoder dec;
	int rate, ch;			/* output format (= the WAV header) */
	volatile int running;		/* 0 = silence, decoder untouched */
	volatile int waiting;		/* ran dry: silence until net holds resume_at bytes */
	uint32_t resume_at;
	volatile uint32_t gaps;		/* times the ring ran dry while playing */
	volatile uint32_t gap_bytes;	/* PCM bytes of silence served while playing */
	volatile uint32_t frames, errors;
	volatile uint32_t out_bytes;	/* PCM bytes served */
	volatile uint32_t real_bytes;	/* of them decoded audio (the rest: silence) */
	volatile uint32_t dec_ticks;	/* OS ticks spent in MP3Decode (clock set: sampled, unbiased over many frames) */
	volatile uint32_t dec_hist[4];	/* frames by OS ticks their MP3Decode took: 0, 1, 2, 3+ (CPU-bound decode of
					 * a few ms = mostly 0-1; preemption = some long ones; slow memory = all long) */
	uint32_t (*clock)(void);	/* OS tick counter (GetSessionAge), 0 = no timing */
	volatile int fmt_changed;	/* a frame with another rate/channels came */
	volatile int bitrate;		/* kbps of the last frame */
	int widen;			/* mp3s_start: output 2x rate (< 32 kHz) and stereo (SK65: the format s65-mp3
					 * plays there; 22.05 kHz mono WAV was silent on the phone) */
	int mono, mono_rate, mono_bits;	/* mp3s_start: mono = 1 -> mono PCM at mono_rate (0 = the stream's rate;
					 * never above it), mono_bits 16 or 8 (unsigned): less PCM for weak players
					 * (SK65: 22.05k stereo stutters, 8 kHz is the stock DSP format) */
	int out_rate, out_ch, out_bits;	/* format of the PCM given (= the WAV header) */
	int dec_rate, dec_ch;		/* format of Helix's output: mono -> MP3SetOutput gives 1 channel (stereo
					 * averaged before the IMDCT/synthesis) at rate >> decim (synthesis decimated
					 * for 11/8 kHz), the box decimation does the rest */
	int d_acc, d_err;		/* resampling to out_rate (down): previous sample, phase */
	short last[2];			/* last output sample per channel (2x upsampling) */
	int in_pos, in_len, pcm_len, pcm_off;	/* pcm_len/pcm_off in bytes */
	uint8_t in[MP3S_IN];
	short pcm[1152 * 2];
} MP3S;
#endif

int mp3s_init(MP3S *s, RING *net);	/* 0 = no memory for the decoder */
void mp3s_free(MP3S *s);
/* new stream (no reader active): format of the stream, bytes to wait for after running dry.
 * Helix build: s->widen (set before) -> PCM at out_rate/out_ch (= the WAV header) */
void mp3s_start(MP3S *s, int rate, int ch, uint32_t resume_at);
void mp3s_stop(MP3S *s);		/* reads give silence from now on */
/* fill out with len bytes of PCM (silence where there is no data); returns len. Decodes at most the frames len
 * needs + 2 (never hogs the Obs task; the rest is silence).
 * RADIO_PASS: len bytes of MP3 frames (silent frames where there is no data) */
int mp3s_read(MP3S *s, uint8_t *out, int len);

/* find the stream format in the ring without taking bytes: two valid layer 3 headers in a row.
 * Returns 1 and rate/ch/kbps when found. */
int mp3s_probe(const RING *r, int *rate, int *ch, int *kbps);
/* frame length from a 4-byte header, 0 = not a layer 3 header */
int mp3_frame_len(const uint8_t *h, int *rate, int *ch, int *kbps);

#endif

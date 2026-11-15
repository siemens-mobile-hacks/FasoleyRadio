/*
 * MP3 pass-through over the network ring (ELKA build, RADIO_PASS; interface in mp3s.h). The EL71 firmware
 * decodes MP3 itself, so the virtual file (vstream_nsg.c) gets the stream's own frames: only whole frames
 * of the started format, and where the ring has none (network gap) a silent frame of that format
 * (header + zero side info = main_data_begin 0, no granule data -> the decoder outputs zeros). The firmware
 * decoder never sees a broken frame or a format change; after a gap the first real frame may point into the
 * bit reservoir of frames it did not get (one frame glitch, as after any seek). No firmware calls.
 */
#include "mp3s.h"

static void bzero_(void *d, uint32_t n)
{
	uint8_t *p = d;
	while (n--)
		*p++ = 0;
}

int mp3s_init(MP3S *s, RING *net)
{
	bzero_(s, sizeof(*s));
	s->net = net;
	return 1;
}

void mp3s_free(MP3S *s)
{
	s->running = 0;
}

/* samples per frame: MPEG1 1152, MPEG2/2.5 576 */
static int frame_samples(const uint8_t *h)
{
	return ((h[1] >> 3) & 3) == 3 ? 1152 : 576;
}

/* silent frame from a real header of the stream: no CRC, no padding, the same rate/mode/bitrate */
static void make_silent(MP3S *s, const uint8_t *h)
{
	int len, rate, ch;

	bzero_(s->sil, sizeof(s->sil));
	s->sil[0] = 0xFF;
	s->sil[1] = h[1] | 1;		/* protection bit 1 = no CRC */
	s->sil[2] = h[2] & ~2;		/* no padding */
	s->sil[3] = h[3] & 0xC0;	/* channel mode; no mode extension, no copyright/emphasis */
	len = mp3_frame_len(s->sil, &rate, &ch, 0);
	if (len <= 4 || len > MP3_MAXFRAME) {
		s->sil_len = 0;
		return;
	}
	s->sil_len = len;
	s->sil_pcm = frame_samples(s->sil) * ch * 2;
}

/* first frame header of the format in the ring (the caller found two with mp3s_probe) */
static void silent_from_ring(MP3S *s)
{
	uint32_t used = ring_used(s->net), off;
	uint8_t h[4];
	int r, c;

	s->sil_len = 0;
	for (off = 0; off + 4 <= used && off < 64 * 1024; off++) {
		if (ring_peek(s->net, off, h, 4) != 4)
			break;
		if (mp3_frame_len(h, &r, &c, 0) > 0 && r == s->rate && c == s->ch) {
			make_silent(s, h);
			return;
		}
	}
}

void mp3s_start(MP3S *s, int rate, int ch, uint32_t resume_at)
{
	s->running = 0;
	s->rate = rate;
	s->ch = ch;
	s->resume_at = resume_at;
	s->waiting = 0;
	s->gaps = s->gap_bytes = s->frames = s->errors = s->out_bytes = 0;
	s->fmt_changed = 0;
	s->fpos = s->flen = 0;
	silent_from_ring(s);
	s->running = 1;
}

void mp3s_stop(MP3S *s)
{
	s->running = 0;
}

/* next real frame of the started format into fr[]: 1 = got one, 0 = the ring has no whole frame */
static int take_frame(MP3S *s)
{
	uint8_t h[4], h2[4];
	int len, r, c, k, r2, c2, skipped = 0;
	uint32_t used;

	for (;;) {
		used = ring_used(s->net);
		if (used < 4)
			return 0;
		ring_peek(s->net, 0, h, 4);
		len = mp3_frame_len(h, &r, &c, &k);
		if (len <= 4 || len > MP3_MAXFRAME) {
			s->net->rd++;		/* not a header: resync byte by byte */
			s->errors++;
			if (++skipped >= 8192)
				return 0;	/* junk: a silent frame now, more on the next read */
			continue;
		}
		if (used < (uint32_t)len)
			return 0;
		/* a false sync inside frame data: the next header must follow (when it is already here) */
		if (used >= (uint32_t)len + 4) {
			ring_peek(s->net, (uint32_t)len, h2, 4);
			if (mp3_frame_len(h2, &r2, &c2, 0) <= 0 || r2 != r || c2 != c) {
				s->net->rd++;
				s->errors++;
				if (++skipped >= 8192)
					return 0;
				continue;
			}
		}
		if (r != s->rate || c != s->ch) {	/* another format: the firmware decoder was set up for ours */
			s->fmt_changed = 1;
			s->net->rd += (uint32_t)len;
			continue;
		}
		ring_read(s->net, s->fr, (uint32_t)len);
		s->flen = len;
		s->fpos = 0;
		s->frames++;
		s->bitrate = k;
		if (!s->sil_len)
			make_silent(s, s->fr);
		return 1;
	}
}

static void next_frame(MP3S *s)
{
	if (s->running) {
		if (s->waiting && ring_used(s->net) >= s->resume_at)
			s->waiting = 0;
		if (!s->waiting) {
			if (take_frame(s))
				return;
			s->waiting = 1;		/* ran dry */
			s->gaps++;
		}
		s->gap_bytes += (uint32_t)s->sil_pcm;
	}
	if (s->sil_len) {
		ring_cpy(s->fr, s->sil, (uint32_t)s->sil_len);
		s->flen = s->sil_len;
	} else {			/* no format known (not started): zeros */
		bzero_(s->fr, 1024);
		s->flen = 1024;
	}
	s->fpos = 0;
}

int mp3s_read(MP3S *s, uint8_t *out, int len)
{
	int done = 0, k;

	while (done < len) {
		if (s->fpos >= s->flen)
			next_frame(s);
		k = s->flen - s->fpos;
		if (k > len - done)
			k = len - done;
		ring_cpy(out + done, s->fr + s->fpos, (uint32_t)k);
		s->fpos += k;
		done += k;
	}
	s->out_bytes += (uint32_t)len;
	return len;
}

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

/* CRC-16 of an MPEG audio frame (poly 0x8005, init 0xFFFF) over bytes 2-3 and the side info */
static uint16_t crc16_bits(uint16_t crc, const uint8_t *p, int n)
{
	while (n--) {
		int i;
		crc ^= (uint16_t)(*p++ << 8);
		for (i = 0; i < 8; i++)
			crc = crc & 0x8000 ? (uint16_t)((crc << 1) ^ 0x8005) : (uint16_t)(crc << 1);
	}
	return crc;
}

/* silent frame from a real header of the stream: the same header (D150: CRC bit, mode extension, copyright,
 * original, emphasis kept, so the firmware's parser sees one format; D149 and before cleared them), no
 * padding; zero side info (+ its CRC when the stream has CRCs) */
static void make_silent(MP3S *s, const uint8_t *h)
{
	int len, rate, ch, si;

	bzero_(s->sil, sizeof(s->sil));
	s->sil[0] = 0xFF;
	s->sil[1] = h[1];
	s->sil[2] = h[2] & ~2;		/* no padding */
	s->sil[3] = h[3];
	len = mp3_frame_len(s->sil, &rate, &ch, 0);
	if (len <= 4 || len > MP3_MAXFRAME) {
		s->sil_len = 0;
		return;
	}
	if (!(h[1] & 1)) {		/* protected: CRC over header bytes 2-3 + side info, at bytes 4-5 */
		uint16_t crc;
		if (((h[1] >> 3) & 3) == 3)
			si = (h[3] >> 6) == 3 ? 17 : 32;
		else
			si = (h[3] >> 6) == 3 ? 9 : 17;
		crc = crc16_bits(0xFFFF, s->sil + 2, 2);
		crc = crc16_bits(crc, s->sil + 6, si);
		s->sil[4] = (uint8_t)(crc >> 8);
		s->sil[5] = (uint8_t)crc;
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
	s->hold = 0;
	s->xing = 0;	/* D150: no Xing frame (the parser treats a stream without it as CBR, RE-EL71.txt) */
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

static void be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* D140: the first frame is a silent frame carrying a "Xing" tag (frames + bytes of a 255 MB stream), as LAME
 * writes: in D139 the firmware's MP3 parser (PacketVideo, "Xing" at A11FBE58) walked the whole endless input
 * on Obs_Prepare for 44.1 kHz streams (no PrepareCon, ~2 MB/s reads). Side info is zero: decodes to silence. */
static int xing_frame(MP3S *s)
{
	int off;
	if (!s->sil_len || s->sil_len < 4 + 32 + 16)
		return 0;
	ring_cpy(s->fr, s->sil, (uint32_t)s->sil_len);
	if (((s->sil[1] >> 3) & 3) == 3)		/* MPEG1 */
		off = (s->sil[3] >> 6) == 3 ? 4 + 17 : 4 + 32;
	else
		off = (s->sil[3] >> 6) == 3 ? 4 + 9 : 4 + 17;
	s->fr[off] = 'X';
	s->fr[off + 1] = 'i';
	s->fr[off + 2] = 'n';
	s->fr[off + 3] = 'g';
	be32(s->fr + off + 4, 3);			/* frames + bytes present */
	be32(s->fr + off + 8, 0x0FF00000u / (uint32_t)s->sil_len);
	be32(s->fr + off + 12, 0x0FF00000u);
	s->flen = s->sil_len;
	s->fpos = 0;
	return 1;
}

static void next_frame(MP3S *s)
{
	if (s->xing && s->running) {
		s->xing = 0;
		if (xing_frame(s))
			return;
	}
	if (s->running && !s->hold) {		/* D151: hold = silent frames, the current frame was finished */
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

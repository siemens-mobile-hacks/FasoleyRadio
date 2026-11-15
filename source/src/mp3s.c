/* MP3 stream decoder over the network ring (see mp3s.h). */
#include "mp3s.h"

static void bzero_(void *d, uint32_t n)
{
	uint8_t *p = d;
	while (n--)
		*p++ = 0;
}

static void bmove(uint8_t *d, const uint8_t *s, int n)
{
	int i;
	for (i = 0; i < n; i++)		/* d < s only */
		d[i] = s[i];
}

int mp3s_init(MP3S *s, RING *net)
{
	bzero_(s, sizeof(*s));
	s->net = net;
	s->dec = MP3InitDecoder();
	return s->dec != 0;
}

void mp3s_free(MP3S *s)
{
	s->running = 0;
	if (s->dec)
		MP3FreeDecoder(s->dec);
	s->dec = 0;
}

void mp3s_start(MP3S *s, int rate, int ch, uint32_t resume_at)
{
	s->running = 0;
	if (s->dec) {			/* fresh decoder state (bit reservoir of the old stream) */
		MP3FreeDecoder(s->dec);
		s->dec = MP3InitDecoder();
	}
	s->rate = rate;
	s->ch = ch;
	s->out_rate = s->widen && rate < 32000 ? rate * 2 : rate;
	s->out_ch = s->widen ? 2 : ch;
	s->out_bits = 16;
	if (s->mono) {
		s->out_rate = s->mono_rate > 0 && s->mono_rate < rate ? s->mono_rate : rate;
		s->out_ch = 1;
		s->out_bits = s->mono_bits == 8 ? 8 : 16;
	}
	s->dec_rate = rate;
	s->dec_ch = ch;
	if (s->mono && s->dec) {
		/* cheap mono (MP3SetOutput): one IMDCT/synthesis instead of two; for a lower rate the synthesis
		 * computes only every 2nd/4th sample (rate >> d >= out_rate) and the spectrum is cut at the output's
		 * Nyquist (nsub subbands of rate/64 Hz each) */
		int d = 0, nsub;
		while (d < 2 && (rate >> (d + 1)) >= s->out_rate)
			d++;
		nsub = (s->out_rate * 64 / rate + 1) / 2;
		if (nsub > (32 >> d))
			nsub = 32 >> d;
		if (nsub < 1)
			nsub = 1;
		MP3SetOutput(s->dec, 1, d, nsub);
		s->dec_rate = rate >> d;
		s->dec_ch = 1;
	}
	s->d_acc = s->d_err = 0;
	s->last[0] = s->last[1] = 0;
	s->resume_at = resume_at;
	s->waiting = 0;
	s->gaps = s->gap_bytes = s->frames = s->errors = s->out_bytes = s->real_bytes = s->dec_ticks = 0;
	s->fmt_changed = 0;
	s->in_len = s->in_pos = s->pcm_len = s->pcm_off = 0;
	s->running = s->dec != 0;
}

void mp3s_stop(MP3S *s)
{
	s->running = 0;
}

/* bytes in[in_pos .. in_pos + in_len) are not decoded yet */
#define IN(s)	((s)->in + (s)->in_pos)

static void refill(MP3S *s)
{
	if (s->in_len >= MP3S_IN / 2 || !ring_used(s->net))
		return;
	if (s->in_pos) {
		bmove(s->in, s->in + s->in_pos, s->in_len);
		s->in_pos = 0;
	}
	s->in_len += (int)ring_read(s->net, s->in + s->in_len, (uint32_t)(MP3S_IN - s->in_len));
}

static void drop(MP3S *s, int n)
{
	if (n >= s->in_len) {
		s->in_len = 0;
		s->in_pos = 0;
		return;
	}
	s->in_pos += n;
	s->in_len -= n;
}

/* PCM of one frame (n samples, all channels) -> out_rate/out_ch in place (backwards: the output is bigger).
 * 22.05k mono 576 -> 2304, 22.05k stereo 1152 -> 2304, 44.1k mono 1152 -> 2304: always fits pcm[]. */
static int widen(MP3S *s, int n)
{
	int fr = n / s->ch, up = s->out_rate / s->rate, i, c;
	short *p = s->pcm;

	if (s->out_rate == s->rate && s->out_ch == s->ch)
		return n;
	if (fr * up * s->out_ch > 1152 * 2)
		return n;			/* cannot happen (see above) */
	{
		int r1 = s->ch == 2 ? 1 : 0;	/* input index of the right channel */
		short l0 = p[(fr - 1) * s->ch], l1 = p[(fr - 1) * s->ch + r1];
		for (i = fr - 1; i >= 0; i--) {
			short x[2], pv[2];
			for (c = 0; c < 2; c++) {	/* all inputs of this step before any write */
				x[c] = p[i * s->ch + (c ? r1 : 0)];
				pv[c] = i ? p[(i - 1) * s->ch + (c ? r1 : 0)] : s->last[c];
			}
			for (c = 0; c < 2; c++) {
				if (up == 2) {
					p[(2 * i + 1) * 2 + c] = x[c];
					p[(2 * i) * 2 + c] = (short)(((int)pv[c] + x[c]) / 2);
				} else
					p[i * 2 + c] = x[c];
			}
		}
		s->last[0] = l0;
		s->last[1] = l1;
	}
	return fr * up * 2;
}

/* mono: PCM of one frame from Helix (n samples, dec_ch channels at dec_rate) -> mono at out_rate <= dec_rate, 16 or 8
 * bits, forward in place (the output is never bigger). Equal rates: as is. Else linear interpolation (Helix already
 * cut the spectrum at out_rate's Nyquist, see mp3s_start; the old box average of 2-6 samples at uneven times
 * smeared the highs): d_acc = previous sample, d_err = where the next output lies after it, in 1/out_rate of
 * an input sample (out_rate <= 22050 here: the product below stays in 31 bits). Returns bytes. */
static inline int put(MP3S *s, int o, int y)
{
	if (s->out_bits == 8)
		((uint8_t *)s->pcm)[o++] = (uint8_t)((y >> 8) + 128);
	else {
		s->pcm[o / 2] = (short)y;
		o += 2;
	}
	return o;
}

static int down(MP3S *s, int n)
{
	int fr = n / s->dec_ch, i, o = 0;
	short *p = s->pcm;

	for (i = 0; i < fr; i++) {
		int x = s->dec_ch == 2 ? (p[2 * i] + p[2 * i + 1]) / 2 : p[i];
		if (s->dec_rate == s->out_rate) {
			o = put(s, o, x);
			continue;
		}
		for (; s->d_err < s->out_rate; s->d_err += s->dec_rate)
			o = put(s, o, s->d_acc + (x - s->d_acc) * s->d_err / s->out_rate);
		s->d_err -= s->out_rate;
		s->d_acc = x;
	}
	return o;
}

/* one frame into pcm[]: >0 = PCM bytes, 0 = not enough data */
static int decode_frame(MP3S *s)
{
	int tries, off, flen, rate, ch, kbps, err, left;
	unsigned char *p;
	MP3FrameInfo fi;

	for (tries = 0; tries < 32; tries++) {
		refill(s);
		if (s->in_len < 4)
			return 0;
		off = MP3FindSyncWord(IN(s), s->in_len);
		if (off < 0) {
			drop(s, s->in_len - 3);
			continue;
		}
		drop(s, off);
		if (s->in_len < 4)
			continue;
		flen = mp3_frame_len(IN(s), &rate, &ch, &kbps);
		if (flen <= 0) {
			drop(s, 1);
			continue;
		}
		if (s->in_len < flen + 4) {
			if (s->in_pos) {	/* the whole frame must be in in[] */
				bmove(s->in, s->in + s->in_pos, s->in_len);
				s->in_pos = 0;
			}
			s->in_len += (int)ring_read(s->net, s->in + s->in_len, (uint32_t)(MP3S_IN - s->in_len));
			if (s->in_len < flen)
				return 0;
		}
		if (rate != s->rate || ch != s->ch) {	/* another format: not ours to play */
			int r2, c2;
			/* only if the next header (here by now: in_len >= flen + 4 or the ring ran dry) agrees: a
			 * false sync inside frame data (seen on the phone after a TLS reconnect) must not restart */
			if (s->in_len < flen + 4 || mp3_frame_len(IN(s) + flen, &r2, &c2, 0) <= 0 || r2 != rate ||
			    c2 != ch) {
				drop(s, 1);
				continue;
			}
			s->fmt_changed = 1;
			drop(s, flen);
			continue;
		}
		p = IN(s);
		left = s->in_len;
		{
			uint32_t t0 = s->clock ? s->clock() : 0;
			err = MP3Decode(s->dec, &p, &left, s->pcm, 0);
			if (s->clock) {
				uint32_t dt = s->clock() - t0;
				s->dec_ticks += dt;
				s->dec_hist[dt > 3 ? 3 : dt]++;
			}
		}
		drop(s, s->in_len - left > 0 ? s->in_len - left : 1);
		if (err == ERR_MP3_NONE) {
			MP3GetLastFrameInfo(s->dec, &fi);
			s->frames++;
			s->bitrate = kbps;
			if (fi.outputSamps <= 0 || fi.outputSamps > 1152 * 2)
				continue;
			if (s->mono) {
				int nb = down(s, fi.outputSamps);
				if (!nb)
					continue;
				return nb;
			}
			return widen(s, fi.outputSamps) * 2;
		}
		if (err != ERR_MP3_MAINDATA_UNDERFLOW)	/* underflow = bit reservoir after a sync */
			s->errors++;
	}
	return -1;	/* only junk so far: try again on the next read */
}

int mp3s_read(MP3S *s, uint8_t *out, int len)
{
	/* frames this read may decode: what len needs (by the last frame's PCM size) + 2 */
	int done = 0, k, r, budget = 2 + len / (s->pcm_len > 64 ? s->pcm_len : 64);

	while (done < len) {
		if (s->pcm_off < s->pcm_len) {
			k = s->pcm_len - s->pcm_off;
			if (k > len - done)
				k = len - done;
			ring_cpy(out + done, (const uint8_t *)s->pcm + s->pcm_off, (uint32_t)k);
			s->real_bytes += (uint32_t)k;
			s->pcm_off += k;
			done += k;
			continue;
		}
		if (!s->running)
			break;
		if (s->waiting) {
			if (ring_used(s->net) + (uint32_t)s->in_len < s->resume_at)
				break;
			s->waiting = 0;
		}
		if (--budget < 0)	/* never hog the Obs task: the rest of this read is silence */
			break;
		r = decode_frame(s);
		if (r > 0) {
			s->pcm_len = r;
			s->pcm_off = 0;
			continue;
		}
		if (r == 0) {		/* ran dry */
			s->waiting = 1;
			s->gaps++;
		}
		break;
	}
	if (done < len) {
		if (s->out_bits == 8) {		/* unsigned 8-bit: silence = 0x80 */
			int i;
			for (i = done; i < len; i++)
				out[i] = 0x80;
		} else
			bzero_(out + done, (uint32_t)(len - done));
		if (s->running)
			s->gap_bytes += (uint32_t)(len - done);
	}
	s->out_bytes += (uint32_t)len;
	return len;
}

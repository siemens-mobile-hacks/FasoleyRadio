/* MP3 frame headers (both builds: mp3s.c with Helix, mp3pass.c pass-through). No firmware calls. */
#include "mp3s.h"

int mp3_frame_len(const uint8_t *h, int *rate, int *ch, int *kbps)
{
	static const uint16_t br1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
	static const uint16_t br2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
	static const uint16_t sr[3] = { 44100, 48000, 32000 };
	int ver, bi, si, r, br, pad;

	if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0 || ((h[1] >> 1) & 3) != 1)
		return 0;
	ver = (h[1] >> 3) & 3;		/* 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5 */
	bi = h[2] >> 4;
	si = (h[2] >> 2) & 3;
	if (ver == 1 || bi == 0 || bi == 15 || si == 3)
		return 0;
	pad = (h[2] >> 1) & 1;
	r = sr[si] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
	br = (ver == 3 ? br1[bi] : br2[bi]);
	if (rate)
		*rate = r;
	if (ch)
		*ch = (h[3] >> 6) == 3 ? 1 : 2;
	if (kbps)
		*kbps = br;
	return (ver == 3 ? 144 : 72) * br * 1000 / r + pad;
}

int mp3s_probe(const RING *r, int *rate, int *ch, int *kbps)
{
	uint32_t used = ring_used(r), off;
	uint8_t h[4], h2[4];
	int len, r2, c2, k2;

	for (off = 0; off + 4 <= used && off < 64 * 1024; off++) {
		if (ring_peek(r, off, h, 1) != 1 || h[0] != 0xFF)
			continue;
		if (ring_peek(r, off, h, 4) != 4)
			return 0;
		len = mp3_frame_len(h, rate, ch, kbps);
		if (len <= 0)
			continue;
		if (off + len + 4 > used)
			return 0;	/* wait for more */
		ring_peek(r, off + len, h2, 4);
		if (mp3_frame_len(h2, &r2, &c2, &k2) > 0 && r2 == *rate && c2 == *ch)
			return 1;
	}
	return 0;
}

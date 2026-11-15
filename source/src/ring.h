/* Byte ring, one writer and one reader in different tasks (helper writes, Obs task reads).
 * wr/rd are free-running byte counters; size = power of 2. No firmware calls. */
#ifndef RADIO_RING_H
#define RADIO_RING_H
#include <stdint.h>

typedef struct {
	uint8_t *buf;
	uint32_t mask;			/* size - 1 */
	volatile uint32_t wr, rd;
} RING;

static inline uint32_t ring_size(const RING *r) { return r->mask + 1; }
static inline uint32_t ring_used(const RING *r) { return r->wr - r->rd; }
static inline uint32_t ring_free(const RING *r) { return ring_size(r) - ring_used(r); }

static inline void ring_cpy(uint8_t *d, const uint8_t *s, uint32_t n)
{
	if (!(((uintptr_t)d | (uintptr_t)s) & 3)) {	/* PCM to the player's chunk: words */
		uint32_t *dw = (uint32_t *)d;
		const uint32_t *sw = (const uint32_t *)s;
		for (; n >= 4; n -= 4)
			*dw++ = *sw++;
		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}
	while (n--)
		*d++ = *s++;
}

/* writer side: as much as fits */
static inline uint32_t ring_write(RING *r, const void *src, uint32_t n)
{
	const uint8_t *s = src;
	uint32_t f = ring_free(r), done = 0, wr = r->wr;
	if (n > f)
		n = f;
	while (done < n) {
		uint32_t off = wr & r->mask, k = n - done;
		if (k > ring_size(r) - off)
			k = ring_size(r) - off;
		ring_cpy(r->buf + off, s + done, k);
		done += k;
		wr += k;
	}
	r->wr = wr;			/* publish after the bytes are in */
	return n;
}

/* reader side: copy n bytes at offset off from the read position without taking them */
static inline uint32_t ring_peek(const RING *r, uint32_t off, void *dst, uint32_t n)
{
	uint8_t *d = dst;
	uint32_t u = ring_used(r), done = 0, rd = r->rd + off;
	if (off >= u)
		return 0;
	if (n > u - off)
		n = u - off;
	while (done < n) {
		uint32_t o = rd & r->mask, k = n - done;
		if (k > ring_size(r) - o)
			k = ring_size(r) - o;
		ring_cpy(d + done, r->buf + o, k);
		done += k;
		rd += k;
	}
	return n;
}

static inline uint32_t ring_read(RING *r, void *dst, uint32_t n)
{
	n = ring_peek(r, 0, dst, n);
	r->rd += n;
	return n;
}

#endif

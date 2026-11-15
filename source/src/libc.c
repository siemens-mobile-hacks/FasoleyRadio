/* No libc in this elf (__NO_LIBC). The SDK crt has no linkable string functions: GCC lowers struct zeroing to
 * memset (as s65-pcmplayer) and BearSSL (src/link.c) calls the rest. Built with
 * -fno-tree-loop-distribute-patterns so the loops do not turn back into calls of themselves. */
#include <stddef.h>
#include <stdint.h>

void *memset(void *dst, int c, size_t n)
{
	unsigned char *p = (unsigned char *)dst;
	while (n--)
		*p++ = (unsigned char)c;
	return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	if ((((uintptr_t)d | (uintptr_t)s) & 3) == 0) {
		uint32_t *dw = (uint32_t *)d;
		const uint32_t *sw = (const uint32_t *)s;
		for (; n >= 4; n -= 4)
			*dw++ = *sw++;
		d = (unsigned char *)dw;
		s = (const unsigned char *)sw;
	}
	while (n--)
		*d++ = *s++;
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	if (d <= s || d >= s + n)
		return memcpy(dst, src, n);
	while (n--)
		d[n] = s[n];
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;
	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

size_t strlen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

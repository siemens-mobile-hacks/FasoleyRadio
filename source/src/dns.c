/* DNS over TCP, message side (see dns.h). RFC 1035; one question, A/IN, recursion desired. */
#include "dns.h"

int dns_query(uint8_t *b, const char *host, uint16_t id)
{
	int n = 2 + 12, lab, k;
	const char *p = host;

	b[2] = (uint8_t)(id >> 8);
	b[3] = (uint8_t)id;
	b[4] = 0x01;		/* RD */
	b[5] = 0;
	b[6] = 0;
	b[7] = 1;		/* QDCOUNT */
	for (k = 8; k < 14; k++)
		b[k] = 0;
	while (*p) {
		lab = n++;
		for (k = 0; *p && *p != '.'; p++, k++) {
			if (k >= 63 || n >= 2 + 12 + 255)
				return 0;
			b[n++] = (uint8_t)*p;
		}
		if (!k)
			return 0;
		b[lab] = (uint8_t)k;
		if (*p == '.')
			p++;
	}
	if (n == 2 + 12)
		return 0;
	b[n++] = 0;
	b[n++] = 0;
	b[n++] = 1;		/* QTYPE A */
	b[n++] = 0;
	b[n++] = 1;		/* QCLASS IN */
	b[0] = (uint8_t)((n - 2) >> 8);
	b[1] = (uint8_t)(n - 2);
	return n;
}

/* skip a (possibly compressed) name at m[i]; -> index after it, -1 = bad */
static int skip_name(const uint8_t *m, int len, int i)
{
	int guard = 0;
	while (i < len && guard++ < 128) {
		uint8_t c = m[i];
		if (!c)
			return i + 1;
		if ((c & 0xC0) == 0xC0)
			return i + 2 <= len ? i + 2 : -1;
		if (c & 0xC0)
			return -1;
		i += 1 + c;
	}
	return -1;
}

int dns_answer(const uint8_t *buf, int n, uint16_t id, uint32_t *ip)
{
	const uint8_t *m = buf + 2;
	int len, i, qd, an;

	if (n < 2 + 12)
		return -1;
	len = buf[0] << 8 | buf[1];
	if (len > n - 2 || len < 12)
		return -1;
	if ((m[0] << 8 | m[1]) != id || !(m[2] & 0x80))
		return -1;
	if (m[3] & 0x0F)
		return 0;		/* NXDOMAIN, SERVFAIL, ... */
	qd = m[4] << 8 | m[5];
	an = m[6] << 8 | m[7];
	i = 12;
	while (qd-- > 0) {
		if ((i = skip_name(m, len, i)) < 0 || i + 4 > len)
			return -1;
		i += 4;
	}
	while (an-- > 0) {		/* CNAMEs come first; the first A/IN with 4 bytes wins */
		int type, cls, rdlen;
		if ((i = skip_name(m, len, i)) < 0 || i + 10 > len)
			return -1;
		type = m[i] << 8 | m[i + 1];
		cls = m[i + 2] << 8 | m[i + 3];
		rdlen = m[i + 8] << 8 | m[i + 9];
		i += 10;
		if (i + rdlen > len)
			return -1;
		if (type == 1 && cls == 1 && rdlen == 4) {
			*ip = (uint32_t)m[i] | (uint32_t)m[i + 1] << 8 | (uint32_t)m[i + 2] << 16 | (uint32_t)m[i + 3] << 24;
			return 1;
		}
		i += rdlen;
	}
	return 0;
}

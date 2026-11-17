/*
 * net.h for EL71v45 (ELKA build): net.c's ENIP TCP socket, but names are resolved by the elf itself with
 * DNS over TCP (dns.c) on another ENIP TCP socket, not with the firmware's async_gethostbyname.
 *
 * Why (D49, RE-EL71.txt): D30 hung the EL71 hard right after the first async_gethostbyname (log: net state
 * RS_CONNECT, nothing after). The firmware resolver (A0A649A0) blocks the caller on a global semaphore while
 * ENIP works on a request that lives on the caller's stack, and it picks the context/bearer from the
 * caller's socket registration; on EL71 with bt-pan a resolve from a non-cid-1 context already corrupted
 * memory (bt-pan D27: data abort A0A64B40 in async_gethostbyname, RAP_BEARER). No elf on EL71 used it
 * successfully (tgram, smb, bt-pan's test connect to IP addresses). Plain TCP sockets from the helper work.
 *
 * Servers, in order: the DNS bt-pan put into the GPRS PDP driver table (A8E9E9CC, 8 x 0x2C, +8 dns1,
 * +C dns2: what the firmware resolver would ask), then 8.8.8.8, 1.1.1.1. 5 s each. Answers are kept
 * for the session (reconnects / redirects to the same host do not ask again).
 *
 * Stages go synchronously to 0:\radio-net.txt (open/append/close per line, helper context only),
 * so the last line survives a hang.
 */
#include <swilib.h>
#include "net.h"
#include "dns.h"
#include "radio.h"

#ifdef HOST_TEST	/* tests/host/netelka */
extern uint32_t host_pdp[];
#define PDP_DRIVERS	((uint32_t)host_pdp)
#else
#define PDP_DRIVERS	((uint32_t)RamPdpDrivers())	/* library 8471 (D73; was EL71v45 A8E9E9CC) */
#endif
#define DNS_MS		5000
#define NCACHE		4

static int st = NET_NONE, fd = -1, port_;
static char host_[96];
int net_err;

/* ---------- TCP socket ---------- */

static int tcp_open(uint32_t ip, int port)
{
	struct sockaddr_in sa;
	int s;

	*socklasterr() = 0;
	s = socket(1, 1, 0);	/* AF_INET, SOCK_STREAM on this stack */
	if (s < 0)
		return -2;
	zeromem(&sa, sizeof(sa));
	sa.sin_family = 1;
	sa.sin_port = htons((uint16_t)port);
	sa.sin_addr.s_addr = ip;
	if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == -1) {
		closesocket(s);
		return -3;
	}
	return s;
}

/* ---------- name cache ---------- */

static struct { char host[96]; uint32_t ip; } cache[NCACHE];
static int cache_next;

static int cache_get(const char *h, uint32_t *ip)
{
	int i;
	for (i = 0; i < NCACHE; i++)
		if (cache[i].ip && !strcmp(cache[i].host, h)) {
			*ip = cache[i].ip;
			return 1;
		}
	return 0;
}

static void cache_put(const char *h, uint32_t ip)
{
	strcpy(cache[cache_next].host, h);
	cache[cache_next].ip = ip;
	cache_next = (cache_next + 1) % NCACHE;
}

static void cache_drop(const char *h)
{
	int i;
	for (i = 0; i < NCACHE; i++)
		if (!strcmp(cache[i].host, h))
			cache[i].ip = 0;
}

/* ---------- DNS over TCP ---------- */

static int dfd = -1, dphase, dsrv, nsrv, dlen, dqlen;	/* dphase: 1 connecting, 2 query sent */
static uint32_t srv[4], dt0;
static uint16_t did;
static uint8_t dbuf[DNS_MAX], dq[300];
static int from_cache, dgen;

static void do_connect(uint32_t ip)
{
	fd = tcp_open(ip, port_);
	if (fd < 0) {
		net_err = fd == -2 ? 2 : 3;
		fd = -1;
		st = NET_FAILED;
		cache_drop(host_);
		return;
	}
	st = NET_CONNECTING;
}

static void dns_close(void)
{
	if (dfd >= 0)
		closesocket(dfd);
	dfd = -1;
	dphase = 0;
	dgen++;
}

static void dns_try(void)
{
	dns_close();
	while (dsrv < nsrv) {
		dfd = tcp_open(srv[dsrv], 53);
		if (dfd >= 0) {
			dphase = 1;
			dlen = 0;
			dt0 = radio_ms();
			return;
		}
		dfd = -1;
		dsrv++;
	}
	st = NET_FAILED;
	net_err = 1;
}

static void dns_next(void)
{
	dsrv++;
	dns_try();
}

static void resolve(void)
{
	volatile uint32_t *t = (volatile uint32_t *)PDP_DRIVERS;
	int i, k;

	nsrv = 0;
	for (i = 0; i < 8 && nsrv < 2; i++)
		for (k = 2; k <= 3 && nsrv < 2; k++) {
			uint32_t a = t[i * 11 + k];
			if (a && a != 0xFFFFFFFFu && (nsrv == 0 || srv[0] != a))
				srv[nsrv++] = a;
		}
	srv[nsrv++] = 0x08080808u;	/* 8.8.8.8 */
	srv[nsrv++] = 0x01010101u;	/* 1.1.1.1 */
	did = (uint16_t)(radio_ms() * 2654435761u >> 16);
	dqlen = dns_query(dq, host_, did);
	if (!dqlen) {
		st = NET_FAILED;
		net_err = 1;
		return;
	}
	dsrv = 0;
	st = NET_RESOLVING;
	dns_try();
}

/* polled from net_state() while resolving */
static void dns_poll(void)
{
	int n, r;
	uint32_t ip;

	if (dfd < 0 || st != NET_RESOLVING)
		return;
	if (radio_ms() - dt0 > DNS_MS) {
		dns_next();
		return;
	}
	if (dphase != 2)
		return;
	n = recv(dfd, dbuf + dlen, DNS_MAX - dlen, 0);
	if (n <= 0)
		return;
	dlen += n;
	if (dlen < 2 || dlen < 2 + (dbuf[0] << 8 | dbuf[1])) {
		if (dlen >= DNS_MAX)
			dns_next();
		return;
	}
	r = dns_answer(dbuf, dlen, did, &ip);
	dns_close();
	if (r < 0) {
		dns_next();
		return;
	}
	if (!r) {
		int rc = dlen > 5 ? dbuf[5] & 0x0F : -1;
		if (rc == 3) {		/* NXDOMAIN: the name does not exist */
			st = NET_FAILED;
			net_err = 1;
			return;
		}
		/* REFUSED/SERVFAIL (the phone's NAP refused some TCP queries, D54) or no A record: next server */
		dns_next();
		return;
	}
	cache_put(host_, ip);
	from_cache = 0;
	do_connect(ip);
}

static int parse_ip(const char *p, uint32_t *out)
{
	unsigned v[4], i;
	for (i = 0; i < 4; ++i) {
		unsigned n = 0, d = 0;
		while (*p >= '0' && *p <= '9') {
			n = n * 10 + (unsigned)(*p++ - '0');
			if (++d > 3)
				return 0;
		}
		if (!d || n > 255)
			return 0;
		v[i] = n;
		if (i < 3 && *p++ != '.')
			return 0;
	}
	if (*p)
		return 0;
	*out = (uint32_t)(v[0] | v[1] << 8 | v[2] << 16 | v[3] << 24);	/* network order (LE CPU) */
	return 1;
}

/* ---------- net.h ---------- */

void net_open(const char *host, int port)
{
	uint32_t ip;

	net_close();
	net_err = 0;
	strncpy(host_, host, sizeof(host_) - 1);
	host_[sizeof(host_) - 1] = 0;
	port_ = port;
	from_cache = 0;
	if (parse_ip(host_, &ip))
		do_connect(ip);
	else if (cache_get(host_, &ip)) {
		from_cache = 1;
		do_connect(ip);
	} else
		resolve();
}

int net_state(void)
{
	dns_poll();
	return st;
}

int net_send(const void *buf, int len)
{
	int n;
	if (st != NET_UP)
		return st == NET_CLOSED || st == NET_FAILED ? -1 : 0;
	n = send(fd, buf, len, 0);
	return n > 0 ? n : 0;	/* buffer full: ENIP_BUFFER_FREE comes, the next poll sends more */
}

int net_recv(void *buf, int len)
{
	int n;
	if (fd < 0)
		return st == NET_CLOSED || st == NET_FAILED ? -1 : 0;
	n = recv(fd, buf, len, 0);
	if (n > 0)
		return n;
	return st == NET_CLOSED || st == NET_FAILED ? -1 : 0;
}

void net_close(void)
{
	dns_close();
	if (fd >= 0)
		closesocket(fd);
	fd = -1;
	st = NET_NONE;
}

int net_ours(int id)
{
	return (fd >= 0 && fd == id) || (dfd >= 0 && dfd == id);
}

void net_event(int event, int id)
{
	if (dfd >= 0 && id == dfd) {
		switch (event) {
		case ENIP_SOCK_CONNECTED:
			if (dphase == 1) {
				int n = send(dfd, dq, dqlen, 0);
				if (n != dqlen) {
					dns_next();
					return;
				}
				dphase = 2;
			}
			break;
		case ENIP_SOCK_DATA_READ:
			dns_poll();
			break;
		case ENIP_SOCK_REMOTE_CLOSED:
		case ENIP_SOCK_CLOSED:
		{
			int g = dgen;
			dns_poll();	/* the answer may come with the close */
			if (g == dgen)
				dns_next();
		}
			break;
		}
		return;
	}
	if (fd < 0 || fd != id)
		return;
	switch (event) {
	case ENIP_SOCK_CONNECTED:
		if (st == NET_CONNECTING) {
			st = NET_UP;
		}
		break;
	case ENIP_SOCK_REMOTE_CLOSED:
	case ENIP_SOCK_CLOSED:
		if (st == NET_CONNECTING && from_cache)
			cache_drop(host_);
		st = NET_CLOSED;
		break;
	}
}

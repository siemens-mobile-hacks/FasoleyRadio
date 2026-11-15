/*
 * One TCP connection over the ENIP sockets, helper context (from browser/src/phone/plat_phone.c):
 * names resolved with async_gethostbyname (0xC9/0xD6 = wait for ENIP_DNR_HOST_BY_NAME with our request
 * id, then ask again). Any open LMAN session carries the socket: bt-pan or GPRS.
 */
#include <swilib.h>
#include "net.h"

static int st = NET_NONE, fd = -1, dns_id, port_;
static char host_[96];
int net_err;

static void do_connect(uint32_t ip)
{
	struct sockaddr_in sa;

	*socklasterr() = 0;
	fd = socket(1, 1, 0);	/* AF_INET, SOCK_STREAM on this stack */
	if (fd < 0) {
		fd = -1;
		st = NET_FAILED;
		net_err = 2;
		return;
	}
	zeromem(&sa, sizeof(sa));
	sa.sin_family = 1;
	sa.sin_port = htons((uint16_t)port_);
	sa.sin_addr.s_addr = ip;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == -1) {
		closesocket(fd);
		fd = -1;
		st = NET_FAILED;
		net_err = 3;
		return;
	}
	st = NET_CONNECTING;
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

static void resolve(void)
{
	struct hostent *res = 0;
	int id = 0, err;
	uint32_t ip;

	*socklasterr() = 0;
	err = async_gethostbyname(host_, &res, &id);
	if (!err && res && res->h_addr_list && res->h_addr_list[0]) {
		memcpy(&ip, res->h_addr_list[0], 4);
		do_connect(ip);
	} else if ((err == 0xC9 || err == 0xD6) && id) {
		dns_id = id;
		st = NET_RESOLVING;
	} else {
		st = NET_FAILED;
		net_err = 1;
	}
}

void net_open(const char *host, int port)
{
	uint32_t ip;

	net_close();
	net_err = 0;
	strncpy(host_, host, sizeof(host_) - 1);
	host_[sizeof(host_) - 1] = 0;
	port_ = port;
	dns_id = 0;
	if (parse_ip(host_, &ip))
		do_connect(ip);
	else {
		st = NET_RESOLVING;
		resolve();
	}
}

int net_state(void)
{
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
	if (fd >= 0)
		closesocket(fd);
	fd = -1;
	st = NET_NONE;
	dns_id = 0;
}

int net_ours(int id)
{
	return (fd >= 0 && fd == id) || (st == NET_RESOLVING && dns_id && dns_id == id);
}

void net_event(int event, int id)
{
	if (event == ENIP_DNR_HOST_BY_NAME) {
		if (st == NET_RESOLVING && dns_id == id)
			resolve();
		return;
	}
	if (fd < 0 || fd != id)
		return;
	switch (event) {
	case ENIP_SOCK_CONNECTED:
		if (st == NET_CONNECTING)
			st = NET_UP;
		break;
	case ENIP_SOCK_REMOTE_CLOSED:
	case ENIP_SOCK_CLOSED:
		st = NET_CLOSED;
		break;
	}
}

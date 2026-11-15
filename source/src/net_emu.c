/*
 * net.h for the Radio_EMU build: elfloader3-linux has no sockets (bsd_* SWIs are stubs), so this plays
 * the server from files in the elf's folder, like tests/emu/server.py would:
 *   /mp3-32  -> 302 Location: http://127.0.0.1:18080/raw32 (absolute)
 *   /mp3-128 -> 302 Location: /raw128 (relative)
 *   /fasoley -> 308 Location: /raw320 (rp320.raw = tests/data/fasoley.raw: 320k 44.1 kHz stereo, metaint 16000)
 *   /rawNNs  -> the same at 1/8 of the bitrate (hopeless: the elf must stop with a message)
 *   /rawNNh  -> at 5/8 (slow but playable: plays with gaps, no stop)
 *   /rawNN   -> rpNN.raw (captured response: HTTP headers + ICY metadata), 2x the bitrate, then closed
 * Everything above it (radio.c state machine, redirects, reconnect, ICY, ring, decoder) is the real code.
 */
#include <swilib.h>
#include "net.h"
#include "radio.h"

int net_err;
extern char elf_dir[];

static int st = NET_NONE, fd = -1, kbps, opens, slow;
static char reqbuf[600];
static int req_len;
static const char *canned;	/* redirect answer */
static int canned_pos;
static uint32_t t0, sent;

void net_open(const char *host, int port)
{
	(void)host;
	(void)port;
	net_close();
	opens++;
	st = NET_CONNECTING;
	t0 = radio_ms();
}

int net_state(void)
{
	if (st == NET_CONNECTING && radio_ms() - t0 > 300)
		st = NET_UP;	/* "connected" after 0.3 s */
	return st;
}

int net_send(const void *buf, int len)
{
	char p[160];
	unsigned err;
	int n;

	if (st != NET_UP)
		return st == NET_CLOSED || st == NET_FAILED ? -1 : 0;
	if (req_len + len > (int)sizeof(reqbuf) - 1)
		return -1;
	memcpy(reqbuf + req_len, buf, len);
	req_len += len;
	reqbuf[req_len] = 0;
	if (!strstr(reqbuf, "\r\n\r\n"))
		return len;
	if (!strncmp(reqbuf, "GET /mp3-32 ", 12))
		canned = "HTTP/1.0 302 Found\r\nLocation: http://127.0.0.1:18080/raw32\r\n\r\n";
	else if (!strncmp(reqbuf, "GET /mp3-128 ", 13))
		canned = "HTTP/1.1 302 Moved Temporarily\r\nLocation: /raw128\r\nContent-Length: 0\r\n\r\n";
	else if (!strncmp(reqbuf, "GET /fasoley ", 13))
		canned = "HTTP/1.1 308 Permanent Redirect\r\nLocation: /raw320\r\nContent-Length: 0\r\n\r\n";
	else if (!strncmp(reqbuf, "GET /raw", 8)) {
		for (kbps = 0, n = 8; reqbuf[n] >= '0' && reqbuf[n] <= '9'; n++)
			kbps = kbps * 10 + reqbuf[n] - '0';
		slow = reqbuf[n] == 's' ? 1 : reqbuf[n] == 'h' ? 2 : 0;	/* /rawNNs 1/8, /rawNNh 5/8 of the bitrate */
		sprintf(p, "%s\\rp%d.raw", elf_dir, kbps);
		fd = _open(p, A_ReadOnly + A_BIN, P_READ, &err);
		if (fd < 0)
			canned = "HTTP/1.0 404 Not Found\r\n\r\n";
		t0 = radio_ms();
		sent = 0;
	} else
		canned = "HTTP/1.0 404 Not Found\r\n\r\n";
	canned_pos = 0;
	return len;
}

int net_recv(void *buf, int len)
{
	unsigned err;
	uint32_t allowed;
	int n;

	if (st == NET_CLOSED)
		return -1;
	if (st != NET_UP)
		return 0;
	if (canned) {
		n = strlen(canned + canned_pos);
		if (!n) {
			st = NET_CLOSED;
			return -1;
		}
		if (n > len)
			n = len;
		memcpy(buf, canned + canned_pos, n);
		canned_pos += n;
		return n;
	}
	if (fd < 0)
		return 0;
	allowed = (radio_ms() - t0) * kbps * (slow == 1 ? 1 : slow == 2 ? 5 : 16) / 64 + 2048;	/* 2x, 1/8x, 5/8x */
	if (sent >= allowed)
		return 0;
	if ((uint32_t)len > allowed - sent)
		len = (int)(allowed - sent);
	n = _read(fd, buf, len, &err);
	if (n <= 0) {
		_close(fd, &err);
		fd = -1;
		st = NET_CLOSED;	/* end of the capture: the server closes */
		return -1;
	}
	sent += (uint32_t)n;
	return n;
}

void net_close(void)
{
	unsigned err;
	if (fd >= 0)
		_close(fd, &err);
	fd = -1;
	st = NET_NONE;
	req_len = 0;
	canned = 0;
}

int net_ours(int id)
{
	(void)id;
	return 0;
}

void net_event(int event, int id)
{
	(void)event;
	(void)id;
}

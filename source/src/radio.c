/* Stream connection (see radio.h). Helper context. */
#include <swilib.h>
#include "radio.h"
#include "net.h"
#include "link.h"

#define MAX_REDIRECTS	5
#define CONNECT_MS	20000	/* DNS + TCP + headers */
#define SILENT_MS	15000	/* no body bytes for this long: reconnect */
#define RX_CHUNK	2048

RADIO radio;
RING net_ring;

static char start_url[256];
static char req[512];
static int req_len, req_sent, redirects, fails;
static uint32_t t_state, t_rx, retry_at;
static uint8_t rxbuf[RX_CHUNK];

uint32_t radio_ms(void)
{
	return (uint32_t)((uint64_t)GetSessionAge() * OS_TICK_PERIOD_US / 1000u);
}

static void set_state(int s)
{
	radio.state = s;
	t_state = radio_ms();
}

static void connect_url(void)
{
	char host[96], path[200];
	int port, k;

	icy_init(&radio.icy);
	k = url_parse(radio.url, host, sizeof(host), &port, path, sizeof(path));
	if (!k) {
		radio.err = "URL is not http(s)://";
		link_close();
		set_state(RS_ERROR);
		return;
	}
	radio.tls = k == 2;
	req_len = http_request(req, sizeof(req), host, port, path, radio.tls);
	req_sent = 0;
	link_open(host, port, radio.tls);
	set_state(RS_CONNECT);
}

static void retry(const char *why)
{
	static const uint16_t delay[] = { 1000, 2000, 4000, 8000, 15000 };
	link_close();
	radio.err = why;
	if (radio.no_retry) {
		set_state(RS_ERROR);
		return;
	}
	strcpy(radio.url, start_url);	/* redirects are taken again */
	redirects = 0;
	retry_at = radio_ms() + delay[fails < 5 ? fails : 4];
	if (fails < 100)
		fails++;
	set_state(RS_RETRY);
}

void radio_open(const char *url)
{
	strncpy(start_url, url, sizeof(start_url) - 1);
	start_url[sizeof(start_url) - 1] = 0;
	strcpy(radio.url, start_url);
	net_ring.wr = net_ring.rd = 0;	/* no reader now (main.c) */
	radio.rx = 0;
	radio.reconnects = 0;
	radio.err = 0;
	redirects = 0;
	fails = 0;
	connect_url();
}

void radio_close(void)
{
	link_close();
	set_state(RS_IDLE);
}

static void sink(void *ctx, const uint8_t *p, int n)
{
	(void)ctx;
	ring_write(&net_ring, p, (uint32_t)n);	/* fits: recv asked for at most ring_free bytes */
	radio.rx += (uint32_t)n;
}

void radio_poll(void)
{
	int ns, n, r, loops;
	uint32_t now = radio_ms();

	switch (radio.state) {
	case RS_IDLE:
	case RS_ERROR:
		return;
	case RS_RETRY:
		if ((int32_t)(now - retry_at) >= 0) {
			radio.reconnects++;
			connect_url();
		}
		return;
	}
	ns = link_state();
	if (ns == NET_FAILED) {
		retry(link_err ? link_err : net_err == 1 ? "No DNS (internet?)" : "No connection");
		return;
	}
	if (radio.state == RS_CONNECT) {
		if (ns == NET_UP) {
			set_state(RS_HEADERS);
			t_rx = now;
		} else if (ns == NET_CLOSED) {
			retry("Connection closed");
			return;
		} else {
			if (now - t_state > CONNECT_MS)
				retry("No response");
			return;
		}
	}
	while (req_sent < req_len) {
		n = link_send(req + req_sent, req_len - req_sent);
		if (n < 0) {
			retry("Connection closed");
			return;
		}
		if (n == 0)
			break;
		req_sent += n;
	}
	for (loops = 0; loops < 16; loops++) {
		uint32_t room = ring_free(&net_ring);
		if (room < 512)
			break;		/* full: TCP waits for us */
		if (room > RX_CHUNK)
			room = RX_CHUNK;
		n = link_recv(rxbuf, (int)room);
		if (n < 0) {
			retry("Stream broke off");
			return;
		}
		if (n == 0)
			break;
		t_rx = now;
		r = icy_feed(&radio.icy, rxbuf, n, sink, 0);
		if (r == ICY_REDIRECT) {
			char next[256];
			if (++redirects > MAX_REDIRECTS || !url_join(next, sizeof(next), radio.url, radio.icy.location)) {
				link_close();
				radio.err = "Bad redirect";
				set_state(RS_ERROR);
				return;
			}
			strcpy(radio.url, next);
			connect_url();
			return;
		}
		if (r == ICY_ERROR) {
			link_close();
			radio.err = radio.icy.status ? "HTTP error" : "Not an HTTP reply";
			if (radio.icy.status >= 500 || !radio.icy.status)
				retry(radio.err);
			else
				set_state(RS_ERROR);
			return;
		}
		if (r == ICY_BODY && radio.state == RS_HEADERS) {
			set_state(RS_STREAM);
			fails = 0;
		}
	}
	if (ring_free(&net_ring) >= 512 && now - t_rx > (radio.state == RS_STREAM ? SILENT_MS : CONNECT_MS))
		retry("No data");
}

/* Stream connection: HTTP GET, redirects, ICY, reconnect; writes the MP3 bytes into the net ring.
 * Helper context (radio_open/radio_close/radio_poll go through SUBPROC); the MMI only reads. */
#ifndef RADIO_RADIO_H
#define RADIO_RADIO_H
#include <stdint.h>
#include "ring.h"
#include "icy.h"

enum { RS_IDLE, RS_CONNECT, RS_HEADERS, RS_STREAM, RS_RETRY, RS_ERROR };

typedef struct {
	volatile int state;
	volatile uint32_t rx;		/* body bytes received (this station) */
	volatile int reconnects;
	volatile int tls;		/* the current connection is https */
	volatile int no_retry;		/* a broken stream = RS_ERROR instead of reconnecting (settings) */
	const char *err;		/* RS_ERROR / RS_RETRY reason (cp1251) */
	char url[256];			/* current URL (after redirects) */
	ICY icy;
} RADIO;

extern RADIO radio;
extern RING net_ring;

void radio_open(const char *url);
void radio_close(void);
void radio_poll(void);
uint32_t radio_ms(void);

#endif

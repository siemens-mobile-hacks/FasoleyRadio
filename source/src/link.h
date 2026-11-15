/* The stream's connection: plain TCP (net.c) or TLS over it (BearSSL sans-I/O client as browser/src/core/
 * conn.c: no trust anchors, any certificate chain and clock accepted). Helper context. The TLS state
 * (~22 KB: 16.7 KB record buffer + contexts) is allocated only while an https connection is open. */
#ifndef RADIO_LINK_H
#define RADIO_LINK_H

void link_open(const char *host, int port, int tls);
int link_state(void);			/* NET_* of net.h; NET_UP = TLS handshake done */
int link_send(const void *buf, int len);	/* bytes taken, 0 = try later, -1 = gone */
int link_recv(void *buf, int len);	/* bytes, 0 = nothing now, -1 = closed */
void link_close(void);
extern const char *link_err;		/* TLS failure text (cp1251), 0 = none */
extern int link_tls_err;		/* BearSSL error code of the last TLS failure */
unsigned link_tls_mem(void);		/* bytes the TLS state takes while open */

#endif

/* One TCP connection over the firmware's ENIP sockets (as browser/src/phone/plat_phone.c), helper context. */
#ifndef RADIO_NET_H
#define RADIO_NET_H

enum { NET_NONE, NET_RESOLVING, NET_CONNECTING, NET_UP, NET_CLOSED, NET_FAILED };

void net_open(const char *host, int port);	/* state -> NET_RESOLVING/NET_CONNECTING/NET_FAILED */
int net_state(void);
int net_send(const void *buf, int len);	/* bytes sent, 0 = try later, -1 = gone */
int net_recv(void *buf, int len);	/* bytes, 0 = nothing now, -1 = closed */
void net_close(void);
int net_ours(int id);			/* socket or DNS request id of a MSG_HELPER_TRANSLATOR event */
void net_event(int event, int id);
extern int net_err;			/* last failure: 1 DNS, 2 socket, 3 connect */

#endif

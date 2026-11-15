/* DNS A query / answer in the TCP form (2-byte length + message), no I/O: src/net_elka.c sends it over an
 * ordinary ENIP TCP socket instead of the firmware resolver (EL71, see RE-EL71.txt "D49"). Host test:
 * tests/host/dns_test.c. */
#ifndef RADIO_DNS_H
#define RADIO_DNS_H
#include <stdint.h>

#define DNS_MAX		512	/* answers we accept (TCP form, incl. the 2 length bytes) */

/* query for host's A record into buf (>= 300 bytes) -> bytes incl. the length prefix, 0 = bad name */
int dns_query(uint8_t *buf, const char *host, uint16_t id);
/* complete TCP answer (n bytes incl. the prefix): 1 = *ip (network order as in memory), 0 = no A record /
 * error rcode, -1 = malformed or not our id */
int dns_answer(const uint8_t *buf, int n, uint16_t id, uint32_t *ip);

#endif

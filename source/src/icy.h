/* HTTP/1.0 + ICY (Shoutcast/Icecast) response parser and URL helpers. No firmware calls, no libc:
 * the same code runs in the host test. */
#ifndef RADIO_ICY_H
#define RADIO_ICY_H
#include <stdint.h>

#define ICY_LINE	256
#define ICY_TITLE	160

typedef struct {
	int hdr_done;		/* headers parsed, body follows */
	int status;		/* 200, 302, ... (ICY 200 OK = 200) */
	int br;			/* icy-br, kbps (0 = not given) */
	uint32_t metaint;	/* icy-metaint (0 = no metadata in the body) */
	char location[ICY_LINE];
	char name[64];		/* icy-name */
	char ctype[48];		/* Content-Type */
	char title[ICY_TITLE];	/* last StreamTitle, UTF-8 or latin1 as sent */
	volatile int title_seq;	/* +1 on every new title */
	uint32_t audio;		/* audio bytes passed on */
	/* private */
	int first;
	char line[ICY_LINE];
	int line_len;
	uint32_t until_meta;
	int meta_left;		/* -1 = next byte is the length byte */
	char meta[512];
	int meta_len;
	int chunked;		/* Transfer-Encoding: chunked */
	int chunk_st;		/* 0 size digits, 1 extension, 2 CRLF after data, 3 after the last chunk */
	uint32_t chunk_size, chunk_left;
} ICY;

enum { ICY_MORE = 0, ICY_BODY = 1, ICY_REDIRECT = 2, ICY_ERROR = -1 };

typedef void (*icy_sink)(void *ctx, const uint8_t *p, int n);

void icy_init(ICY *c);
/* feed received bytes; audio goes to sink. Returns ICY_MORE (headers not complete yet), ICY_BODY
 * (streaming), ICY_REDIRECT (3xx with a Location: stop reading), ICY_ERROR (bad status/response). */
int icy_feed(ICY *c, const uint8_t *p, int n, icy_sink sink, void *ctx);

/* http[s]://host[:port][/path] -> parts; 0 = not an http/https URL, 1 = http, 2 = https (port 443) */
int url_parse(const char *url, char *host, int host_sz, int *port, char *path, int path_sz);
/* GET request for the URL parts; returns its length */
int http_request(char *out, int out_sz, const char *host, int port, const char *path, int tls);
/* resolve a Location value against the current URL (absolute http[s]://..., or /path on the same host and scheme) */
int url_join(char *out, int out_sz, const char *base, const char *loc);

#endif

/* HTTP/ICY response parser (see icy.h). */
#include "icy.h"

static int s_len(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

static int lc(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* "Name:" at the start of line (any case) -> value after spaces, else 0 */
static const char *hdr_val(const char *line, const char *name)
{
	int i;
	for (i = 0; name[i]; i++)
		if (lc((unsigned char)line[i]) != name[i])
			return 0;
	if (line[i] != ':')
		return 0;
	line += i + 1;
	while (*line == ' ' || *line == '\t')
		line++;
	return line;
}

static void s_copy(char *d, int sz, const char *s)
{
	int i = 0;
	while (s[i] && i < sz - 1) {
		d[i] = s[i];
		i++;
	}
	d[i] = 0;
}

static uint32_t s_num(const char *s)
{
	uint32_t v = 0;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (uint32_t)(*s++ - '0');
	return v;
}

void icy_init(ICY *c)
{
	char *p = (char *)c;
	unsigned i;
	for (i = 0; i < sizeof(*c); i++)
		p[i] = 0;
	c->first = 1;
	c->meta_left = -1;
}

static int status_line(ICY *c)
{
	const char *l = c->line;
	/* "HTTP/1.x 200 OK" or "ICY 200 OK" */
	if ((l[0] == 'H' && l[1] == 'T' && l[2] == 'T' && l[3] == 'P' && l[4] == '/') ||
	    (l[0] == 'I' && l[1] == 'C' && l[2] == 'Y')) {
		while (*l && *l != ' ')
			l++;
		while (*l == ' ')
			l++;
		c->status = (int)s_num(l);
		return c->status > 0;
	}
	return 0;
}

static void header_line(ICY *c)
{
	const char *v;
	if ((v = hdr_val(c->line, "location")))
		s_copy(c->location, sizeof(c->location), v);
	else if ((v = hdr_val(c->line, "icy-metaint")))
		c->metaint = s_num(v);
	else if ((v = hdr_val(c->line, "icy-br")))
		c->br = (int)s_num(v);
	else if ((v = hdr_val(c->line, "icy-name")))
		s_copy(c->name, sizeof(c->name), v);
	else if ((v = hdr_val(c->line, "content-type")))
		s_copy(c->ctype, sizeof(c->ctype), v);
	else if ((v = hdr_val(c->line, "transfer-encoding")) && lc((unsigned char)v[0]) == 'c')
		c->chunked = 1;		/* only HTTP/1.1 answers; we ask 1.0, but a proxy might */
}

/* StreamTitle='Artist - Title';StreamUrl='...'; */
static void meta_done(ICY *c)
{
	static const char key[] = "StreamTitle='";
	int i, k, n = c->meta_len, end = -1, t;
	char *m = c->meta;

	for (i = 0; i + (int)sizeof(key) - 1 <= n; i++) {
		for (k = 0; key[k] && m[i + k] == key[k]; k++)
			;
		if (key[k])
			continue;
		i += k;
		for (t = i; t < n && m[t]; t++)	/* the first "';" (a title may contain quotes) */
			if (m[t] == '\'' && (t + 1 >= n || m[t + 1] == ';' || m[t + 1] == 0)) {
				end = t;
				break;
			}
		if (end < 0)
			end = t;
		if (end - i > ICY_TITLE - 1)
			end = i + ICY_TITLE - 1;
		for (k = 0; k < end - i && c->title[k] == m[i + k]; k++)
			;
		if (k == end - i && c->title[k] == 0)
			return;		/* same title again */
		for (k = 0; k < end - i; k++)
			c->title[k] = m[i + k];
		c->title[k] = 0;
		c->title_seq++;
		return;
	}
}

static int end_of_headers(ICY *c)
{
	c->hdr_done = 1;
	c->until_meta = c->metaint;
	if (c->status >= 300 && c->status < 400)
		return c->location[0] ? ICY_REDIRECT : ICY_ERROR;
	if (c->status != 200)
		return ICY_ERROR;
	return ICY_BODY;
}

static void body(ICY *c, const uint8_t *p, int n, icy_sink sink, void *ctx);

int icy_feed(ICY *c, const uint8_t *p, int n, icy_sink sink, void *ctx)
{
	int i = 0, r;

	while (!c->hdr_done) {
		uint8_t b;
		if (i >= n)
			return ICY_MORE;
		b = p[i++];
		if (b == '\r')
			continue;
		if (b != '\n') {
			if (c->line_len < ICY_LINE - 1)
				c->line[c->line_len++] = (char)b;
			continue;
		}
		c->line[c->line_len] = 0;
		if (c->first) {
			c->first = 0;
			if (!status_line(c))
				return ICY_ERROR;
		} else if (c->line_len == 0) {
			r = end_of_headers(c);
			if (r != ICY_BODY)
				return r;
		} else
			header_line(c);
		c->line_len = 0;
	}
	if (c->status != 200)
		return c->location[0] && c->status >= 300 && c->status < 400 ? ICY_REDIRECT : ICY_ERROR;
	if (!c->chunked) {
		body(c, p + i, n - i, sink, ctx);
		return ICY_BODY;
	}
	while (i < n) {		/* chunked: "<hex>[;ext]\r\n" <data> "\r\n" ... */
		if (c->chunk_left) {
			int k = n - i;
			if ((uint32_t)k > c->chunk_left)
				k = (int)c->chunk_left;
			body(c, p + i, k, sink, ctx);
			c->chunk_left -= (uint32_t)k;
			i += k;
			if (!c->chunk_left)
				c->chunk_st = 2;	/* CRLF after the data */
			continue;
		}
		{
			int b = p[i++];
			if (c->chunk_st == 2) {		/* skip to the end of the CRLF */
				if (b == '\n') {
					c->chunk_st = 0;
					c->chunk_size = 0;
				}
				continue;
			}
			if (b == '\n') {
				if (c->chunk_st == 3)	/* the last chunk (0): trailers; the stream ended */
					return ICY_BODY;
				if (!c->chunk_size) {
					c->chunk_st = 3;
					continue;
				}
				c->chunk_left = c->chunk_size;
				c->chunk_st = 0;
				continue;
			}
			if (c->chunk_st == 0) {
				int d = b >= '0' && b <= '9' ? b - '0' : lc(b) >= 'a' && lc(b) <= 'f' ? lc(b) - 'a' + 10 : -1;
				if (d >= 0 && c->chunk_size < 0x1000000u)
					c->chunk_size = c->chunk_size * 16 + (uint32_t)d;
				else if (b != '\r')
					c->chunk_st = 1;	/* extension: ignored up to the LF */
			}
		}
	}
	return ICY_BODY;
}

/* body bytes (de-chunked): audio to the sink, ICY metadata blocks taken out */
static void body(ICY *c, const uint8_t *p, int n, icy_sink sink, void *ctx)
{
	int i = 0;

	while (i < n) {
		if (!c->metaint) {
			sink(ctx, p + i, n - i);
			c->audio += (uint32_t)(n - i);
			break;
		}
		if (c->until_meta) {
			int k = n - i;
			if ((uint32_t)k > c->until_meta)
				k = (int)c->until_meta;
			sink(ctx, p + i, k);
			c->audio += (uint32_t)k;
			c->until_meta -= (uint32_t)k;
			i += k;
			continue;
		}
		if (c->meta_left < 0) {		/* length byte: 16 * value bytes of text follow */
			c->meta_left = p[i++] * 16;
			c->meta_len = 0;
			if (c->meta_left == 0) {
				c->meta_left = -1;
				c->until_meta = c->metaint;
			}
			continue;
		}
		while (i < n && c->meta_left > 0) {
			if (c->meta_len < (int)sizeof(c->meta) - 1)
				c->meta[c->meta_len++] = (char)p[i];
			i++;
			c->meta_left--;
		}
		if (c->meta_left == 0) {
			c->meta[c->meta_len] = 0;
			meta_done(c);
			c->meta_left = -1;
			c->until_meta = c->metaint;
		}
	}
}

int url_parse(const char *url, char *host, int host_sz, int *port, char *path, int path_sz)
{
	static const char pre[] = "http";
	int i, h = 0, tls = 0;

	for (i = 0; pre[i]; i++)
		if (lc((unsigned char)url[i]) != pre[i])
			return 0;
	if (lc((unsigned char)url[i]) == 's') {
		tls = 1;
		i++;
	}
	if (url[i] != ':' || url[i + 1] != '/' || url[i + 2] != '/')
		return 0;
	url += i + 3;
	*port = tls ? 443 : 80;
	while (*url && *url != '/' && *url != ':' && *url != '?') {
		if (h >= host_sz - 1)
			return 0;
		host[h++] = *url++;
	}
	host[h] = 0;
	if (!h)
		return 0;
	if (*url == ':') {
		url++;
		*port = (int)s_num(url);
		while (*url >= '0' && *url <= '9')
			url++;
		if (*port <= 0 || *port > 65535)
			return 0;
	}
	if (*url == '/')
		s_copy(path, path_sz, url);
	else {
		if (path_sz < 2)
			return 0;
		path[0] = '/';
		s_copy(path + 1, path_sz - 1, url);
	}
	return 1 + tls;
}

static int put(char *out, int sz, int n, const char *s)
{
	while (*s && n < sz - 1)
		out[n++] = *s++;
	out[n] = 0;
	return n;
}

int http_request(char *out, int out_sz, const char *host, int port, const char *path, int tls)
{
	int n = 0;
	char pb[8];

	n = put(out, out_sz, n, "GET ");
	n = put(out, out_sz, n, path);
	n = put(out, out_sz, n, " HTTP/1.0\r\nHost: ");
	n = put(out, out_sz, n, host);
	if (port != (tls ? 443 : 80)) {
		int i = 7, v = port;
		pb[i--] = 0;
		do {
			pb[i--] = (char)('0' + v % 10);
			v /= 10;
		} while (v && i >= 1);
		pb[i] = ':';
		n = put(out, out_sz, n, pb + i);
	}
	n = put(out, out_sz, n, "\r\nUser-Agent: SiemensRadio/1.0\r\nAccept: */*\r\nIcy-MetaData: 1\r\n"
		"Connection: close\r\n\r\n");
	return n;
}

int url_join(char *out, int out_sz, const char *base, const char *loc)
{
	char host[96], path[8];
	int port, n, k;

	if (lc((unsigned char)loc[0]) == 'h' && (loc[4] == ':' || loc[5] == ':')) {	/* absolute */
		s_copy(out, out_sz, loc);
		return s_len(out) < out_sz - 1;
	}
	if (loc[0] != '/' || !(k = url_parse(base, host, sizeof(host), &port, path, sizeof(path))))
		return 0;
	n = put(out, out_sz, 0, k == 2 ? "https://" : "http://");
	n = put(out, out_sz, n, host);
	if (port != (k == 2 ? 443 : 80)) {
		char pb[8];
		int i = 7, v = port;
		pb[i--] = 0;
		do {
			pb[i--] = (char)('0' + v % 10);
			v /= 10;
		} while (v && i >= 1);
		pb[i] = ':';
		n = put(out, out_sz, n, pb + i);
	}
	n = put(out, out_sz, n, loc);
	return n < out_sz - 1;
}

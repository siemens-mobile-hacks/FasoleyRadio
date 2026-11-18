/*
 * Settings and stations (MMI only).
 *
 * <elf folder>\radio.cfg: "key=value" lines (older builds wrote just "fmt=N", still read):
 *   vol=3 ring=512 start=3 resume=2 retry=1 def=0 auto=0 fmt=5 (+ dbg=0: emulator builds only, RADIO_LOG)
 *   skin=0/1 (EL71 only: main screen modern / classic x65 player)
 * <elf folder>\stations.txt: "Name = URL" or just a URL per line, # = comment. Without the first line
 *   "#!stations" (a file written by hand) its stations come after the built-in ones; the elf writes the
 *   whole list with that line (built-in ones included, so they can be edited and deleted too).
 */
#include "app.h"

STATION stations[MAX_ST];
int nst;
CFG cfg;

#define LIST_MARK	"#!stations"

/* Sources: Fasoley Radio first (the default), RFI, Radio Paradise */
static const struct { const char *name, *url; } builtin[] = {
#ifdef RADIO_EMU	/* src/net_emu.c: redirects + the captured streams (no TLS in the emulator) */
	{ "Fasoley Radio", "http://localhost:18080/fasoley" },
	{ "RFI Monde 64k", "http://localhost:18080/raw64" },
	{ "Radio Paradise 32k", "http://localhost:18080/mp3-32" },
	{ "Radio Paradise 128k", "http://127.0.0.1:18080/mp3-128" },
#else
	{ "Fasoley Radio", "https://fasoley.net/radio_128.mp3" },	/* first = default (cfg.def 0), 128k: all phones (user) */
#if !defined(SGOLD) && !defined(RADIO_OBS)	/* D185: > 190k: not on EL71 and S65/SK65 (user) */
	{ "Party FM 320k", "http://stream.partyfm.dk/Party320/" },
#endif
	{ "Relax on Radio 96k", "http://streaming.radiodancefloor.it:443/" },
#if !defined(SGOLD) && !defined(RADIO_OBS)	/* D185: > 190k: not on EL71 and S65/SK65 (user) */
	{ "Kink 90's 192k", "https://25243.live.streamtheworld.com/KINK_90S_SC" },
#endif
	/* D168: top-voted MP3 stations of radio-browser.info (alondrilich.github.io/radio-playlists/top) */
	{ "MANGORADIO 128k", "https://mangoradio.stream.laut.fm/mangoradio" },
	{ "Dance Wave! 128k", "https://dancewave.online/dance.mp3" },
#ifndef SGOLD	/* D185: > 128k: not on S65/SK65 (user) */
	{ "Classic Vinyl HD 320k", "https://icecast.walmradio.com:8443/classic" },
#endif
#if !defined(SGOLD) && !defined(RADIO_OBS)	/* D185: > 190k: not on EL71 and S65/SK65 (user) */
	{ "WALM 2 HD 320k", "https://icecast.walmradio.com:8443/walm2" },
#endif
	{ "RFE/RL Radio Farda", "http://rfe21.akacast.akamaistream.net/7/751/437779/v1/ibb.akacast.akamaistream.net/rfe21" },
#if !defined(SGOLD) && !defined(RADIO_OBS)	/* D185: > 190k: not on EL71 and S65/SK65 (user) */
	{ "101 SMOOTH JAZZ 192k", "http://jking.cdnstream1.com/b22139_128mp3" },
#endif
	{ "RFI Afrique 64k", "http://live02.rfi.fr/rfiafrique-64.mp3" },
	{ "Mosaique FM 90k", "http://radio.mosaiquefm.net:8000/mosalive" },
	{ "parsa 64k", "http://parsa.icdndhcp.com:18000/stream" },
	{ "Faz 128k", "http://www.radiofaaz.com:8000/radiofaaz" },
	{ "Your Classical - Relax 128k", "http://relax.stream.publicradio.org/relax.mp3" },
	{ ".977 Country 128k", "http://15113.live.streamtheworld.com:3690/977_COUNTRY_SC" },
	{ "RFI-Afrique 96k", "http://live02.rfi.fr/rfiafrique-96k.mp3" },
	{ "Радио Русские Песни 128k", "http://listen.rusongs.ru/ru-mp3-128" },
	{ ".977 80s 128k", "http://29318.live.streamtheworld.com/977_80_SC" },
	{ ".977 Hitz 128k", "http://14123.live.streamtheworld.com/977_HITS_SC" },
	{ "Dance Wave Retro! 128k", "https://dancewave.online/retrodance.mp3" },
#if !defined(SGOLD) && !defined(RADIO_OBS)	/* D185: > 190k: not on EL71 and S65/SK65 (user) */
	{ "Ambient Sleeping Pill 256k", "http://radio.stereoscenic.com/asp-h" },
#endif
	{ "Frisky 96k", "http://stream2.friskyradio.com/frisky_mp3_hi" },
	{ "Шансон Радио", "http://chanson.hostingradio.ru:8041/chanson128.mp3" },
	{ "Дорожное радио", "http://dorognoe.hostingradio.ru:8000/dorognoe" },
	{ "LBC Radio 48k", "http://media-sov.musicradio.com/LBC973MP3Low" },
#if !defined(SGOLD) && !defined(RADIO_OBS)	/* D185: > 190k: not on EL71 and S65/SK65 (user) */
	{ "Вести FM 320k", "http://icecast.vgtrk.cdnvideo.ru/vestifm" },
#endif
	{ "Европа плюс", "http://ep128server.streamr.ru:8030/ep128" },
#if !defined(SGOLD) && !defined(RADIO_OBS)	/* D185: > 190k: not on EL71 and S65/SK65 (user) */
	{ "Intense Radio Dance 256k", "https://secure.live-streams.nl/main" },
#endif
	{ "Radio Sarcheshme 96k", "http://sarcheshmeh.icdndhcp.com:18452/stream" },
	{ "Radio Caroline 128k", "http://78.129.202.200:8040/;" },
	{ "SomaFM Groove Salad 128k", "https://ice5.somafm.com/groovesalad-128-mp3" },
#endif
};
#define NBUILTIN	((int)(sizeof(builtin) / sizeof(builtin[0])))

static void path(char *p, const char *name)
{
	sprintf(p, "%s\\%s", elf_dir, name);
}

/* whole small file into a malloc'ed, 0-terminated buffer; 0 = none */
static char *read_file(const char *name, int max, int *len)
{
	static char p[160];
	unsigned err;
	char *buf;
	int h, n;

	path(p, name);
	h = _open(p, A_ReadOnly + A_BIN, P_READ, &err);
	if (h < 0)
		return 0;
	buf = malloc(max + 1);
	n = buf ? _read(h, buf, max, &err) : 0;
	_close(h, &err);
	if (n <= 0) {
		if (buf)
			mfree(buf);
		return 0;
	}
	buf[n] = 0;
	*len = n;
	return buf;
}

static int write_file(const char *name, const char *b, int n)
{
	static char p[160];
	unsigned err;
	int h, w;

	path(p, name);
	h = _open(p, A_ReadWrite + A_BIN + A_Create + A_Truncate, P_READ + P_WRITE, &err);
	if (h < 0)
		return 0;
	w = _write(h, (void *)b, n, &err);
	_close(h, &err);
	return w == n;
}

/* next line of buf (\n or \r\n ended), 0-terminated in place, leading blanks and a UTF-8 BOM skipped */
static char *next_line(char *buf, int n, int *i)
{
	char *l, *e;
	if (*i >= n)
		return 0;
	l = buf + *i;
	while (*i < n && buf[*i] != '\n')
		(*i)++;
	buf[*i] = 0;
	(*i)++;
	if ((e = strchr(l, '\r')))
		*e = 0;
	while (*l == ' ' || *l == '\t' || (uint8_t)*l == 0xEF || (uint8_t)*l == 0xBB || (uint8_t)*l == 0xBF)
		l++;
	return l;
}

/* ------------------------------------------------------------------ radio.cfg */

static int num(const char *s)
{
	int v = 0, neg = *s == '-';
	if (neg)
		s++;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + *s++ - '0';
	return neg ? -v : v;
}

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

void cfg_load(void)
{
	char *buf, *l;
	int n, i = 0, k;

	cfg.vol = be_vol_def;
	cfg.ring_kb = be_ring_def;
	cfg.start_sec = 3;
	cfg.resume_sec = 2;
	cfg.retry = 1;
#ifdef RADIO_LOG_ALWAYS
	cfg.log = 1;			/* FasoleyRadio_ELKA_dbg.elf */
#elif defined(RADIO_LOG)
	cfg.log = 0;			/* key "dbg" (emulator builds only; phone builds ignore it); old files' log=1 is ignored */
#endif
	cfg.def = 0;
	cfg.autoplay = 0;
	cfg.fmt = -1;			/* the phone's default (be_setup) */
	cfg.card = 0;
#ifdef RADIO_SKIN
	cfg.skin = 0;		/* Modern by default (user) */
#endif
	if (!(buf = read_file("radio.cfg", 1024, &n)))
		return;
	while ((l = next_line(buf, n, &i))) {
		char *v = strchr(l, '=');
		if (!v)
			continue;
		*v++ = 0;
		if (!strcmp(l, "vol"))
			cfg.vol = clamp(num(v), VOL_MIN, VOL_MAX);
		else if (!strcmp(l, "ring")) {
			cfg.ring_kb = num(v);
			for (k = 0; k < be_nring && be_ring_kb[k] != cfg.ring_kb; k++)
				;
			if (k == be_nring)
				cfg.ring_kb = be_ring_def;
		} else if (!strcmp(l, "start"))
			cfg.start_sec = clamp(num(v), 1, 30);
		else if (!strcmp(l, "resume"))
			cfg.resume_sec = clamp(num(v), 1, 30);
		else if (!strcmp(l, "retry"))
			cfg.retry = num(v) != 0;
#ifdef RADIO_LOG_CFG
		else if (!strcmp(l, "dbg"))
			cfg.log = num(v) != 0;
#endif
		else if (!strcmp(l, "def"))
			cfg.def = num(v);
		else if (!strcmp(l, "auto"))
			cfg.autoplay = num(v) != 0;
		else if (!strcmp(l, "card"))
			cfg.card = num(v) != 0;
#ifdef RADIO_SKIN
		else if (!strcmp(l, "skin"))
			cfg.skin = num(v) != 0;
#endif
		else if (!strcmp(l, "fmt") && *v >= '0' && *v <= '9')
			cfg.fmt = num(v);
	}
	mfree(buf);
}

void cfg_save(void)
{
	static char b[200];
	int n = sprintf(b, "vol=%d\r\nring=%d\r\nstart=%d\r\nresume=%d\r\nretry=%d\r\ndef=%d\r\nauto=%d\r\n",
			cfg.vol, cfg.ring_kb, cfg.start_sec, cfg.resume_sec, cfg.retry, cfg.def, cfg.autoplay);
#ifdef RADIO_LOG_CFG
	n += sprintf(b + n, "dbg=%d\r\n", cfg.log);
#endif
	if (cfg.fmt >= 0)
		n += sprintf(b + n, "fmt=%d\r\n", cfg.fmt);
	if (cfg.card)
		n += sprintf(b + n, "card=1\r\n");
#ifdef RADIO_SKIN
	n += sprintf(b + n, "skin=%d\r\n", cfg.skin);
#endif
	write_file("radio.cfg", b, n);
}

/* ------------------------------------------------------------------ stations */

static void host_name(char *name, const char *url)
{
	const char *hs = strstr(url, "//");
	int k;
	hs = hs ? hs + 2 : url;
	for (k = 0; hs[k] && hs[k] != '/' && hs[k] != ':' && k < ST_NAME - 1; k++)
		name[k] = hs[k];
	name[k] = 0;
}

int station_set(int i, const char *name, const char *url)
{
	int k;
	if (i < 0 || i > nst || i >= MAX_ST)
		return 0;
	while (*url == ' ')
		url++;
	for (k = 0; url[k] && url[k] != ' ' && url[k] != '\t' && k < ST_URL - 1; k++)
		stations[i].url[k] = url[k];
	stations[i].url[k] = 0;
	while (*name == ' ')
		name++;
	if (*name) {
		uint8_t *b = (uint8_t *)stations[i].name;
		int j;
		strncpy(stations[i].name, name, ST_NAME - 1);
		stations[i].name[ST_NAME - 1] = 0;
		k = strlen(stations[i].name);
		if ((int)strlen(name) > k) {	/* cut by the limit: not inside a UTF-8 sequence */
			for (j = k; j > 0 && (b[j - 1] & 0xC0) == 0x80; j--)
				;
			if (j > 0 && b[j - 1] >= 0xC0 && k - (j - 1) < (b[j - 1] >= 0xE0 ? 3 : 2))
				b[j - 1] = 0;
		}
		for (k = strlen(stations[i].name); k > 0 && stations[i].name[k - 1] == ' '; k--)
			stations[i].name[k - 1] = 0;
	} else
		host_name(stations[i].name, stations[i].url);
	if (i == nst)
		nst++;
	return 1;
}

void station_del(int i)
{
	if (i < 0 || i >= nst)
		return;
	memmove(&stations[i], &stations[i + 1], (nst - i - 1) * sizeof(STATION));
	nst--;
	if (cfg.def == i)
		cfg.def = 0;
	else if (cfg.def > i)
		cfg.def--;
}

void stations_load(void)
{
	char *buf, *l;
	int n, i = 0, full = 0;

	nst = 0;
	buf = read_file("stations.txt", 16384, &n);
	if (buf) {
		char *s = buf;
		while ((uint8_t)*s == 0xEF || (uint8_t)*s == 0xBB || (uint8_t)*s == 0xBF)
			s++;
		full = !strncmp(s, LIST_MARK, sizeof(LIST_MARK) - 1);
	}
	if (!full)
		for (nst = 0; nst < NBUILTIN; nst++) {
			strcpy(stations[nst].name, builtin[nst].name);
			strcpy(stations[nst].url, builtin[nst].url);
		}
	if (!buf)
		return;
	while (nst < MAX_ST && (l = next_line(buf, n, &i))) {
		char *u, *q;
		if (!*l || *l == '#')
			continue;
		u = strstr(l, "= http");	/* "Name = URL" (a name may hold "http" itself) */
		u = u ? u + 2 : strstr(l, "http");
		if (!u)
			continue;
		q = u;			/* the name ends before " =" */
		while (q > l && (q[-1] == ' ' || q[-1] == '=' || q[-1] == '\t'))
			q--;
		*q = 0;
		station_set(nst, l, u);
	}
	mfree(buf);
	if (!nst)		/* everything deleted by hand: the built-in ones again */
		for (nst = 0; nst < NBUILTIN; nst++) {
			strcpy(stations[nst].name, builtin[nst].name);
			strcpy(stations[nst].url, builtin[nst].url);
		}
}

int stations_save(void)
{
	char *b = malloc(MAX_ST * (ST_NAME + ST_URL + 8) + 64);
	int n, i, ok;

	if (!b)
		return 0;
	n = sprintf(b, LIST_MARK " (Fasoley Radio): Name = URL\r\n");
	for (i = 0; i < nst; i++)
		n += sprintf(b + n, "%s = %s\r\n", stations[i].name, stations[i].url);
	ok = write_file("stations.txt", b, n);
	mfree(b);
	return ok;
}

/* ------------------------------------------------------------------ text */

void text_ws(WSHDR *ws, const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	int utf8 = 1, i;

	for (i = 0; p[i]; i++) {	/* valid UTF-8? */
		int n = p[i] < 0x80 ? 0 : (p[i] & 0xE0) == 0xC0 ? 1 : (p[i] & 0xF0) == 0xE0 ? 2 : -1, k;
		if (n < 0) {
			utf8 = 0;
			break;
		}
		for (k = 1; k <= n; k++)
			if ((p[i + k] & 0xC0) != 0x80) {
				utf8 = 0;
				break;
			}
		if (!utf8)
			break;
		i += n;
	}
	if (!utf8) {
		WSHDR *t = AllocWS(strlen(s) + 8);
		str_2ws(t, s, strlen(s));	/* the firmware's 8-bit table */
		wstrcat(ws, t);
		FreeWS(t);
		return;
	}
	while (*p) {
		int c = *p++;
		if (c >= 0xE0) {
			c = (c & 0x0F) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F);
			p += 2;
		} else if (c >= 0xC0) {
			c = (c & 0x1F) << 6 | (p[0] & 0x3F);
			p++;
		}
		wsAppendChar(ws, c);
	}
}

void ws_utf8(const WSHDR *ws, char *out, int n)
{
	int i, k = 0, len = ws ? ws->wsbody[0] : 0;
	for (i = 1; i <= len; i++) {
		unsigned c = ws->wsbody[i];
		if (c >= 0xE000 && c < 0xF900)	/* the firmware's control characters */
			continue;
		if (c < 0x80) {
			if (k + 1 >= n)
				break;
			out[k++] = (char)c;
		} else if (c < 0x800) {
			if (k + 2 >= n)
				break;
			out[k++] = (char)(0xC0 | c >> 6);
			out[k++] = (char)(0x80 | (c & 0x3F));
		} else {
			if (k + 3 >= n)
				break;
			out[k++] = (char)(0xE0 | c >> 12);
			out[k++] = (char)(0x80 | (c >> 6 & 0x3F));
			out[k++] = (char)(0x80 | (c & 0x3F));
		}
	}
	out[k] = 0;
}

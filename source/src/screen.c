/*
 * Main screen: own GUI (CreateGUI, as MC / tgram), drawn only in onRedraw (MMI). Layout from the screen size
 * and the font heights (ELKA 240x320 under the icon bar, SG 132x176 "compact"):
 *
 *   header       Fasoley Radio  D73                 2/6
 *   station   <  Radio Paradise 128k                  >
 *                (the stream's own name)
 *   track     [ Artist - Title, scrolling if long      ]
 *   state     (Играет)                         ||||| volume
 *             detail: problem / gaps / hint
 *   buffer    Буфер                        4.2 с / 512К
 *             [=========|----------]  (| = start level)
 *   info      Поток 128 кбит/с, Формат 44.1 кГц стерео, Сеть 15.6 КБ/с https
 *   softkeys  Меню              (>)/[]           Свернуть
 *
 * Redraws: screen_tick (0.1 s MMI timer) redraws our GUI (DirectRedrawGUI_ID, never REDRAW) only when
 * something shown changed and at most every 0.3 s on ELKA / 0.5 s on SG (the decode there is CPU-bound);
 * nothing while another GUI is on top. Keys redraw at once.
 *
 * Keys: centre/green play-stop, left/right station (switches at once while playing), up/down * # volume keys
 * volume, 1-9 station n, 0 stations, left soft the menu (menus.c), right soft/red minimize.
 *
 * EL71 (RADIO_SKIN) "classic" (Victor89) skin (cfg.skin = 1, Settings > Skin), in the spirit of the x65 media player, all
 * drawn here (no firmware bitmaps): as the x65 player (reference picture): blue with scanlines, cyan lines, title bar
 * (film strip + note + scrolling title),
 * the reels = frames of the user's GIF (tools/mkreels.py -> reels_data.h, 8 frames 210x90, one unpacked at a time
 * into a 75 KB ARGB8888 image) that turn while playing (~4 fps: only playing, on top and with the display lit; frame
 * 0 still otherwise), one bar = the network buffer, big "m:ss / 0:07". Soft keys: left Стоп/Играть (=
 * centre), right Опции (the menu); red minimizes; the rest as in the modern skin. Full screen (no icon bar).
 * Menu name "Виктор 89".
 * EL71: no redraws at all while the display light is off (GetIlluminationDataTable()[3], display level).
 */
#include "app.h"
#include "build_id.h"
#ifdef RADIO_SKIN
#include "reels_data.h"
#endif

#define RGB(r, g, b)	{ r, g, b, 100 }
static const char C_BG[4] = RGB(0x10, 0x17, 0x20), C_PANEL[4] = RGB(0x1B, 0x28, 0x36);
static const char C_EDGE[4] = RGB(0x2F, 0x45, 0x5A), C_ACC[4] = RGB(0xFF, 0x8A, 0x1E);
static const char C_TXT[4] = RGB(0xFF, 0xFF, 0xFF), C_SUB[4] = RGB(0xA8, 0xBA, 0xCC);
static const char C_DIM[4] = RGB(0x66, 0x7B, 0x90), C_PLAY[4] = RGB(0x35, 0xC7, 0x67);
static const char C_BUF[4] = RGB(0xF2, 0xB3, 0x3D), C_ERR[4] = RGB(0xE5, 0x48, 0x4D);
static const char C_STOP[4] = RGB(0x6F, 0x80, 0x91), C_BARBG[4] = RGB(0x08, 0x0D, 0x12);
static const char C_DARK[4] = RGB(0x10, 0x17, 0x20), C_NONE[4] = { 0, 0, 0, 0 };

typedef struct { GUI gui; } SCR_GUI;
static int gui_id;
static RECT canvas;
static WSHDR *ws, *ws_title;
int screen_st;

static int W, H, Y0, compact, pad;
static int f_big, f_mid, f_small, f_bold, h_big, h_mid, h_small;
static int period;		/* min ticks between redraws */

static void metrics(void)
{
	W = ScreenW();
	H = ScreenH();
	Y0 = YDISP;
	compact = H - Y0 < 250;
	pad = compact ? 3 : 6;
	f_big = FONT_MEDIUM_BOLD;
	f_mid = compact ? FONT_SMALL : FONT_MEDIUM;
	f_small = FONT_SMALL;
	f_bold = FONT_SMALL_BOLD;
	h_big = GetFontYSIZE(f_big);
	h_mid = GetFontYSIZE(f_mid);
	h_small = GetFontYSIZE(f_small);
	period = compact ? 5 : 3;
}

/* ------------------------------------------------------------------ what is shown */

static int title_seq = -1, title_st = -2, title_w;
static int xdisp, xdir, xwait;

/* the track row's text: the stream's title, else its name; stopped: the station's address */
static void make_title(void)
{
	CutWSTR(ws_title, 0);
#ifdef RADIO_SKIN
	if (cfg.skin) {		/* the title bar: StreamTitle, else the station's name */
		int st = station >= 0 ? station : screen_st;
		if (station >= 0 && radio.icy.title[0])
			text_ws(ws_title, radio.icy.title);
		else if (st < nst)
			text_ws(ws_title, stations[st].name);
	} else
#endif
	if (station >= 0 && radio.icy.title[0])
		text_ws(ws_title, radio.icy.title);
	else if (station >= 0 && radio.icy.name[0])
		text_ws(ws_title, radio.icy.name);
	else if (station >= 0)
		wsprintf(ws_title, "%t", "No track title");
	else if (screen_st < nst)
		text_ws(ws_title, stations[screen_st].url);
	title_w = Get_WS_width(ws_title, f_mid);
	xdisp = 0;
	xdir = 1;
	xwait = 4;
}

#ifdef RADIO_SKIN
static int cl_title_x(void)
{
	return 46;	/* after the film strip and note icons */
}
#endif

static int title_box_w(void)
{
#ifdef RADIO_SKIN
	if (cfg.skin)
		return W - cl_title_x() - pad;
#endif
	return W - 4 * pad - 2;
}

/* MC-like ping-pong scroll of a long title: wait, move, wait, move back */
static void scroll_step(void)
{
	int over = title_w - title_box_w(), step = compact ? 6 : 10;
	if (title_seq != radio.icy.title_seq || title_st != (station >= 0 ? station : -1 - screen_st)) {
		title_seq = radio.icy.title_seq;
		title_st = station >= 0 ? station : -1 - screen_st;
		make_title();
		return;
	}
	if (over <= 0)
		return;
	if (xwait > 0) {
		xwait--;
		return;
	}
	xdisp += xdir * step;
	if (xdisp >= over) {
		xdisp = over;
		xdir = -1;
		xwait = 4;
	} else if (xdisp <= 0) {
		xdisp = 0;
		xdir = 1;
		xwait = 6;
	}
}

/* what the network ring holds for the station on (stopped: the old bytes do not count) */
static uint32_t used_b(void)
{
	return station >= 0 && !open_pending() ? ring_used(&net_ring) : 0;
}

static int buffered_ms(void)
{
	return (int)((uint64_t)used_b() * 1000 / stream_bps());
}

/* the state pill and the line below it */
static const char *state_text(char *b, const char **col, char *detail)
{
	int rs = radio.state;
	*col = C_STOP;
	detail[0] = 0;
	if (!app_started) {
		*col = C_BUF;
		return "Starting...";
	}
#ifdef RADIO_LOG
	if (be_test_on()) {
		*col = C_BUF;
		be_test_line(detail);
		return "Sound test";
	}
#endif
	switch (pstate) {
	case P_IDLE:
		if (note) {
			*col = C_ERR;
			strcpy(detail, note);
			return "Error";
		}
		if (!fw) {
			*col = C_ERR;
			strcpy(detail, vs_fail ? vs_fail : be_msg(BM_NO_SOUND));
			return "No sound";
		}
		sprintf(detail, compact ? "Centre: play, < > stations" : "Centre: play,  < >  stations,  0: list");
		return "Stopped";
	case P_WAIT_OLD:
		return "Stopping...";
	case P_BUFFER:
		*col = C_BUF;
		if (rs == RS_RETRY) {
			strcpy(detail, radio.err ? radio.err : "");
			return "Retrying...";
		}
		if (rs == RS_CONNECT || open_pending())
			return "Connecting...";
		if (rs == RS_HEADERS)
			return "Waiting for reply...";
		if (start_need)
			sprintf(b, "Buffering %d%%", (int)((uint64_t)ring_used(&net_ring) * 100 / start_need));
		else
			strcpy(b, "Buffering");
		return b;
	case P_PLAY:
		if (rs == RS_RETRY || rs == RS_CONNECT || rs == RS_HEADERS) {
			*col = C_BUF;
			strcpy(detail, rs == RS_RETRY && radio.err ? radio.err : "");
			return "Reconnecting";
		}
		if (dec->waiting) {
			*col = C_BUF;
			strcpy(detail, "Buffer empty: refilling");
			return "Paused";
		}
		*col = C_PLAY;
		if (rx_secs >= 4 && rx_bps * 10 < stream_bps() * 9)
			sprintf(detail, "Net %d.%dK/s, slower than stream", (int)(rx_bps / 1024), (int)(rx_bps % 1024 * 10 / 1024));
		else if (dec->gaps) {
			uint32_t pbps = be_pcm_bps(), gms = pbps ? (uint32_t)((uint64_t)dec->gap_bytes * 1000 / pbps) : 0;
			sprintf(detail, "Gaps %d (%d.%d s)", (int)dec->gaps, (int)(gms / 1000), (int)(gms % 1000 / 100));
		}
		if (radio.reconnects)
			sprintf(detail + strlen(detail), "%sreconnects %d", detail[0] ? ", " : "", radio.reconnects);
		if (play_t0) {	/* how long this station plays (reconnects and refills included) */
			uint32_t t = (radio_ms() - play_t0) / 1000;
			if (t >= 3600)
				sprintf(b, "Playing %u:%02u:%02u", (unsigned)(t / 3600), (unsigned)(t / 60 % 60), (unsigned)(t % 60));
			else
				sprintf(b, "Playing %u:%02u", (unsigned)(t / 60), (unsigned)(t % 60));
			return b;
		}
		return "Playing";
	case P_CALL:
		*col = C_BUF;
		return "Paused: call";
	}
	return "";
}

#ifdef RADIO_SKIN
/* EL71: the display's light level now (0 = off, sleeping); the emulator has no table */
static int display_lit(void)
{
	const char *t = GetIlluminationDataTable();	/* A8D8726C: 0x38 per device, display first, +3 = level */
	return !t || t[3] != 0;
}

/* classic skin: the reels turn (playing, sound going) */
static int reels_turn(void)
{
	return cfg.skin && pstate == P_PLAY && !dec->waiting;
}

/* the reels' frame: a new one every REELS_MS (~250 ms, the GIF's speed) while they turn, else 0 */
static int reel_phase(void)
{
	return reels_turn() ? (int)(radio_ms() / REELS_MS % REELS_N) : 0;
}
#endif

/* hash of what is drawn (quantized): redraw only when it changes */
#define MIX(s, v)	((s) = (s) * 31u + (unsigned)(v))
static unsigned signature(void)
{
	unsigned s = 0;
	char b[48], d[96];
	const char *c, *t = state_text(b, &c, d), *p;
	for (p = t; *p; p++)
		MIX(s, *p);
	for (p = d; *p; p++)
		MIX(s, *p);
	MIX(s, (uintptr_t)c);
	MIX(s, screen_st);
	MIX(s, station);
	MIX(s, nst);
	MIX(s, volume);
	MIX(s, kbps);
	MIX(s, rate);
	MIX(s, chans);
	MIX(s, radio.tls);
	MIX(s, rx_bps / 100);
	MIX(s, buffered_ms() / (compact ? 500 : 100));
	MIX(s, (uint64_t)used_b() * (W - 2 * pad) / ring_size(&net_ring));
	MIX(s, start_need);
	MIX(s, title_seq);
	MIX(s, title_st);
	MIX(s, xdisp);
	MIX(s, radio.icy.name[0]);
#ifdef RADIO_SKIN
	MIX(s, cfg.skin);
	MIX(s, reel_phase());
#endif
	return s;
}

/* ------------------------------------------------------------------ drawing */

static void fill(int x1, int y1, int x2, int y2, const char *c)
{
	DrawRectangle(x1, y1, x2, y2, 0, c, c);
}

/* cp1251 text in [x1, x2] */
static void text(const char *s, int x1, int y, int x2, int h, int font, int align, const char *c)
{
	wsprintf(ws, "%t", s);
	DrawString(ws, x1, y, x2, y + h, font, align, c, C_NONE);
}

static int text_w(const char *s, int font)
{
	wsprintf(ws, "%t", s);
	return Get_WS_width(ws, font);
}

static void tri(int x1, int y1, int x2, int y2, int x3, int y3, const char *c)
{
	DrawTriangle(x1, y1, x2, y2, x3, y3, 0, c, c);
}

/* label left, value right */
static void info_row(int y, const char *label, const char *value, const char *vc)
{
	text(label, pad * 2, y, W / 2, h_small, f_small, TEXT_ALIGNLEFT, C_DIM);
	text(value, W / 3, y, W - pad * 2, h_small, f_small, TEXT_ALIGNRIGHT, vc);
}

static void fmt_kbs(char *b, uint32_t bps)
{
	sprintf(b, "%d.%d KB/s", (int)(bps / 1024), (int)(bps % 1024 * 10 / 1024));
}

/* ------------------------------------------------------------------ classic skin (EL71) */
#ifdef RADIO_SKIN
/* as the x65 player (reference picture): blue with scanlines, light cyan lines and bar, white text */
static const char K_BG[4] = RGB(0x24, 0x52, 0xA6), K_SCAN[4] = RGB(0x1C, 0x44, 0x8E);
static const char K_NAVY[4] = RGB(0x1C, 0x40, 0x88), K_LINE[4] = RGB(0x48, 0xCC, 0xF2);
static const char K_BARF[4] = RGB(0xA8, 0xF2, 0xFF), K_BARE[4] = RGB(0x2E, 0x6C, 0xC0);
static const char K_BARK[4] = RGB(0x3A, 0x8C, 0xE0), K_MARK[4] = RGB(0x14, 0x3C, 0x90);
static const char K_NOTE[4] = RGB(0x5C, 0xE0, 0x4A), K_BLACK[4] = RGB(0x08, 0x10, 0x20);

/* body: blue, every other row a little darker */
static void scan_bg(int y1, int y2)
{
	int y;
	fill(0, y1, W - 1, y2, K_BG);
	for (y = y1 + 1; y <= y2; y += 2)
		fill(0, y, W - 1, y, K_SCAN);
}

/* the reels: frames of the user's GIF (tools/mkreels.py), the one shown unpacked into an ARGB8888 image */
static IMGHDR reels_img;
static int reels_frame = -1;

static void reels_free(void)
{
	if (reels_img.bitmap)
		mfree(reels_img.bitmap);
	reels_img.bitmap = 0;
	reels_frame = -1;
}

static int reels_unpack(int f)
{
	const uint8_t *p = reels_rle + reels_off[f], *e = reels_rle + reels_off[f + 1];
	uint32_t *o, *oe;
	if (f == reels_frame)
		return 1;
	if (!reels_img.bitmap && !(reels_img.bitmap = malloc(REELS_W * REELS_H * 4)))
		return 0;
	o = (uint32_t *)reels_img.bitmap;
	oe = o + REELS_W * REELS_H;
	while (p < e && o < oe) {
		int t = *p++;
		if (t < 0x80) {
			uint32_t c = reels_pal[*p++];
			for (t++; t-- && o < oe;)
				*o++ = c;
		} else
			for (t -= 0x7F; t-- && o < oe;)
				*o++ = reels_pal[*p++];
	}
	reels_img.w = REELS_W;
	reels_img.h = REELS_H;
	reels_img.bpnum = IMGHDR_TYPE_ARGB8888;
	reels_frame = f;
	return 1;
}

/* a beamed note (green, dark outline) in [x, x + w] x [y, y + h] */
static void note_icon(int x, int y, int w, int h, const char *c)
{
	int r = w / 4 + 1, hy = y + h - r - 1, s1 = x + 2 * r - 1, s2 = x + w - 1, k;
	for (k = 1; k >= 0; k--) {	/* outline, then the colour */
		const char *cc = k ? K_BLACK : c;
		DrawRoundedFrame(x - k, hy - r - k, x + 2 * r - 1 + k, hy + r + k, r, r, 0, cc, cc);
		DrawRoundedFrame(s2 - 2 * r + 1 - k, hy - r - 2 - k, s2 + k, hy + r - 2 + k, r, r, 0, cc, cc);
		fill(s1 - 1 - k, y + 2 - k, s1 + k, hy, cc);
		fill(s2 - 1 - k, y - k, s2 + k, hy - 2, cc);
		fill(s1 - 1 - k, y + 1 - k, s2 + k, y + 3 + k, cc);
	}
}

/* a film strip: white band with dark holes, top and bottom */
static void film_icon(int x, int y, int w, int h)
{
	int i;
	fill(x, y, x + w - 1, y + h - 1, K_BLACK);
	fill(x + 1, y + 1, x + w - 2, y + h - 2, C_TXT);
	fill(x + 1, y + h / 2 - 1, x + w - 2, y + h / 2, K_BLACK);
	fill(x + w / 2, y + 3, x + w / 2, y + h - 4, K_BLACK);
	for (i = x + 2; i < x + w - 3; i += 4) {
		fill(i, y + 2, i + 1, y + 3, K_BLACK);
		fill(i, y + h - 4, i + 1, y + h - 3, K_BLACK);
	}
}

/* ws = these Unicode code points (string literals here are cp1251) */
static void ws_chars(const unsigned short *c)
{
	CutWSTR(ws, 0);
	while (*c)
		wsAppendChar(ws, *c++);
}

/* Victor89 skin: title bar, reels, one bar (buffer), big time, soft keys - nothing else */
static void classic_draw(void)
{
	char v[32];
	int f_t = FONT_MEDIUM_BOLD, h_t = GetFontYSIZE(f_t), f_time = FONT_LARGE_BOLD, h_time = GetFontYSIZE(f_time);
	int th = h_t + 14, sk_h = h_t + 14, top = th + 2, bottom = H - sk_h - 2, area = bottom - top, x;
	int bh = 26, bx1 = W / 8, bx2 = W - 1 - W / 8, by;
	uint32_t t = play_t0 && station >= 0 ? (radio_ms() - play_t0) / 1000 : 0;

	/* bands, separator lines, the frame round everything */
	fill(0, 0, W - 1, th - 1, K_NAVY);
	fill(0, th, W - 1, th + 1, K_LINE);
	scan_bg(top, bottom - 1);
	fill(0, bottom, W - 1, bottom + 1, K_LINE);
	fill(0, bottom + 2, W - 1, H - 1, K_NAVY);
	fill(0, 0, 1, H - 1, K_LINE);
	fill(W - 2, 0, W - 1, H - 1, K_LINE);
	fill(0, H - 2, W - 1, H - 1, K_LINE);

	/* title: film strip, note, the title */
	film_icon(6, (th - 14) / 2, 20, 14);
	note_icon(29, (th - 16) / 2, 12, 16, K_NOTE);
	x = cl_title_x();
	if (title_w > title_box_w())
		DrawScrollString(ws_title, x, (th - h_t) / 2, x + title_box_w(), (th + h_t) / 2, xdisp, f_t,
				 TEXT_ALIGNLEFT, C_TXT, C_NONE);
	else
		DrawString(ws_title, x, (th - h_t) / 2, x + title_box_w(), (th + h_t) / 2, f_t, TEXT_ALIGNLEFT,
			   C_TXT, C_NONE);

	/* the reels: centre at ~35% of the body */
	if (reels_unpack(reel_phase()))
		DrawIMGHDR((W - REELS_W) / 2, top + area * 35 / 100 - REELS_H / 2, &reels_img);

	/* the bar (network buffer): light cyan fill, blue frame, dark left end mark */
	by = top + area * 64 / 100 - bh / 2;
	{
		int rs = ring_size(&net_ring), fx = bx1 + 2 + (int)((uint64_t)used_b() * (bx2 - bx1 - 4) / rs);
		fill(bx1, by, bx2, by + bh - 1, K_BARK);
		fill(bx1 + 2, by + 2, bx2 - 2, by + bh - 3, K_BARE);
		if (fx > bx1 + 2)
			fill(bx1 + 2, by + 2, fx, by + bh - 3, K_BARF);
		fill(bx1 + 2, by + 2, bx1 + 4, by + bh - 3, K_MARK);
	}

	/* time: elapsed / 0:07 (the user's constant); the slash drawn (not every big font has one) */
	if (t >= 3600)
		sprintf(v, "%u:%02u:%02u", (unsigned)(t / 3600), (unsigned)(t / 60 % 60), (unsigned)(t % 60));
	else
		sprintf(v, "%u:%02u", (unsigned)(t / 60), (unsigned)(t % 60));
	by += bh;
	{
		int wl = text_w(v, f_time), wr = text_w("0:07", f_time), sw = h_time / 3 + 14, tw = wl + sw + wr;
		int ty = by + (bottom - by - h_time) / 2, tx = (W - tw) / 2, k;
		if (tw > W - 8) {	/* h:mm:ss: elapsed only */
			text(v, 0, ty, W - 1, h_time, f_time, TEXT_ALIGNMIDDLE, C_TXT);
		} else {
			text(v, tx, ty, tx + wl + 2, h_time, f_time, TEXT_ALIGNLEFT, C_TXT);
			text("0:07", tx + wl + sw, ty, W - 1, h_time, f_time, TEXT_ALIGNLEFT, C_TXT);
			for (k = 0; k < 3; k++)
				DrawLine(tx + wl + sw / 2 - h_time / 6 + k - 1, ty + h_time * 5 / 6, tx + wl + sw / 2 + h_time / 6 + k - 1,
					 ty + h_time / 6, 0, C_TXT);
		}
	}

	/* soft keys (Victor89 only): left Стоп / Играть, right Опции (the menu); centre: pause / play sign */
	{
		static const unsigned short stop[] = { 0x421, 0x442, 0x43E, 0x43F, 0 }, play[] = { 0x418, 0x433, 0x440, 0x430,
			0x442, 0x44C, 0 }, opts[] = { 0x41E, 0x43F, 0x446, 0x438, 0x438, 0 };
		int cx = W / 2, cy = bottom + 2 + sk_h / 2, r = h_t / 3, ty = bottom + 2 + (sk_h - h_t) / 2;
		ws_chars(station >= 0 ? stop : play);
		DrawString(ws, 8, ty, cx - r - 4, ty + h_t, f_t, TEXT_ALIGNLEFT, C_TXT, C_NONE);
		ws_chars(opts);
		DrawString(ws, cx + r + 4, ty, W - 9, ty + h_t, f_t, TEXT_ALIGNRIGHT, C_TXT, C_NONE);
		if (station >= 0) {
			fill(cx - r, cy - r, cx - r / 3 - 1, cy + r, C_TXT);
			fill(cx + r / 3 + 1, cy - r, cx + r, cy + r, C_TXT);
		} else
			tri(cx - r + 1, cy - r, cx - r + 1, cy + r, cx + r, cy, C_TXT);
	}
}
#endif

static void OnRedraw(GUI *g)
{
	char b[96], d[96], v[64];
	const char *col, *st;
	int y = Y0, x, i, h, sk_h = h_small + (compact ? 3 : 8), bottom = H - sk_h;
	int rows_h, gap, info_n = 2, show_icy;
	(void)g;

	if (title_seq != radio.icy.title_seq || title_st != (station >= 0 ? station : -1 - screen_st)) {
		title_seq = radio.icy.title_seq;
		title_st = station >= 0 ? station : -1 - screen_st;
		make_title();
	}
#ifdef RADIO_SKIN
	if (cfg.skin) {
		classic_draw();
		return;
	}
	reels_free();		/* modern again: the frame buffer back */
#endif
	fill(0, Y0, W - 1, H - 1, C_BG);

	/* header */
	h = h_small + (compact ? 2 : 6);
	fill(0, y, W - 1, y + h - 1, C_PANEL);
	fill(0, y + h, W - 1, y + h + (compact ? 0 : 1), C_ACC);
	text(APP_NAME, pad, y + (h - h_small) / 2, W - 1, h_small, f_bold, TEXT_ALIGNLEFT, C_ACC);
	if (!compact)
		text(RADIO_BUILD_TAG, pad + text_w(APP_NAME, f_bold) + pad, y + (h - h_small) / 2, W - 1, h_small, f_small,
		     TEXT_ALIGNLEFT, C_DIM);
	sprintf(b, "%d/%d", screen_st + 1, nst);
	text(b, 0, y + (h - h_small) / 2, W - pad - 1, h_small, f_small, TEXT_ALIGNRIGHT, C_SUB);
	y += h + (compact ? 1 : 2);

	/* the rows below share the free space */
	show_icy = !compact && station >= 0 && radio.icy.name[0];
	rows_h = h_big + (show_icy ? h_small : 0) + (h_mid + 2 * pad) + (h_small + 4) + 2 * h_small +
		 (h_small + (compact ? 6 : 9)) + info_n * h_small;
	gap = (bottom - y - rows_h) / 8;
	if (gap < 1)
		gap = 1;
	if (gap > 3 * pad)
		gap = 3 * pad;
	y += gap;

	/* station, < > when there are others */
	if (nst > 1) {
		int ah = h_big / 3, cy = y + h_big / 2;
		tri(pad, cy, pad + ah, cy - ah, pad + ah, cy + ah, C_EDGE);
		tri(W - 1 - pad, cy, W - 1 - pad - ah, cy - ah, W - 1 - pad - ah, cy + ah, C_EDGE);
	}
	CutWSTR(ws, 0);
	if (screen_st < nst)
		text_ws(ws, stations[screen_st].name);
	{	/* too long for the big font: the small bold one */
		int x1 = pad + h_big / 2 + 2, x2 = W - 1 - pad - h_big / 2 - 2, f = f_big, fy = y;
		if (Get_WS_width(ws, f_big) > x2 - x1) {
			f = f_bold;
			fy = y + (h_big - h_small) / 2;
		}
		DrawString(ws, x1, fy, x2, fy + GetFontYSIZE(f), f, TEXT_ALIGNMIDDLE, C_TXT, C_NONE);
	}
	y += h_big;
	if (show_icy) {
		CutWSTR(ws, 0);
		text_ws(ws, radio.icy.name);
		DrawString(ws, pad, y, W - 1 - pad, y + h_small, f_small, TEXT_ALIGNMIDDLE, C_SUB, C_NONE);
		y += h_small;
	}
	y += gap;

	/* track */
	h = h_mid + 2 * pad;
	DrawRoundedFrame(pad, y, W - 1 - pad, y + h - 1, compact ? 3 : 6, compact ? 3 : 6, 0, C_EDGE, C_PANEL);
	if (title_w > title_box_w())
		DrawScrollString(ws_title, 2 * pad, y + pad, 2 * pad + title_box_w(), y + pad + h_mid, xdisp, f_mid,
				 TEXT_ALIGNLEFT, station >= 0 ? C_TXT : C_DIM, C_NONE);
	else
		DrawString(ws_title, 2 * pad, y + pad, 2 * pad + title_box_w(), y + pad + h_mid, f_mid, TEXT_ALIGNMIDDLE,
			   station >= 0 ? C_TXT : C_DIM, C_NONE);
	y += h + gap;

	/* state pill + volume bars */
	st = state_text(b, &col, d);
	h = h_small + 4;
	x = text_w(st, f_bold) + 2 * pad + 4;
	DrawRoundedFrame(pad, y, pad + x, y + h - 1, h / 2, h / 2, 0, col, col);
	text(st, pad, y + 2, pad + x, h_small, f_bold, TEXT_ALIGNMIDDLE, C_DARK);
	{
		/* one bar per step (EL71: 15 thin ones, SG: 5) */
		int nb = VOL_MAX, bw = nb > 8 ? 3 : compact ? 3 : 5, bg = nb > 8 ? 1 : compact ? 1 : 2;
		int x0 = W - 1 - pad - nb * (bw + bg) + bg;
		for (i = 0; i < nb; i++) {
			int bh = h * (i + 2) / (nb + 1), bx = x0 + i * (bw + bg);
			fill(bx, y + h - bh, bx + bw - 1, y + h - 1, i < volume ? C_ACC : C_EDGE);
		}
	}
	y += h + 2;
	/* detail: up to 2 lines */
	if (d[0]) {
		wsprintf(ws, "%t", d);
		DrawString(ws, pad, y, W - 1 - pad, y + 2 * h_small, f_small, TEXT_ALIGNLEFT,
			   col == C_ERR ? C_ERR : C_SUB, C_NONE);
	}
	y += 2 * h_small + gap;

	/* buffer */
	{
		int ms = buffered_ms(), bx1 = pad, bx2 = W - 1 - pad, bh = compact ? 5 : 8, rs = ring_size(&net_ring);
		int fx = bx1 + (int)((uint64_t)used_b() * (bx2 - bx1) / rs);
		text("Buffer", pad, y, W / 2, h_small, f_small, TEXT_ALIGNLEFT, C_DIM);
		sprintf(v, "%d.%d s / %dK", ms / 1000, ms % 1000 / 100, rs >> 10);
		text(v, W / 3, y, W - 1 - pad, h_small, f_small, TEXT_ALIGNRIGHT, C_SUB);
		y += h_small + 1;
		DrawRoundedFrame(bx1, y, bx2, y + bh - 1, 2, 2, 0, C_EDGE, C_BARBG);
		if (fx > bx1 + 1)
			DrawRoundedFrame(bx1, y, fx, y + bh - 1, 2, 2, 0, pstate == P_PLAY ? C_PLAY : C_BUF,
					 pstate == P_PLAY ? C_PLAY : C_BUF);
		if (start_need && pstate == P_BUFFER) {
			int mx = bx1 + (int)((uint64_t)start_need * (bx2 - bx1) / rs);
			fill(mx, y - 1, mx + (compact ? 0 : 1), y + bh, C_TXT);
		}
		y += bh + gap;
	}

	/* stream (+ format), network */
	if (kbps > 0 || radio.icy.br > 0)
		sprintf(v, "%d kbps", kbps > 0 ? kbps : radio.icy.br);
	else
		strcpy(v, "-");
	if (pstate == P_PLAY && rate) {
		if (compact)
			sprintf(v + strlen(v), " %dk%s", (rate + 500) / 1000, chans == 1 ? "m" : "");
		else {
			char *k = strstr(v, " kbps");
			if (k)
				strcpy(k, "k");	/* "128k 44.1k st": "kbps, ..." does not fit the row */
			sprintf(v + strlen(v), " %d.%dk %s", rate / 1000, rate % 1000 / 100, chans == 1 ? "m" : "st");
			be_fmt_desc(b);
			if (b[0])
				sprintf(v + strlen(v), " > %s", b);
		}
	}
	info_row(y, "Stream", v, C_TXT);
	y += h_small;
	if (radio.state == RS_STREAM || rx_secs) {
		fmt_kbs(v, rx_bps);
		if (radio.tls)
			strcat(v, ", https");
	} else
		strcpy(v, station >= 0 && (radio.state == RS_CONNECT || radio.state == RS_HEADERS) ? "..." : "-");
	info_row(y, "Net", v, rx_secs >= 4 && rx_bps * 10 < stream_bps() * 9 ? C_BUF : C_TXT);

	/* softkeys */
	fill(0, bottom, W - 1, H - 1, C_PANEL);
	fill(0, bottom, W - 1, bottom, C_EDGE);
	text("Menu", pad, bottom + (sk_h - h_small) / 2, W / 2, h_small, f_bold, TEXT_ALIGNLEFT, C_TXT);
	text("Hide", W / 2, bottom + (sk_h - h_small) / 2, W - 1 - pad, h_small, f_bold, TEXT_ALIGNRIGHT, C_TXT);
	{	/* play / stop between the two labels */
		int l = pad + text_w("Menu", f_bold), rt = W - 1 - pad - text_w("Hide", f_bold);
		int r = sk_h / 2 - 2, cx = (l + rt) / 2, cy = bottom + sk_h / 2;
		DrawRoundedFrame(cx - r - 3, cy - r, cx + r + 3, cy + r, r, r, 0, C_ACC, C_ACC);
		if (station >= 0)	/* stop */
			fill(cx - r / 2, cy - r / 2, cx + r / 2, cy + r / 2, C_DARK);
		else
			tri(cx - r / 2 + 1, cy - r / 2 - 1, cx - r / 2 + 1, cy + r / 2 + 1, cx + r / 2 + 2, cy, C_DARK);
	}
}

/* ------------------------------------------------------------------ GUI */

static unsigned shown_sig;
#ifdef RADIO_SKIN
static int bar_off;		/* we disabled the icon bar (Victor89 full screen) */
#endif
static int since;

void screen_redraw(void)
{
	if (gui_id && IsGuiOnTop(gui_id)) {
		shown_sig = signature();
		since = 0;
		DirectRedrawGUI_ID(gui_id);
	}
}

void screen_tick(void)
{
	static int scroll_t;
	int per = period;
	if (!gui_id || !IsGuiOnTop(gui_id))
		return;
#ifdef RADIO_SKIN
	if (!display_lit())
		return;		/* sleeping: nothing drawn; the change shows at the first lit tick */
	if (reels_turn())
		per = 2;	/* ~4 fps: the reel phase changes every 250 ms */
#endif
	since++;
	if (++scroll_t >= period) {
		scroll_t = 0;
		scroll_step();
	}
	if (since >= per && signature() != shown_sig)
		screen_redraw();
}

int screen_on_top(void)
{
	return gui_id && IsGuiOnTop(gui_id);
}

void screen_free(void)
{
	if (ws)
		FreeWS(ws);
	if (ws_title)
		FreeWS(ws_title);
	ws = ws_title = 0;
	gui_id = 0;
#ifdef RADIO_SKIN
	reels_free();
#endif
}

static void OnCreate(GUI *g, malloc_func_t f)
{
	(void)f;
	g->state = CSM_GUI_STATE_UNFOCUSED;
}

static void OnClose(GUI *g, mfree_func_t f)
{
	(void)f;
#ifdef RADIO_SKIN
	if (bar_off) {
		bar_off = 0;
		DisableIconBar(0);
	}
#endif
	g->state = CSM_GUI_STATE_CLOSED;
}

#ifdef RADIO_SKIN

/* Victor89: the whole 240x320 (no icon bar: canvas from 0, flag30 = 2 as vNes/MC, DisableIconBar as duke3d) */
static void full_screen(GUI *g, int on)
{
	StoreXYWHtoRECT(&canvas, 0, on ? 0 : YDISP, ScreenW(), ScreenH() - (on ? 0 : YDISP));
	g->flag30 = on ? 2 : 0;
	if (on != bar_off) {
		bar_off = on;
		DisableIconBar(on);
	}
}
#endif

static void OnFocus(GUI *g, malloc_func_t m, mfree_func_t f)
{
	(void)m;
	(void)f;
	g->state = CSM_GUI_STATE_FOCUSED;
#ifdef RADIO_SKIN
	full_screen(g, cfg.skin);
#endif
	metrics();
	title_seq = -1;		/* fonts may differ: measure again */
}

static void OnUnfocus(GUI *g, mfree_func_t f)
{
	(void)f;
	if (g->state != CSM_GUI_STATE_FOCUSED)
		return;
#ifdef RADIO_SKIN
	if (bar_off) {		/* the icon bar back for whatever comes on top */
		bar_off = 0;
		DisableIconBar(0);
	}
#endif
	g->state = CSM_GUI_STATE_UNFOCUSED;
}

static void next_station(int d)
{
	if (nst < 1)
		return;
	screen_st = (screen_st + d + nst) % nst;
	if (station >= 0)
		play_station(screen_st);
}

static int OnKey(GUI *g, GUI_MSG *msg)
{
	int k = msg->gbsmsg->submess, m = msg->gbsmsg->msg;
	(void)g;
	if (m == LONG_PRESS && (k == UP_BUTTON || k == DOWN_BUTTON || k == VOL_UP_BUTTON || k == VOL_DOWN_BUTTON ||
				k == '*' || k == '#'))
		m = KEY_DOWN;	/* repeat */
	if (m != KEY_DOWN)
		return 0;
#ifdef RADIO_SKIN
	if (cfg.skin && k == RIGHT_SOFT) {	/* Victor89: right Опции = the menu, left Стоп/Играть = centre */
		menu_open();
		return 0;
	}
	if (cfg.skin && k == LEFT_SOFT)
		k = ENTER_BUTTON;
#endif
	switch (k) {
	case LEFT_SOFT:
		menu_open();
		return 0;
	case RIGHT_SOFT:
	case RED_BUTTON:
		minimize();
		return 0;
	case ENTER_BUTTON:
	case GREEN_BUTTON:
		if (station >= 0)
			stop_all();
		else
			play_station(screen_st);
		break;
	case LEFT_BUTTON:
		next_station(-1);
		break;
	case RIGHT_BUTTON:
		next_station(1);
		break;
	case UP_BUTTON:
	case VOL_UP_BUTTON:
	case '#':
		set_volume(volume + 1);
		break;
	case DOWN_BUTTON:
	case VOL_DOWN_BUTTON:
	case '*':
		set_volume(volume - 1);
		break;
	case '0':
		stations_open();
		return 0;
	default:
		if (k >= '1' && k <= '9' && k - '1' < nst)
			play_station(k - '1');
		else
			return 0;
	}
	screen_redraw();
	return 0;
}

static int Method8(void) { return 0; }
static int Method9(void) { return 0; }

static GUI_METHODS methods = {
	OnRedraw, OnCreate, OnClose, OnFocus, OnUnfocus, OnKey, NULL, kill_data, Method8, Method9, NULL
};

int screen_create(MAIN_CSM *csm)
{
	SCR_GUI *g = malloc(sizeof(*g));
	(void)csm;
	if (!g)
		return 0;
	zeromem(g, sizeof(*g));
	if (!ws)
		ws = AllocWS(256);
	if (!ws_title)
		ws_title = AllocWS(ICY_TITLE + 8);
	metrics();
	StoreXYWHtoRECT(&canvas, 0, YDISP, ScreenW(), ScreenH() - YDISP);
#ifdef RADIO_SKIN
	if (cfg.skin) {
		StoreXYWHtoRECT(&canvas, 0, 0, ScreenW(), ScreenH());
		g->gui.flag30 = 2;
	}
#endif
	g->gui.canvas = &canvas;
	g->gui.methods = &methods;
	g->gui.item_ll.data_mfree = mfree_adr();
	return gui_id = CreateGUI(g);
}

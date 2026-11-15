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
 */
#include "app.h"
#include "build_id.h"

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

static int title_box_w(void)
{
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
	if (be_test_on()) {
		*col = C_BUF;
		be_test_line(detail);
		return "Sound test";
	}
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
		return "Playing";
	case P_CALL:
		*col = C_BUF;
		return "Paused: call";
	}
	return "";
}

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
	if (!gui_id || !IsGuiOnTop(gui_id))
		return;
	since++;
	if (++scroll_t >= period) {
		scroll_t = 0;
		scroll_step();
	}
	if (since >= period && signature() != shown_sig)
		screen_redraw();
}

void screen_free(void)
{
	if (ws)
		FreeWS(ws);
	if (ws_title)
		FreeWS(ws_title);
	ws = ws_title = 0;
	gui_id = 0;
}

static void OnCreate(GUI *g, malloc_func_t f)
{
	(void)f;
	g->state = CSM_GUI_STATE_UNFOCUSED;
}

static void OnClose(GUI *g, mfree_func_t f)
{
	(void)f;
	g->state = CSM_GUI_STATE_CLOSED;
}

static void OnFocus(GUI *g, malloc_func_t m, mfree_func_t f)
{
	(void)m;
	(void)f;
	g->state = CSM_GUI_STATE_FOCUSED;
	metrics();
	title_seq = -1;		/* fonts may differ: measure again */
}

static void OnUnfocus(GUI *g, mfree_func_t f)
{
	(void)f;
	if (g->state != CSM_GUI_STATE_FOCUSED)
		return;
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
	screen_st = cfg.def >= 0 && cfg.def < nst ? cfg.def : 0;
	StoreXYWHtoRECT(&canvas, 0, YDISP, ScreenW(), ScreenH() - YDISP);
	g->gui.canvas = &canvas;
	g->gui.methods = &methods;
	g->gui.item_ll.data_mfree = mfree_adr();
	return gui_id = CreateGUI(g);
}

/*
 * The firmware's own menus (CreateMenu, as bt-pan's native UI / ncdu): the firmware draws them, we give the
 * item texts. Opened only from a key handler of ours (CreateMenu attaches the GUI to the CSM whose message
 * runs - s65-player: from a foreign timer -> MMI data abort).
 *
 *   Меню        Станции..., Настройки..., [SG emulator: Тест звука], Свернуть, Выход
 *   Станции     the list (» playing, • default), + Добавить станцию; centre = play, left soft = options
 *   Станция     Играть, Изменить..., По умолчанию, Удалить
 *   Настройки   buffer, start/resume levels, [SG: output format], volume at start, auto-reconnect,
 *               play at start, [EL71: skin], [emulator: log]; select / right = next value, left = previous; saved to radio.cfg at once
 *   Изменить    the firmware's input dialog: name + URL -> stations.txt
 */
#include "app.h"

enum { SC_MENU, SC_SET, SC_ST, SC_OPT, SC_N };
#define ROWS_MAX	(MAX_ST + 2)
static int ids[SC_N], nrows[SC_N], opening = -1;
static unsigned char rows[SC_N][ROWS_MAX];
static unsigned sig_st, sig_menu;
static int opt_st;			/* the station of the options menu */

enum { M_STATIONS, M_SETTINGS, M_TEST, M_MIN, M_EXIT };
enum { R_RING, R_START, R_RESUME, R_FMT, R_VOL, R_RETRY, R_AUTO, R_SKIN, R_LOG };
enum { O_PLAY, O_EDIT, O_DEF, O_DEL };
#define ST_ADD	(MAX_ST + 1)		/* row id of "+ Add station" (station rows = their index) */

static int make_rows(int s, unsigned char *r)
{
	int n = 0, i;
	switch (s) {
	case SC_MENU:
		r[n++] = M_STATIONS;
		r[n++] = M_SETTINGS;
#ifdef RADIO_LOG
		if (be_has_test())
			r[n++] = M_TEST;
#endif
		r[n++] = M_MIN;
		r[n++] = M_EXIT;
		break;
	case SC_SET:
		r[n++] = R_RING;
		r[n++] = R_START;
		r[n++] = R_RESUME;
		if (be_nfmt())
			r[n++] = R_FMT;
		r[n++] = R_VOL;
		r[n++] = R_RETRY;
		r[n++] = R_AUTO;
#ifdef RADIO_SKIN
		r[n++] = R_SKIN;
#endif
#ifdef RADIO_LOG_CFG
		r[n++] = R_LOG;
#endif
		break;
	case SC_ST:
		for (i = 0; i < nst; i++)
			r[n++] = i;
		if (nst < MAX_ST)
			r[n++] = ST_ADD;
		break;
	default:
		r[n++] = O_PLAY;
		r[n++] = O_EDIT;
		r[n++] = O_DEF;
		r[n++] = O_DEL;
		break;
	}
	return n;
}

static int row_at(int s, int item)
{
	return item >= 0 && item < nrows[s] ? rows[s][item] : -1;
}

static int screen_of(GUI *gui)
{
	int s;
	for (s = 0; s < SC_N; s++)
		if (ids[s] && FindGUIbyId(ids[s], NULL) == (void *)gui)
			return s;
	return opening >= 0 ? opening : SC_MENU;
}

/* ------------------------------------------------------------------ item texts */

static void item_text(void *gui, int n, const char *t)
{
	void *item = AllocMenuItem(gui);
	WSHDR *w = AllocMenuWS(gui, 128);
	wsprintf(w, "%t", t);
	SetMenuItemText(gui, item, w, n);
}

static void menu_item(void *gui, int n, void *user)
{
	(void)user;
	switch (row_at(SC_MENU, n)) {
	case M_STATIONS: item_text(gui, n, "Stations"); break;
	case M_SETTINGS: item_text(gui, n, "Settings"); break;
#ifdef RADIO_LOG
	case M_TEST: item_text(gui, n, be_test_on() ? "Sound test: running" : "Sound test (to log)"); break;
#endif
	case M_MIN: item_text(gui, n, "Hide"); break;
	default: item_text(gui, n, "Exit"); break;
	}
}

static const char *on_off(int v)
{
	return v ? "on" : "off";
}

static void set_item(void *gui, int n, void *user)
{
	char b[96];
	(void)user;
	switch (row_at(SC_SET, n)) {	/* short: the menu font is wide on ELKA too */
	case R_RING: sprintf(b, "Buffer: %d KB", cfg.ring_kb); break;
	case R_START: sprintf(b, "Start: %d s", cfg.start_sec); break;
	case R_RESUME: sprintf(b, "Resume: %d s", cfg.resume_sec); break;
	case R_FMT: sprintf(b, "Output: %s", be_fmt_name(cfg.fmt)); break;
	case R_VOL: sprintf(b, "Volume: %d/%d", cfg.vol, VOL_MAX); break;
	case R_RETRY: sprintf(b, "Reconnect: %s", on_off(cfg.retry)); break;
	case R_AUTO: sprintf(b, "Autoplay: %s", on_off(cfg.autoplay)); break;
#ifdef RADIO_SKIN
	case R_SKIN: {		/* "Виктор 89" as Unicode code points (literals here are cp1251) */
		static const unsigned short victor[] = { 0x412, 0x438, 0x43A, 0x442, 0x43E, 0x440, ' ', '8', '9', 0 };
		void *item = AllocMenuItem(gui);
		WSHDR *w = AllocMenuWS(gui, 64);
		const unsigned short *c;
		wsprintf(w, "%t", cfg.skin ? "Skin: " : "Skin: Modern");
		if (cfg.skin)
			for (c = victor; *c; c++)
				wsAppendChar(w, *c);
		SetMenuItemText(gui, item, w, n);
		return;
	}
#endif
#ifdef RADIO_LOG_CFG
	case R_LOG: sprintf(b, "Log: %s", on_off(cfg.log)); break;
#endif
	default: b[0] = 0; break;
	}
	item_text(gui, n, b);
}

static void st_item(void *gui, int n, void *user)
{
	int r = row_at(SC_ST, n);
	void *item;
	WSHDR *w;
	(void)user;
	if (r < 0 || r >= nst) {
		item_text(gui, n, "+ Add station");
		return;
	}
	item = AllocMenuItem(gui);
	w = AllocMenuWS(gui, 128);
	wsprintf(w, "%t", r == station ? "» " : r == cfg.def ? "• " : "");
	text_ws(w, stations[r].name);
	SetMenuItemText(gui, item, w, n);
}

static void opt_item(void *gui, int n, void *user)
{
	(void)user;
	switch (row_at(SC_OPT, n)) {
	case O_PLAY: item_text(gui, n, opt_st == station ? "Playing" : "Play"); break;
	case O_EDIT: item_text(gui, n, "Edit..."); break;
	case O_DEF: item_text(gui, n, opt_st == cfg.def ? "Default: this one" : "Make default"); break;
	default: item_text(gui, n, "Delete"); break;
	}
}

/* ------------------------------------------------------------------ menus */

static unsigned st_signature(void)
{
	return (unsigned)(station + 1) * 977u + (unsigned)nst * 31u + (unsigned)(cfg.def + 1) * 7u;
}

/* rows and texts again; the cursor stays on its row */
static void sync(int s)
{
	void *gui = ids[s] ? FindGUIbyId(ids[s], NULL) : NULL;
	int cur, old, i, n;
	unsigned char r[ROWS_MAX];
	if (!gui)
		return;
	cur = GetCurMenuItem(gui);
	old = row_at(s, cur);
	n = make_rows(s, r);
	for (i = 0; i < n && old >= 0; i++)
		if (r[i] == old)
			break;
	if (old >= 0 && i < n)
		cur = i;
	if (cur >= n)
		cur = n - 1;
	if (cur < 0)
		cur = 0;
	memcpy(rows[s], r, n);
	nrows[s] = n;
	Menu_SetItemCountDyn(gui, n);
	SetCursorToMenuItem(gui, cur);
	if (IsGuiOnTop(ids[s]))
		RefreshGUI();
}

void menus_sync(void)
{
	unsigned a = st_signature(), b = (unsigned)be_test_on();
	if (a != sig_st) {
		sig_st = a;
		sync(SC_ST);
		sync(SC_OPT);
	}
	if (b != sig_menu) {
		sig_menu = b;
		sync(SC_MENU);
	}
}

static void hook(GUI *gui, int cmd)
{
	int s = screen_of(gui);
	if (cmd == UI_CMD_FOCUS) {	/* back on top (after the input dialog, a message): fresh rows */
		nrows[s] = make_rows(s, rows[s]);
		Menu_SetItemCountDyn(gui, nrows[s]);
	} else if (cmd == UI_CMD_DESTROY && ids[s] && FindGUIbyId(ids[s], NULL) == (void *)gui)
		ids[s] = 0;
}

/* 'S' = centre / select, 'L' = left soft, 'B' = back, else the key, 0 = nothing. On SGOLD the softkeys come as
 * an action (msg->keys = the SOFTKEY_DESC key), KEY_DOWN RIGHT_SOFT alone did not come (s65-player, bt-pan). */
static int act(GUI_MSG *msg)
{
	int k = msg->gbsmsg->submess;
#ifdef SGOLD
	if (msg->keys == 0x18)
		return 'L';
	if (msg->keys == 0x3D)
		return 'S';
	if (msg->keys == 0x01)
		return 'B';
#endif
	if (msg->gbsmsg->msg != KEY_DOWN && !(msg->gbsmsg->msg == LONG_PRESS && (k == LEFT_BUTTON || k == RIGHT_BUTTON)))
		return 0;
	if (k == LEFT_SOFT)
		return 'L';
	if (k == ENTER_BUTTON)
		return 'S';
	if (k == RIGHT_SOFT)
		return 'B';
	return k;
}

static int key_menu(GUI *gui, GUI_MSG *msg);
static int key_set(GUI *gui, GUI_MSG *msg);
static int key_st(GUI *gui, GUI_MSG *msg);
static int key_opt(GUI *gui, GUI_MSG *msg);

static HEADER_DESC hdr[SC_N] = {
	{ { 0, 0, 0, 0 }, NULL, (int)"Fasoley Radio", LGP_NULL },
	{ { 0, 0, 0, 0 }, NULL, (int)"Settings", LGP_NULL },
	{ { 0, 0, 0, 0 }, NULL, (int)"Stations", LGP_NULL },
	{ { 0, 0, 0, 0 }, NULL, (int)"Station", LGP_NULL },
};
static const int softkeys[] = { SET_LEFT_SOFTKEY, SET_RIGHT_SOFTKEY, SET_MIDDLE_SOFTKEY };
static const SOFTKEY_DESC sk_sel[] = {
	{ 0x0018, 0x0000, (int)"Select" }, { 0x0001, 0x0000, (int)"Back" }, { 0x003D, 0x0000, (int)LGP_DOIT_PIC },
};
static const SOFTKEY_DESC sk_change[] = {
	{ 0x0018, 0x0000, (int)"Edit" }, { 0x0001, 0x0000, (int)"Back" }, { 0x003D, 0x0000, (int)LGP_DOIT_PIC },
};
static const SOFTKEY_DESC sk_opts[] = {
	{ 0x0018, 0x0000, (int)"Options" }, { 0x0001, 0x0000, (int)"Back" }, { 0x003D, 0x0000, (int)LGP_DOIT_PIC },
};
static const SOFTKEYSTAB skt_sel = { sk_sel, 0 }, skt_change = { sk_change, 0 }, skt_opts = { sk_opts, 0 };
static const MENU_DESC menu_d[SC_N] = {
	{ 8, key_menu, hook, NULL, softkeys, &skt_sel, MENU_FLAGS_ENABLE_TEXT_SCROLLING, menu_item, NULL, NULL, 0 },
	{ 8, key_set, hook, NULL, softkeys, &skt_change, MENU_FLAGS_ENABLE_TEXT_SCROLLING, set_item, NULL, NULL, 0 },
	{ 8, key_st, hook, NULL, softkeys, &skt_opts, MENU_FLAGS_ENABLE_TEXT_SCROLLING, st_item, NULL, NULL, 0 },
	{ 8, key_opt, hook, NULL, softkeys, &skt_sel, MENU_FLAGS_ENABLE_TEXT_SCROLLING, opt_item, NULL, NULL, 0 },
};

static void open_menu(int s, int cur)
{
	HEADER_DESC *h = &hdr[s];
	if (ids[s])
		return;
	h->rc.x = 0;
	h->rc.y = YDISP;
	h->rc.x2 = ScreenW() - 1;
	h->rc.y2 = YDISP + HeaderH();
	nrows[s] = make_rows(s, rows[s]);
	if (cur >= nrows[s])
		cur = 0;
	if (s == SC_ST)
		sig_st = st_signature();
	opening = s;
	ids[s] = CreateMenu(0, 0, &menu_d[s], h, cur, nrows[s], NULL, NULL);
	opening = -1;
}

static void close_menu(int s)
{
	if (ids[s]) {
		int id = ids[s];
		ids[s] = 0;
		GeneralFunc_flag1(id, 1);
	}
}

void menu_open(void)
{
	open_menu(SC_MENU, 0);
}

void stations_open(void)
{
	open_menu(SC_ST, station >= 0 ? station : screen_st);
}

void menus_close(void)
{
	int s;
	for (s = SC_N - 1; s >= 0; s--)
		close_menu(s);
}

/* ------------------------------------------------------------------ station editor (input dialog) */

static int edit_i;		/* station being edited, nst = a new one */

static void patch_rects(HEADER_DESC *head, INPUTDIA_DESC *inp)
{
	head->rc.x = 0;
	head->rc.y = YDISP;
	head->rc.x2 = ScreenW() - 1;
	head->rc.y2 = HeaderH() + YDISP - 1;
	inp->rc.x = 0;
	inp->rc.y = HeaderH() + 1 + YDISP;
	inp->rc.x2 = ScreenW() - 1;
	inp->rc.y2 = ScreenH() - SoftkeyH() - 1;
}

static int edit_save(GUI *gui)
{
	static char name[ST_NAME * 2], url[ST_URL];
	EDITCONTROL ec;
	int add = edit_i >= nst, was = edit_i == station;

	ExtractEditControl(gui, 2, &ec);
	ws_utf8(ec.pWS, name, sizeof(name));
	ExtractEditControl(gui, 4, &ec);
	ws_utf8(ec.pWS, url, sizeof(url));
	if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
		ShowMSG(1, (int)"URL: http://... or https://...");
		return 0;
	}
	if (url[7] == 0 || (url[4] == 's' && url[8] == 0)) {
		ShowMSG(1, (int)"No server address");
		return 0;
	}
	was = was && strcmp(stations[edit_i].url, url);	/* the playing one got another address */
	if (!station_set(edit_i, name, url))
		return 1;
	if (!stations_save())
		ShowMSG(1, (int)"Cannot write stations.txt");
	rlog("station %d %s: %s = %s", edit_i, add ? "added" : "changed", stations[edit_i].name, stations[edit_i].url);
	(void)add;
	if (was)
		play_station(edit_i);
	return 1;
}

static int edit_key(GUI *gui, GUI_MSG *msg)
{
	if (msg->keys == 0xFFF || (msg->gbsmsg->msg == KEY_DOWN && msg->gbsmsg->submess == GREEN_BUTTON))
		return edit_save(gui) ? 1 : -1;
	if (msg->keys == 0xFF0)
		return 1;
	return 0;
}

static SOFTKEY_DESC sk_ok = { 0x0FFF, 0x0000, (int)"OK" };

static void edit_hook(GUI *gui, int cmd)
{
	if (cmd == UI_CMD_CREATE)
		EDIT_SetFocus(gui, 2);
	else if (cmd == UI_CMD_REDRAW)
		SetSoftKey(gui, &sk_ok, SET_SOFT_KEY_N);
}

static void edit_locret(void) {}

static const SOFTKEY_DESC edit_sk[] = {
	{ 0x0018, 0x0000, (int)"Options" }, { 0x0001, 0x0000, (int)"Back" }, { 0x003D, 0x0000, (int)LGP_DOIT_PIC },
};
static const SOFTKEYSTAB edit_skt = { edit_sk, 0 };
static HEADER_DESC edit_header = { { 0, 0, 0, 0 }, NULL, (int)"Station", LGP_NULL };
static INPUTDIA_DESC edit_desc = {
	.one = 1, .onKey = edit_key, .global_hook_proc = edit_hook, .locret = (void *)edit_locret,
	.softkeystab = &edit_skt, .font = FONT_SMALL, ._100 = 100, ._101 = 101, .flags = 0x40000000
};

static void add_label(void *eq, WSHDR *w, const char *t)
{
	EDITCONTROL ec;
	EDITC_OPTIONS opt;
	wsprintf(w, "%t", t);
	PrepareEditControl(&ec);
	ConstructEditControl(&ec, ECT_HEADER, ECF_APPEND_EOL, w, wstrlen(w));
	PrepareEditCOptions(&opt);
	SetFontToEditCOptions(&opt, FONT_SMALL);
	CopyOptionsToEditControl(&ec, &opt);
	AddEditControlToEditQend(eq, &ec, malloc_adr());
}

static void add_field(void *eq, WSHDR *w, int max)
{
	EDITCONTROL ec;
	PrepareEditControl(&ec);
	ConstructEditControl(&ec, ECT_NORMAL_TEXT, ECF_APPEND_EOL, w, max);
	AddEditControlToEditQend(eq, &ec, malloc_adr());
}

static void edit_open(int i)
{
	void *eq = AllocEQueue(malloc_adr(), mfree_adr());
	WSHDR *w = AllocWS(ST_URL + 8);

	edit_i = i;
	add_label(eq, w, "Name:");
	CutWSTR(w, 0);
	if (i < nst)
		text_ws(w, stations[i].name);
	add_field(eq, w, ST_NAME - 1);
	add_label(eq, w, "URL (http:// or https://):");
	CutWSTR(w, 0);
	if (i < nst)
		text_ws(w, stations[i].url);
	else
		wsprintf(w, "%t", "http://");
	add_field(eq, w, ST_URL - 1);
	FreeWS(w);
	patch_rects(&edit_header, &edit_desc);
	CreateInputTextDialog(&edit_desc, &edit_header, eq, 1, NULL);
}

/* ------------------------------------------------------------------ keys */

static int key_menu(GUI *gui, GUI_MSG *msg)
{
	int a = act(msg);
	if (a == 'B' || a == RED_BUTTON) {
		ids[SC_MENU] = 0;
		return 1;
	}
	if (a != 'S' && a != 'L')
		return 0;
	switch (row_at(SC_MENU, GetCurMenuItem(gui))) {
	case M_STATIONS:
		stations_open();
		break;
	case M_SETTINGS:
		open_menu(SC_SET, 0);
		break;
#ifdef RADIO_LOG
	case M_TEST:
		be_test_start();
		ids[SC_MENU] = 0;
		return 1;
#endif
	case M_MIN:
		ids[SC_MENU] = 0;
		minimize();
		return 1;
	case M_EXIT:
		app_exit();
		break;
	}
	return -1;
}

/* the value of a settings row one step on (d = 1) or back (d = -1) */
static void set_change(int r, int d)
{
	int k;
	switch (r) {
	case R_RING:
		for (k = 0; k < be_nring && be_ring_kb[k] != cfg.ring_kb; k++)
			;
		k = (k + d + be_nring) % be_nring;
		cfg.ring_kb = be_ring_kb[k];
		break;
	case R_START: cfg.start_sec = (cfg.start_sec + d + 9) % 10 + 1; break;	/* 1..10 */
	case R_RESUME: cfg.resume_sec = (cfg.resume_sec + d + 9) % 10 + 1; break;
	case R_FMT:
		if (d > 0)
			cfg.fmt = be_fmt_next(cfg.fmt);
		else
			for (k = 0; k < be_nfmt(); k++)
				if (be_fmt_next(k) == cfg.fmt) {
					cfg.fmt = k;
					break;
				}
		rlog("output format %d (%s), applies on the next start", cfg.fmt, be_fmt_name(cfg.fmt));
		break;
	case R_VOL: {
		int n = VOL_MAX - VOL_MIN + 1;
		cfg.vol = (cfg.vol - VOL_MIN + d + n) % n + VOL_MIN;
		break;
	}
	case R_RETRY: cfg.retry = !cfg.retry; apply_retry(); break;
	case R_AUTO: cfg.autoplay = !cfg.autoplay; break;
#ifdef RADIO_SKIN
	case R_SKIN: cfg.skin = !cfg.skin; break;	/* the screen redraws when it is on top again */
#endif
#ifdef RADIO_LOG_CFG
	case R_LOG: cfg.log = !cfg.log; log_apply(); break;
#endif
	}
	cfg_save();
	if (r == R_FMT && station >= 0)	/* new format at once */
		play_station(station);
}

static int key_set(GUI *gui, GUI_MSG *msg)
{
	int a = act(msg);
	if (a == 'B' || a == RED_BUTTON) {
		ids[SC_SET] = 0;
		return 1;
	}
	if (a != 'S' && a != 'L' && a != LEFT_BUTTON && a != RIGHT_BUTTON)
		return 0;
	set_change(row_at(SC_SET, GetCurMenuItem(gui)), a == LEFT_BUTTON ? -1 : 1);
	sync(SC_SET);
	return -1;
}

static int key_st(GUI *gui, GUI_MSG *msg)
{
	int a = act(msg), r = row_at(SC_ST, GetCurMenuItem(gui));
	if (a == 'B' || a == RED_BUTTON) {
		ids[SC_ST] = 0;
		return 1;
	}
	if (r == ST_ADD && (a == 'S' || a == 'L')) {
		edit_open(nst);
		return -1;
	}
	if (r < 0 || r >= nst)
		return 0;
	if (a == 'S') {		/* play it, back to the main screen */
		play_station(r);
		close_menu(SC_MENU);
		ids[SC_ST] = 0;
		return 1;
	}
	if (a == 'L') {
		opt_st = r;
		open_menu(SC_OPT, 0);
		return -1;
	}
	return 0;
}

static void del_answer(int no)
{
	int i = opt_st;
	if (no || i < 0 || i >= nst)
		return;
	if (nst <= 1) {
		ShowMSG(1, (int)"Cannot delete the last station");
		return;
	}
	rlog("station %d deleted: %s", i, stations[i].url);
	if (station == i)
		stop_all();
	else if (station > i)
		station--;
	station_del(i);
	if (screen_st >= nst || screen_st > i)
		screen_st = screen_st > 0 ? screen_st - 1 : 0;
	if (!stations_save())
		ShowMSG(1, (int)"Cannot write stations.txt");
	cfg_save();
	close_menu(SC_OPT);
	sync(SC_ST);
}

static int key_opt(GUI *gui, GUI_MSG *msg)
{
	int a = act(msg), i = opt_st;
	if (a == 'B' || a == RED_BUTTON) {
		ids[SC_OPT] = 0;
		return 1;
	}
	if (a != 'S' && a != 'L')
		return 0;
	if (i < 0 || i >= nst)
		return -1;
	switch (row_at(SC_OPT, GetCurMenuItem(gui))) {
	case O_PLAY:
		play_station(i);
		close_menu(SC_ST);
		close_menu(SC_MENU);
		ids[SC_OPT] = 0;
		return 1;
	case O_EDIT:
		edit_open(i);
		break;
	case O_DEF:
		cfg.def = i;
		cfg_save();
		sync(SC_OPT);
		sync(SC_ST);
		break;
	case O_DEL:
		MsgBoxYesNo(1, (int)"Delete station?", del_answer);
		break;
	}
	return -1;
}

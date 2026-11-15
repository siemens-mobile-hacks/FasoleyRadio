/*
 * Fasoley Radio: internet radio elf for EL71v45 (FasoleyRadio_ELKA.elf, ELKA, ElfLoader 3) and S65v58 / SK65v50
 * (FasoleyRadio.elf, SGOLD, ElfPack 3). This file is the common core: the player state machine, the CSM,
 * the log. Sources: built-in + <elf folder>\stations.txt (cfg.c), https via src/link.c (BearSSL).
 *
 * Network (helper context, SUBPROC): ENIP socket -> HTTP/ICY (radio.c, icy.c) -> net ring (MP3 bytes).
 * Sound: the firmware player plays a virtual file of the elf (PlayFile); how its bytes are made is the
 * backend's (backend.h: be_elka.c / be_sg.c).
 * Screen: own main screen (screen.c, CreateGUI; drawn only in the MMI, at most a few times a second and only
 * when something shown changed) + the firmware's own menus for stations and settings (menus.c).
 */
#include "app.h"
#include "net.h"
#include "build_id.h"

#define IPC_NAME	"FasoleyRadio"
#define IPC_OLD		"RadioElf"	/* the name before the rename (Radio.elf D41-): one copy across both */
#define IPC_CHECK_DOUBLERUN	1
#define IPC_CLOSE	2
#define TICK		22	/* ~0.1 s */

MAIN_CSM *main_csm;
static GBSTMR tick_tmr, quit_tmr;
static IPC_REQ ipc_out, ipc_out_old, ipc_reply;
static int closing_double;
int app_started;		/* start_proc ran (a running copy did not close us) */

int fw;				/* VS_FW_* */
MP3S *dec;
char elf_dir[128];		/* without the last backslash */
int volume;

int pstate = P_IDLE;
int station = -1;		/* playing / wanted */
static int want = -1;		/* P_WAIT_OLD: station to start next, -1 = none */
static int want_reconnect;
int handle = -1, started;
static int start_ticks, wait_ticks, fail_starts, quiet;
static int was_live, dead_ticks, play_ticks, restarts;
static uint32_t reads0, last_reads __attribute__((unused));	/* not in the emulator */
int rate, chans, kbps;
const char *note;
uint32_t start_need;
static volatile int poll_queued;
static int last_cmd = -1, msg_count;	/* diagnostics: last PlayFile report (any handle), how many */
uint32_t rx_bps;
static uint32_t rx_last;
int rx_secs;

uint32_t stream_bps(void)
{
	int k = kbps > 0 ? kbps : radio.icy.br > 0 ? radio.icy.br : 128;
	return (uint32_t)k * 125;
}

/* ------------------------------------------------------------------ log file (diagnostics) */
/* radio-log.txt (be_log_path): written only from the MMI (tick, messages), never from the firmware's tasks.
 * RAM: a 4 KB text buffer flushed once a second (full = lines dropped and counted) + the hook's 5 KB event
 * ring (vstream.h). The file stops at LOG_MAX with a last line saying so. log_sync (ELKA backend): every
 * line to the file at once. */
#define LOG_BUF		4096
#define LOG_MAX		(500 * 1024)
static char log_buf[LOG_BUF];
static int log_len, log_dropped, log_off;
static uint32_t log_size;
static char log_path[160];
int log_sync;

/* no vsnprintf in the S65/SK65 libraries: rlog() is sprintf into a line buffer (lines stay < 300 chars) */
char log_line[400];
int log_n;

void log_head(void)
{
	uint32_t ms = radio_ms();
	log_n = sprintf(log_line, "%u.%03u ", (unsigned)(ms / 1000), (unsigned)(ms % 1000));
}

void log_commit(int n)
{
	n += log_n;
	if (log_off || !log_path[0])
		return;
	log_line[n++] = '\r';
	log_line[n++] = '\n';
	if (log_len + n > LOG_BUF) {
		log_dropped++;
		return;
	}
	memcpy(log_buf + log_len, log_line, n);
	log_len += n;
	if (log_sync)
		log_flush();
}

void log_flush(void)
{
	unsigned err;
	int h;

	if (log_off || !log_path[0] || (!log_len && !log_dropped))
		return;
	if (log_dropped && log_len + 40 <= LOG_BUF) {
		log_len += sprintf(log_buf + log_len, "(%d lines dropped: buffer full)\r\n", log_dropped);
		log_dropped = 0;
	}
	h = _open(log_path, A_ReadWrite + A_BIN + A_Create + A_Append, P_READ + P_WRITE, &err);
	if (h < 0) {
		log_len = 0;
		return;
	}
	if (log_size + log_len > LOG_MAX) {
		static const char cut[] = "(log stopped at 500 KB)\r\n";
		_write(h, (void *)cut, sizeof(cut) - 1, &err);
		log_off = 1;
	} else {
		_write(h, log_buf, log_len, &err);
		log_size += log_len;
	}
	_close(h, &err);
	log_len = 0;
}

int nlog_enabled;	/* radio-net.txt (net_elka.c) follows cfg.log too */

/* cfg.log: on = a fresh radio-log.txt from here on, off = nothing written */
void log_apply(void)
{
	unsigned err;
	nlog_enabled = cfg.log;
	if (!cfg.log) {
		log_path[0] = 0;
		log_len = 0;
		return;
	}
	if (log_path[0])
		return;
	be_log_path(log_path);
	_unlink(log_path, &err);
	log_size = 0;
	log_off = 0;
}

void log_events(void)
{
	while (vs_ev_rd != vs_ev_wr) {
		const VS_EV *e = &vs_ev[vs_ev_rd % VS_EV_N];
		switch (e->kind) {
		case EV_TAKEN:
			rlog("ev %u.%u TAKEN obj %X path %X size %X pos %X", e->t / 10, e->t % 10, (unsigned)e->obj,
			     (unsigned)e->a, (unsigned)e->b, (unsigned)e->r);
			break;
		case EV_OTHER: {
			char nm[48];
			const char *p = (const char *)e->a;
			int i = 0;
			if (e->a >= 0xA0000000u && e->a < 0xB0000000u)
				for (; i < 47 && p[i] >= ' ' && p[i] < 127; i++)
					nm[i] = p[i];
			nm[i] = 0;
			rlog("ev %u.%u other obj %X path %X '%s' size %X fill %X", e->t / 10, e->t % 10, (unsigned)e->obj,
			     (unsigned)e->a, nm, (unsigned)e->b, (unsigned)e->r);
			break;
		}
		case EV_TRACKFAIL:
			rlog("ev %u.%u TRACK FULL obj %X", e->t / 10, e->t % 10, (unsigned)e->obj);
			break;
		default:
			rlog("ev %u.%u %s %s obj %X a %X b %X r %X", e->t / 10, e->t % 10, e->kind == EV_STOCK ? "stock" : "ours",
			     be_slot_name(e->slot), (unsigned)e->obj, (unsigned)e->a, (unsigned)e->b, (unsigned)e->r);
			break;
		}
		vs_ev_rd++;
	}
	if (vs_ev_lost) {
		rlog("ev: %u events lost (ring full)", (unsigned)vs_ev_lost);
		vs_ev_lost = 0;
	}
}

/* ------------------------------------------------------------------ helper context */

static void op_poll(void)
{
	radio_poll();
	poll_queued = 0;
}

static void op_net(int event, int id)
{
	net_event(event, id);
	radio_poll();
}

static volatile int open_seq, open_done;	/* the ring belongs to the new station once open_done == open_seq */
static char open_url[ST_URL];			/* copied in the MMI: the station list may change meanwhile */

int open_pending(void)
{
	return open_done != open_seq;
}

/* the network ring at the size of the settings; only between stations (nothing reads or writes it now:
 * the old connection is closed, the decoder stopped and the firmware's stream objects gone) */
static void ring_alloc(void)
{
	uint32_t want_b = (uint32_t)cfg.ring_kb * 1024, sz;
	if (net_ring.buf && ring_size(&net_ring) == want_b)
		return;
	if (net_ring.buf)
		mfree(net_ring.buf);
	net_ring.buf = 0;
	for (sz = want_b; sz >= 16 * 1024; sz >>= 1)
		if ((net_ring.buf = malloc(sz)))
			break;
	net_ring.mask = net_ring.buf ? sz - 1 : 0;
	net_ring.rd = net_ring.wr = 0;
}

static void op_open(int seq)
{
	ring_alloc();
	if (!net_ring.buf) {
		radio.err = "Not enough memory for the buffer";
		radio.state = RS_ERROR;
	} else
		radio_open(open_url);
	open_done = seq;
}

static void op_close(void)
{
	radio_close();
}

static void op_quit(void)
{
	radio_close();
	if (dec) {
		mp3s_free(dec);
		mfree(dec);
		dec = 0;
	}
	if (net_ring.buf)
		mfree(net_ring.buf);
	net_ring.buf = 0;
	kill_elf();
}

static void poll_now(void)
{
	if (!poll_queued) {
		poll_queued = 1;
		SUBPROC((void *)op_poll);
	}
}

/* ------------------------------------------------------------------ player */

#ifdef RADIO_EMU
/* elfloader3-linux has no firmware player: a timer takes the stream at the real rate (the Obs task's / the
 * player's job) and writes the first 40 s of each start to <elf folder>\radio_emu_<rate>_<kbps>.<pcm|mp3> for the
 * tests to compare */
static GBSTMR emu_tmr;
static uint32_t emu_t0, emu_taken;
static int emu_fd = -1;
static uint8_t emu_buf[16384];

static void emu_take(GBSTMR *t)
{
	uint32_t want_b = (uint32_t)((uint64_t)(radio_ms() - emu_t0) * be_take_bps() / 1000), err;
	(void)t;
	if (handle < 0)
		return;
	while (emu_taken + sizeof(emu_buf) / 2 <= want_b) {
		int n = sizeof(emu_buf) / 2;
		mp3s_read(dec, emu_buf, n);
		if (emu_fd >= 0 && emu_taken < 40u * be_take_bps())
			_write(emu_fd, emu_buf, n, &err);
		emu_taken += n;
	}
	GBS_StartTimerProc(&emu_tmr, 10, emu_take);
}

static int emu_start(void)
{
	unsigned err;
	static char p[160];
	sprintf(p, "%s\\radio_emu_%d_%d.%s", elf_dir, rate, kbps, be_emu_ext);
	if (emu_fd >= 0)
		_close(emu_fd, &err);
	emu_fd = _open(p, A_ReadWrite + A_BIN + A_Create + A_Truncate, P_READ + P_WRITE, &err);
	emu_t0 = radio_ms();
	emu_taken = 0;
	handle = 1;
	GBS_StartTimerProc(&emu_tmr, 10, emu_take);
	return 1;
}

static void emu_stop(void)
{
	if (IsTimerProc(&emu_tmr))
		GBS_StopTimer(&emu_tmr);
	if (emu_fd >= 0) {
		unsigned err;
		_close(emu_fd, &err);
		emu_fd = -1;
	}
}
#endif

/* bytes to buffer again after running dry: resume_sec of the stream, but at most 3/4 of the ring (320k:
 * 2 s = 80 KB > the 64 KB ring - it never resumed, D41) */
static uint32_t resume_bytes(void)
{
	uint32_t b = stream_bps() * cfg.resume_sec, m = ring_size(&net_ring) * 3 / 4;
	return b < m ? b : m;
}

static int start_player(void)
{
	be_start_stream(resume_bytes());
	started = 0;
	start_ticks = 0;
	was_live = 0;
	dead_ticks = 0;
	play_ticks = 0;
	reads0 = vs_reads;
#ifdef RADIO_EMU
	return emu_start();
#endif
	if (!be_ready()) {
		handle = -1;
		return 0;
	}
	handle = be_play_file(VS_NAME, volume);
	rlog("PlayFile %s\\%s rate %d ch %d kbps %d vol %d -> handle %d", elf_dir, VS_NAME, rate, chans, kbps, volume,
	     handle);
	if (handle < 0) {
		handle = -1;
		return 0;
	}
	return 1;
}

static void stop_player(void)
{
	log_sync |= be_sync_log;	/* ELKA (D69): the stop line reaches the file before anything is stopped */
	rlog("stop_player handle %d", handle);
	if (handle >= 0) {
#ifndef RADIO_EMU
		PlayMelody_StopPlayback(handle);	/* EL71: only posts the stop to the player (A0636918) */
#endif
		handle = -1;
	}
	mp3s_stop(dec);
	vs_stop();		/* ELKA: reads on our handles = silent frames from here on (D72) */
	rlog("stop_player done, our handles open %d", vs_live());
}

/* stop what plays; then start st (-1 = stay stopped) */
static void go(int st, int reconnect)
{
	stop_player();
	if (reconnect || st < 0)
		SUBPROC((void *)op_close);
	want = st;
	want_reconnect = reconnect;
	if (st >= 0 && reconnect)
		strcpy(open_url, stations[st].url);
	if (st >= 0)
		station = st;
	wait_ticks = 0;
	start_need = 0;
	pstate = P_WAIT_OLD;
}

void play_station(int st)
{
	if (st < 0 || st >= nst)
		return;
	screen_st = st;
	if (!fw) {
		note = be_msg(BM_NO_FW);
		return;
	}
	rlog("station %d: %s", st, stations[st].url);
	note = 0;
	fail_starts = 0;
	restarts = 0;
	kbps = 0;
	rx_secs = 0;
	be_test_stop();
	go(st, 1);
}

void stop_all(void)
{
	go(-1, 1);
	station = -1;
	note = 0;
}

void set_volume(int v)
{
	volume = v < VOL_MIN ? VOL_MIN : v > VOL_MAX ? VOL_MAX : v;
	if (handle >= 0)
		be_volume(handle, volume);
}

/* the firmware stopped our stream (call, alarm, another sound, error) */
static void player_lost(int error)
{
	rlog("player_lost error %d started %d fail_starts %d restarts %d", error, started, fail_starts, restarts);
	handle = -1;
	mp3s_stop(dec);
	vs_stop();
	if (error && !started && ++fail_starts >= 3) {
		stop_all();
		note = be_msg(BM_NOT_PLAYING);
		return;
	}
	if (++restarts > 5) {
		stop_all();
		note = "Player keeps stopping";
		return;
	}
	if (IsCalling()) {
		pstate = P_CALL;
		quiet = 0;
		return;
	}
	go(station, 0);		/* start again on the same connection */
}

static void on_started(void)
{
	if (started)
		return;
	rlog("started (handle %d)", handle);
	started = 1;
	be_on_started(handle);
}

static void player_msg(GBS_MSG *msg)
{
	int cmd = msg->submess & 0xFFFF, h = (short)(msg->submess >> 16);

	last_cmd = cmd;
	msg_count++;
	rlog("report cmd %d (0x%X) handle %d (ours %d) data0 %X data1 %X pstate %d", cmd, cmd, h, handle,
	     (unsigned)msg->data0, (unsigned)msg->data1, pstate);

	if (h != handle || handle < 0 || pstate != P_PLAY)
		return;
	switch (cmd) {
	case M_SAE_EVENT_STARTED:
	case M_SAE_PLAYBACK_STARTED:
		on_started();
		break;
	case M_SAE_EVENT_ERROR:
	case M_SAE_PLAYBACK_ERROR:
		player_lost(1);
		break;
	case M_SAE_EVENT_DONE:
	case M_SAE_PLAYBACK_DONE:
	case M_SAE_EVENT_STOPPED:
	case M_SAE_AUDIO_EVENT_STOPPED:
		player_lost(0);
		break;
	}
}

/* the network gives less than half of what the stream needs (320k over bt-pan: 7 of 40 KB/s): hopeless, stop
 * with a message instead of looping. 50..90 %: plays with gaps, the screen shows the rate (RP 128k at 10 of
 * 16 KB/s must play, D48 refused it at < 80 %). Stopping instead of starting/underrunning/restarting (that
 * loop starved the phone into a watchdog reset on SK65, D41). The rate is the ring's fill rate while
 * buffering, or what arrives while playing (the ring is not full then). */
static char slow_note[64];
static int too_slow(uint32_t need_bps)
{
	if (rx_secs < (pstate == P_PLAY ? 10 : 5) || !need_bps || rx_bps * 2 >= need_bps)
		return 0;
	if (pstate == P_PLAY && ring_free(&net_ring) < 4096)
		return 0;	/* ring full: the rate is ours, not the network's */
	rlog("too slow: net %u B/s, stream %u B/s", (unsigned)rx_bps, (unsigned)need_bps);
	sprintf(slow_note, "Net %dK/s < stream %dK/s: too slow", (int)((rx_bps + 512) >> 10),
		(int)((need_bps + 512) >> 10));
	stop_all();
	note = slow_note;
	return 1;
}

/* buffered enough + format known: start the player */
static void try_start(void)
{
	uint32_t need, used = ring_used(&net_ring);
	int r, c, k;

	if (open_done == open_seq && radio.state == RS_ERROR) {
		note = radio.err;
		pstate = P_IDLE;
		station = -1;
		return;
	}
	if (open_done != open_seq)	/* op_open not run yet: ring/state are the old station's */
		return;
	if (radio.state != RS_STREAM && radio.state != RS_RETRY && radio.state != RS_HEADERS)
		return;
	if (!mp3s_probe(&net_ring, &r, &c, &k)) {
		if (ring_free(&net_ring) < 1024)	/* full of junk: not MP3 */
			net_ring.rd = net_ring.wr - 1024;
		return;
	}
	if (too_slow((uint32_t)(radio.icy.br > 0 ? radio.icy.br : k) * 125))
		return;
	need = (uint32_t)(radio.icy.br > 0 ? radio.icy.br : k) * 125 * cfg.start_sec;
	if (need < 4096)
		need = 4096;
	if (need > ring_size(&net_ring) * 3 / 4)
		need = ring_size(&net_ring) * 3 / 4;
	start_need = need;
	if (used < need)
		return;
	rate = r;
	chans = c;
	kbps = radio.icy.br > 0 ? radio.icy.br : k;
	if (start_player())
		pstate = P_PLAY;
	else {
		stop_all();
		note = "PlayFile did not start";
	}
}

static void player_tick(void)
{
	int live = vs_live();

	switch (pstate) {
	case P_WAIT_OLD:	/* the old stream objects must be gone before the decoder is reset */
		if (live && ++wait_ticks < 600)
			break;
		if (wait_ticks < 3 && ++wait_ticks)
			break;
		if (want < 0) {
			pstate = P_IDLE;
			break;
		}
		if (want_reconnect)
			SUBPROC((void *)op_open, ++open_seq);
		pstate = P_BUFFER;
		break;
	case P_BUFFER:
		try_start();
		break;
	case P_PLAY:
#ifndef RADIO_EMU
		if (live)
			was_live = 1;
		if (!started && ++start_ticks >= 13)	/* no STARTED within ~1.3 s: assume it runs */
			on_started();
		/* stream objects gone for 0.5 s (the firmware's probe object comes and goes before the real one) */
		if (was_live && !live && vs_reads == last_reads) {
			if (++dead_ticks >= 5) {
				player_lost(0);
				break;
			}
		} else
			dead_ticks = 0;
		last_reads = vs_reads;
		if (++play_ticks == 60 && vs_reads == reads0) {	/* 6 s and the player never asked for data */
			stop_all();
			note = be_msg(BM_NOT_READING);
			break;
		}
		if (play_ticks > 300)
			restarts = 0;
#endif
		if (radio.state == RS_ERROR) {	/* reconnect refused (HTTP error, bad redirect, no auto-reconnect) */
			const char *e = radio.err;
			stop_all();
			note = e;
			break;
		}
		if (dec->gaps >= 3 && too_slow((uint32_t)kbps * 125))
			break;
		if (dec->fmt_changed)		/* the station changed its format: new header */
			go(station, 1);
		break;
	case P_CALL:
		if (IsCalling())
			quiet = 0;
		else if (++quiet >= 10)
			go(station, 1);	/* fresh audio after the call */
		break;
	}
}

void apply_retry(void)
{
	radio.no_retry = !cfg.retry;
}

/* ------------------------------------------------------------------ CSM moves (s65-player, from bt-pan) */

#ifdef NEWSGOLD
#define MMICMD_ID(cmd) ((cmd)->id)
#else
#define MMICMD_ID(cmd) ((int)(cmd)->cmd_csm)
#endif

static int do_csm_move(CSMQ *csm_q, void *_cmd)
{
	MMICMD *cmd = _cmd;
	CSM_RAM *work = FindCSM(csm_q, MMICMD_ID(cmd));
	CSM_RAM *before = cmd->parent_id == -1 ? NULL : FindCSM(csm_q, cmd->parent_id);
	int *gui;

	if (!work || work == before || (cmd->parent_id != -1 && !before))
		return 1;
	if (before ? work->next == before : work == (CSM_RAM *)csm_q->csm.last)
		return 1;
	UnfocusGUI();
	if (work->prev)
		((CSM_RAM *)work->prev)->next = work->next;
	else
		csm_q->csm.first = work->next;
	if (work->next)
		((CSM_RAM *)work->next)->prev = work->prev;
	else
		csm_q->csm.last = work->prev;
	if (before) {
		work->next = before;
		work->prev = before->prev;
		if (before->prev)
			((CSM_RAM *)before->prev)->next = work;
		else
			csm_q->csm.first = work;
		before->prev = work;
	} else {
		work->prev = csm_q->csm.last;
		work->next = NULL;
		((CSM_RAM *)csm_q->csm.last)->next = work;
		csm_q->csm.last = work;
	}
	if ((gui = ((CSM_RAM *)csm_q->csm.last)->gui_ll.last))
		FocusGUI(gui[3]);
	GBS_SendMessage(MMI_CEPID, MSG_CSM_DESTROYED, 0, 30002, 0);
	return 1;
}

static void csm_move(int id, int below_id)
{
	CSMQ *csm_q = CSM_root()->csm_q;
	MMICMD *cmd;

	if (!FindCSMbyID(id) || !(cmd = malloc(sizeof(MMICMD))))
		return;
	zeromem(cmd, sizeof(MMICMD));
	cmd->csm_q = csm_q;
	cmd->flag1 = 5;
#ifdef NEWSGOLD
	cmd->prio = 0;
	cmd->id = id;
#else
	cmd->cmd_csm = (CSM_RAM *)id;
#endif
	cmd->parent_id = below_id;
	cmd->proc = do_csm_move;
	cmd->next = NULL;
	cmd->prev = csm_q->cmd.last;
	if (csm_q->cmd.last)
		((MMICMD *)csm_q->cmd.last)->next = cmd;
	else
		csm_q->cmd.first = cmd;
	csm_q->cmd.last = cmd;
}

void minimize(void)
{
	if (main_csm)
		csm_move(main_csm->csm.id, CSM_root()->idle_id);
}

void app_exit(void)
{
	menus_close();
	if (main_csm && main_csm->gui_id)
		GeneralFunc_flag1(main_csm->gui_id, 1);
}

/* ------------------------------------------------------------------ CSM */

static void tick(GBSTMR *t)
{
	static int n;
	(void)t;
	vs_now++;
	be_tick();
	poll_now();
	player_tick();
	log_events();
	if (++n % 10 == 0) {
		uint32_t rx = radio.rx, d = rx >= rx_last ? rx - rx_last : rx;
		rx_last = rx;
		if (radio.state != RS_STREAM)
			rx_secs = 0;
		else {
			rx_bps = rx_secs ? (rx_bps * 3 + d) / 4 : d;
			rx_secs++;
		}
		rlog("sec: pstate %d handle %d ring %u net %d rx %u | seen %u taken %u live %d reads %u nz %u | frames %u err %u gaps %u | msgs %d last %d",
		     pstate, handle, (unsigned)ring_used(&net_ring), radio.state, (unsigned)radio.rx, (unsigned)vs_seen,
		     (unsigned)vs_detects, vs_live(), (unsigned)vs_reads, (unsigned)vs_nonzero, dec ? (unsigned)dec->frames : 0,
		     dec ? (unsigned)dec->errors : 0, dec ? (unsigned)dec->gaps : 0, msg_count, last_cmd);
		be_second();
		log_flush();
	}
	screen_tick();
	menus_sync();
	GBS_StartTimerProc(&tick_tmr, TICK, tick);
}

/* ~0.1 s after the start: a running copy has answered IPC_CLOSE by now */
static void start_proc(GBSTMR *t)
{
	(void)t;
	if (closing_double || !main_csm)
		return;
	app_started = 1;
	log_sync = 1;		/* the start lines at once */
	log_apply();
	ring_alloc();
	dec = malloc(sizeof(MP3S));
	if (!net_ring.buf || !dec || !mp3s_init(dec, &net_ring)) {
		note = "Not enough memory";
		fw = VS_FW_NONE;
	} else {
		be_make_stream_file();	/* before the hook (ELKA: it makes our file read-only) */
		fw = be_install();
		rlog("Fasoley Radio %s, elf %s, fw %d (%s)", RADIO_BUILD_TAG, elf_dir, fw, vs_fail ? vs_fail : "ok");
		be_log_install();
		be_setup();
		log_flush();
	}
	log_sync = be_sync_log;
	if (fw && cfg.autoplay)
		play_station(screen_st);
	screen_redraw();
	GBS_StartTimerProc(&tick_tmr, TICK, tick);
}

static void maincsm_oncreate(CSM_RAM *data)
{
	MAIN_CSM *csm = (MAIN_CSM *)data;

	main_csm = csm;
	csm->csm.state = CSM_STATE_OPEN;
	csm->csm.unk1 = 0;
	ipc_out.name_to = IPC_NAME;
	ipc_out.name_from = (char *)csm->csm.id;
	ipc_out.data = 0;
	GBS_SendMessage(MMI_CEPID, MSG_IPC, IPC_CHECK_DOUBLERUN, &ipc_out);
	ipc_out_old = ipc_out;		/* an old Radio.elf still running (minimized) answers to its own name */
	ipc_out_old.name_to = IPC_OLD;
	GBS_SendMessage(MMI_CEPID, MSG_IPC, IPC_CHECK_DOUBLERUN, &ipc_out_old);
	cfg_load();
	volume = cfg.vol;
	apply_retry();
	stations_load();
	csm->gui_id = screen_create(csm);
	GBS_StartTimerProc(&tick_tmr, 20, start_proc);
}

/* exit: the firmware must have destroyed our stream objects before the table is restored and the code goes */
static void quit_wait(GBSTMR *t)
{
	static int n, grace;
	(void)t;
	be_quit_poll();
	log_events();
	if (n % 10 == 0)
		log_flush();
	if (vs_live() && ++n < 600) {		/* ~60 s; after that stay loaded rather than crash */
		if (n % 10 == 1)
			rlog("exit: waiting, our handles open %d", vs_live());
		GBS_StartTimerProc(&quit_tmr, TICK, quit_wait);
		return;
	}
	if (vs_live()) {
		rlog("exit: handles still open after 60 s - staying loaded (methods stay hooked)");
		log_flush();
		return;
	}
	/* D70: a little longer after the last fclose: a late call on our handle range must still find us */
	if (++grace < 20) {
		GBS_StartTimerProc(&quit_tmr, TICK, quit_wait);
		return;
	}
	if (!vs_uninstall())	/* another elf hooked over ours (ELKA: drive methods): stay loaded */
		return;
	if (vs_busy() && ++n < 600) {	/* a firmware caller still inside our methods */
		GBS_StartTimerProc(&quit_tmr, TICK, quit_wait);
		return;
	}
	log_events();
	rlog("exit");
	log_flush();
	SUBPROC((void *)op_quit);
}

static void maincsm_onclose(CSM_RAM *data)
{
	(void)data;
	if (IsTimerProc(&tick_tmr))
		GBS_StopTimer(&tick_tmr);
	be_close();
#ifdef RADIO_EMU
	emu_stop();
#endif
	main_csm = NULL;
	menus_close();
	screen_free();
	if (dec)
		stop_player();
	log_events();
	rlog("close (waiting for the firmware's stream objects: %d)", vs_live());
	log_flush();
	if (!app_started) {
		kill_elf();
		return;
	}
	GBS_StartTimerProc(&quit_tmr, TICK, quit_wait);
}

static int maincsm_onmessage(CSM_RAM *data, GBS_MSG *msg)
{
	MAIN_CSM *csm = (MAIN_CSM *)data;

	if (msg->msg == MSG_IPC && msg->data0) {
		IPC_REQ *ipc = msg->data0;
		if (ipc->name_to && (!strcmp(ipc->name_to, IPC_NAME) || !strcmp(ipc->name_to, IPC_OLD))) {
			if (msg->submess == IPC_CHECK_DOUBLERUN && (int)ipc->name_from != csm->csm.id) {
				csm_move(csm->csm.id, -1);	/* we are the running copy: to the top */
				ipc_reply.name_to = IPC_NAME;
				ipc_reply.name_from = ipc->name_from;
				ipc_reply.data = 0;
				GBS_SendMessage(MMI_CEPID, MSG_IPC, IPC_CLOSE, &ipc_reply);
			} else if (msg->submess == IPC_CLOSE && (int)ipc->name_from == csm->csm.id && !app_started) {
				closing_double = 1;		/* we are the new copy */
				if (csm->gui_id)
					GeneralFunc_flag1(csm->gui_id, 1);
				else
					csm->csm.state = CSM_STATE_CLOSED;
			}
		}
		return 1;
	}
	if (msg->msg == MSG_HELPER_TRANSLATOR && app_started && net_ours((int)msg->data1)) {
		SUBPROC((void *)op_net, (int)msg->data0, (int)msg->data1);
		return 1;
	}
	if (msg->msg == MSG_PLAYFILE_REPORT) {
		player_msg(msg);
		return 1;
	}
	if (msg->msg == MSG_GUI_DESTROYED && (int)msg->data0 == csm->gui_id) {
		csm->gui_id = 0;
		csm->csm.state = CSM_STATE_CLOSED;
	}
	return 1;
}

static void iconbar(short *num)
{
	(void)num;
}

static const int minus11 = -11;
static uint16_t maincsm_name_body[32];
static struct {
	CSM_DESC desc;
	WSHDR name;
	struct { char tag[8]; void (*cb)(short *); } iconbar;
} MAINCSM = {
	.desc = {
		maincsm_onmessage,
		maincsm_oncreate,
#ifdef NEWSGOLD
		0, 0, 0, 0,
#endif
		maincsm_onclose,
		sizeof(MAIN_CSM),
		1, &minus11
	},
	.name = { maincsm_name_body, NAMECSM_MAGIC1, NAMECSM_MAGIC2, 0x0, 31, 0 },
	.iconbar = { "IconBar", iconbar }
};

int main(char *exe, char *fname, void *p1)
{
	MAIN_CSM storage;
	char *s;
	(void)fname;
	(void)p1;
	if (exe && strlen(exe) < sizeof(elf_dir)) {
		strcpy(elf_dir, exe);
		s = strrchr(elf_dir, '\\');
		if (s)
			*s = 0;
		else
			strcpy(elf_dir, "0:\\ZBin\\Radio");
	} else
		strcpy(elf_dir, "0:\\ZBin\\Radio");
	zeromem(&storage, sizeof(storage));
	wsprintf(&MAINCSM.name, APP_NAME);
	CreateCSM(&MAINCSM.desc, &storage, 0);
	return 0;
}

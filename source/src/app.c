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

MAIN_CSM *main_csm;		/* the screen's CSM, NULL while closed (D92: the daemon CSM owns the rest) */
static GBSTMR tick_tmr, quit_tmr;
static IPC_REQ ipc_out, ipc_out_old, ipc_reply;
static int closing_double;
int app_started;		/* start_proc ran (a running copy did not close us) */

int fw;				/* VS_FW_* */
MP3S *dec;
char elf_dir[128];		/* without the last backslash */
int volume;

int pstate = P_IDLE;
uint32_t play_t0;
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
#ifdef RADIO_LOG
static int last_cmd = -1, msg_count;	/* diagnostics: last PlayFile report (any handle), how many */
#endif
uint32_t rx_bps;
static uint32_t rx_last;
int rx_secs;

uint32_t stream_bps(void)
{
	int k = kbps > 0 ? kbps : radio.icy.br > 0 ? radio.icy.br : 128;
	return (uint32_t)k * 125;
}

#ifdef RADIO_LOG
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


/* cfg.log: on = a fresh radio-log.txt from here on, off = nothing written */
void log_apply(void)
{
	unsigned err;
	if (!cfg.log) {
		log_path[0] = 0;
		log_len = 0;
		return;
	}
	if (log_path[0])
		return;
	be_log_path(log_path);
	{	/* D136: keep the previous run's log (a copy still exiting, or the run before a crash) */
		char old[64];
		int n = strlen(log_path);
		if (n > 4 && n < 56) {
			memcpy(old, log_path, n - 4);
			strcpy(old + n - 4, "-old.txt");
			_unlink(old, &err);
			fmove(log_path, old, &err);
		}
	}
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
#endif

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
	be_idle_guard(-1, 1);	/* EL71: the idle's stale sound handle (D121) */
	handle = be_play_file(VS_NAME, volume);
	rlog("player start %s rate %d ch %d kbps %d vol %d -> handle %d", VS_NAME, rate, chans, kbps, volume, handle);
	if (handle < 0) {
		handle = -1;
		return 0;
	}
	return 1;
}

static void stop_player(void)
{
#ifdef RADIO_LOG
	log_sync |= be_sync_log;	/* ELKA (D69): the stop line reaches the file before anything is stopped */
#endif
	rlog("stop_player handle %d", handle);
	if (handle >= 0) {
#ifndef RADIO_EMU
		be_stop_play(handle);	/* posts the stop (PlayFile: A0636918; ELKA Obs: Obs_Stop) */
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
	play_t0 = 0;
	be_test_stop();
	go(st, 1);
}

void stop_all(void)
{
	go(-1, 1);
	station = -1;
	note = 0;
	play_t0 = 0;
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

/* started / failed / ended (PlayFile report or the backend's own message, D129) */
static void player_event(int ev)
{
	if (handle < 0 || pstate != P_PLAY)
		return;
	if (ev == BE_EV_STARTED)
		on_started();
	else if (ev == BE_EV_ERROR)
		player_lost(1);
	else if (ev == BE_EV_DONE)
		player_lost(0);
}

static void player_msg(GBS_MSG *msg)
{
	int cmd = msg->submess & 0xFFFF, h = (short)(msg->submess >> 16);

#ifdef RADIO_LOG
	last_cmd = cmd;
	msg_count++;
#endif
	rlog("report cmd %d (0x%X) handle %d (ours %d) data0 %X data1 %X pstate %d", cmd, cmd, h, handle,
	     (unsigned)msg->data0, (unsigned)msg->data1, pstate);

	if (h != handle || handle < 0)
		return;
	switch (cmd) {
	case M_SAE_EVENT_STARTED:
	case M_SAE_PLAYBACK_STARTED:
		player_event(BE_EV_STARTED);
		break;
	case M_SAE_EVENT_ERROR:
	case M_SAE_PLAYBACK_ERROR:
		player_event(BE_EV_ERROR);
		break;
	case M_SAE_EVENT_DONE:
	case M_SAE_PLAYBACK_DONE:
	case M_SAE_EVENT_STOPPED:
	case M_SAE_AUDIO_EVENT_STOPPED:
		player_event(BE_EV_DONE);
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
	if (start_player()) {
		pstate = P_PLAY;
		if (!play_t0)
			play_t0 = radio_ms() | 1;
	}
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

/* ------------------------------------------------------------------ the two CSMs (D92)
 * The firmware closes CSMs it considers screens: DoIDLE (EL71 A09507BE -> A0A143EE, S65 A1140086) closes every
 * CSM above the idle one, top down (red key on the idle hook A0959400; S65 also the sleep path). D90 had one CSM
 * with the screen and the player: closed that way (or by anything else that closes the screen), the radio and its
 * playback were gone. Now (as btfiles D16):
 *  - the daemon CSM: no GUI, made at the bottom of the CSM list (below idle, out of DoIDLE's reach); it owns the
 *    player state machine, the network, the timers, the PlayFile reports and IPC; only Exit closes it;
 *  - the screen CSM: only the main screen (and the menus opened from it); may be closed by anyone at any time,
 *    the radio plays on; starting the elf again (IPC_CHECK_DOUBLERUN) opens it again. */

static CSM_RAM *dmn_csm;	/* NULL once closed */
static int dmn_id, quitting, quit_ticks, dmn_closing, scr_pending, keyhook;
static int scr_reopen_hidden, scr_hide;	/* D157: red key = minimize (the screen is made again below idle) */

void minimize(void)
{
	if (main_csm)
		csm_move(main_csm->csm.id, CSM_root()->idle_id);
}

/* menu "Exit": the screen goes, then the daemon (tick), then the elf (quit_wait) */
void app_exit(void)
{
	rlog("exit from the menu");
	quitting = 1;
	menus_close();
	if (main_csm && main_csm->gui_id)
		GeneralFunc_flag1(main_csm->gui_id, 1);
	else if (main_csm)
		main_csm->csm.state = CSM_STATE_CLOSED;
}

static const int minus11 = -11;
static uint16_t scr_name_body[32], dmn_name_body[32];

/* --- the screen CSM */

static void scr_oncreate(CSM_RAM *data)
{
	MAIN_CSM *csm = (MAIN_CSM *)data;

	main_csm = csm;
	scr_pending = 0;
	csm->csm.state = CSM_STATE_OPEN;
	csm->csm.unk1 = 0;
	csm->gui_id = screen_create(csm);
	rlog("screen CSM %d gui %d", csm->csm.id, csm->gui_id);
}

static void scr_onclose(CSM_RAM *data)
{
	(void)data;
	main_csm = NULL;
	scr_pending = 0;
	menus_close();
	screen_free();
	rlog("screen CSM closed%s", quitting ? "" : " (not Exit: the radio plays on, screen made again hidden)");
	if (!quitting)
		scr_reopen_hidden = 1;	/* D157: DoIDLE (red key) closed it: back below idle, listed by XTask */
}

static int scr_onmessage(CSM_RAM *data, GBS_MSG *msg)
{
	MAIN_CSM *csm = (MAIN_CSM *)data;

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

static struct {
	CSM_DESC desc;
	WSHDR name;
	struct { char tag[8]; void (*cb)(short *); } iconbar;
} SCRCSM = {
	.desc = {
		scr_onmessage,
		scr_oncreate,
#ifdef NEWSGOLD
		0, 0, 0, 0,
#endif
		scr_onclose,
		sizeof(MAIN_CSM),
		1, &minus11
	},
	.name = { scr_name_body, NAMECSM_MAGIC1, NAMECSM_MAGIC2, 0x0, 31, 0 },
	.iconbar = { "IconBar", iconbar }
};

/* the screen on top: made if closed, else moved there */
static void scr_open(void)
{
	MAIN_CSM storage;
	CSMQ *q = CSM_root()->csm_q;
	CSM_RAM *save;

	if (main_csm) {
		csm_move(main_csm->csm.id, -1);
		return;
	}
	if (scr_pending || quitting)
		return;
	scr_pending = 1;	/* NSG: CreateCSM is an MMI command, run later */
	zeromem(&storage, sizeof(storage));
	wsprintf(&SCRCSM.name, APP_NAME);
	/* the new CSM goes right above the one handling the current message (A0A149B6): from the daemon's
	 * onmessage that would be below the idle screen - so above the top one */
	save = q->current_msg_processing_csm;
	q->current_msg_processing_csm = q->csm.last;
	CreateCSM(&SCRCSM.desc, &storage, 0);
#ifndef RADIO_EMU	/* elfloader3 keeps "current" = the last CSM made and gives it every new GUI (menus) */
	q->current_msg_processing_csm = save;
#else
	(void)save;
#endif
}

/* --- volume keys while the idle screen is on top and our stream plays (EL71, D92)
 * The idle screen's key handler (A05C4134 -> A067222E) calls PlayMelody_StopPlayback(*(u16 *)(GetMissedEventsPtr()
 * + 0xA4)) on a key press: that word is the handle of the idle's own PlaySound (A05BD11C: PlaySound(0x25, ..)),
 * a SAE_S/STI handle, while PlayMelody_StopPlayback (A059B668) first looks it up as a play object id (A0636918).
 * D90 phone: a volume key on the idle screen while the radio played -> AuMa_Stop (A062F374) asserted 0x10
 * "auma_external_if" in cnv_rt_proc (Converter_STOP A0D6AB44 with an AuMa handle AuMa no longer knows).
 * The idle screen never gets those keys while we play: they change the radio's volume here instead. */
static int vol_taken;

#ifdef RADIO_LOG
static int idle_on_top(void)
{
	CSM_RAM *top = (CSM_RAM *)CSM_root()->csm_q->csm.last;
	if (top && top->id == CSM_root()->idle_id)
		return 1;
	return be_idle_ui_on_top();
}
#endif

#ifdef RADIO_EMU
/* tests/emu: 0x14 (camera) = the firmware's DoIDLE, 0x15 (side) = the elf started again */
static IPC_REQ emu_ipc;
static void emu_key(int key)
{
	if (key == 0x14) {
		int ids[16], n = 0, i;
		CSM_RAM *c;
		for (c = (CSM_RAM *)CSM_root()->csm_q->csm.last; c && c->id != CSM_root()->idle_id && n < 16;
		     c = (CSM_RAM *)c->prev)
			ids[n++] = c->id;
		rlog("emu: DoIDLE closes %d CSM(s) above idle", n);
		for (i = 0; i < n; i++)
			CloseCSM(ids[i]);
	} else {
		rlog("emu: started again");
		emu_ipc.name_to = IPC_NAME;
		emu_ipc.name_from = (char *)0x7777;
		emu_ipc.data = 0;
		GBS_SendMessage(MMI_CEPID, MSG_IPC, IPC_CHECK_DOUBLERUN, &emu_ipc);
	}
}
#define EMU_KEYS 1
#else
#define EMU_KEYS 0
#endif

static int key_hook(int key, int m)
{
#ifdef RADIO_EMU
	if (m == KEY_DOWN && (key == 0x14 || key == 0x15)) {
		emu_key(key);
		return KEYHOOK_BREAK;
	}
#endif
	if (key == RED_BUTTON) {
		/* D170: red on our screen or its menus (our CSM on top) = minimize. Passed on, the firmware's A0959400 calls DoIDLE(1), which closes every
		 * CSM above idle: our screen and other elfs' screens too (bt-pan's single CSM -> BT internet gone). */
		static int red_taken;
		if (m == KEY_DOWN && main_csm && !quitting && (void *)CSM_root()->csm_q->csm.last == (void *)main_csm) {
			red_taken = 1;
			minimize();
			return KEYHOOK_BREAK;
		}
		if (red_taken) {
			if (m == KEY_UP)
				red_taken = 0;
			return KEYHOOK_BREAK;
		}
		return KEYHOOK_NEXT;
	}
	if (key != VOL_UP_BUTTON && key != VOL_DOWN_BUTTON)
		return KEYHOOK_NEXT;
	/* D129: while we play with our screen not on top, VOL+/VOL- = the radio's volume +-1 (down and every long-press
	 * repeat) and every message of these keys is swallowed. The idle's own key hook A066F064 (AddKeybMsgHook at
	 * A066F52C) runs the key shortcut 0x1A/0x1C on VOL key-up when the media player is not on (IsPlayerOn A06A2078,
	 * MediaProc's registry, no SWI to join it), and that shortcut opens the volume popup (Aud_Volume_Csm A0641C28)
	 * whose OK crashed with our stream (D121). So our hook goes first: AddKeybMsgHook_end (12C, A0A1406E) puts it at
	 * the HEAD of the list on EL71 (12B appends), see start_proc. */
	if (be_idle_volkeys && (handle >= 0 || vs_live()) && !screen_on_top()) {
		if (m == KEY_DOWN || m == LONG_PRESS) {
			set_volume(volume + (key == VOL_UP_BUTTON ? 1 : -1));
			rlog("volume key %X msg %X: idle on top %d -> volume %d", key, m, idle_on_top(), volume);
			log_flush();
		}
		vol_taken = m != KEY_UP;
		return KEYHOOK_BREAK;
	}
	if (vol_taken && m == KEY_UP) {
		vol_taken = 0;
		return KEYHOOK_BREAK;
	}
	vol_taken = 0;
	return KEYHOOK_NEXT;
}

/* --- the daemon CSM */

/* D172: with the display off (sleep) the firmware lowers the CPU clock and the stream stutters. While we play,
 * hold one high-clock request: SetCpuClockTempHi(2) (A06252E0) counts requests per level ("UI-MSOCPU"),
 * SetCpuClockLow(2) gives ours back. Balanced: one up per play, one down at stop/exit. */
#if defined(RADIO_OBS) && !defined(RADIO_EMU)
static int cpu_hi;
#endif
static void cpu_clock(int want)
{
#if defined(RADIO_OBS) && !defined(RADIO_EMU)
	if (want == cpu_hi)
		return;
	cpu_hi = want;
	if (want)
		SetCpuClockTempHi(2);
	else
		SetCpuClockLow(2);
	rlog("cpu clock request %s", want ? "high" : "released");
#else
	(void)want;
#endif
}

static void tick(GBSTMR *t)
{
	static int n;
	(void)t;
	if (quitting && dmn_csm && !dmn_closing) {
		static int stopped;
		if (!stopped) {		/* D156: stop first; the Obs object's ConvDestroyed comes to this daemon's
					 * onMessage, so the daemon stays until it came (D155: closed first -> the event
					 * was lost, Exit waited 60 s, the elf stayed loaded) */
			stopped = 1;
			stop_all();
		}
		++quit_ticks;
		if ((main_csm && quit_ticks <= 20) || (vs_live() && quit_ticks <= 130))
			goto ticking;	/* obs_tick forgets an unconfirmed object after 10 s (methods given back) */
		dmn_closing = 1;	/* Exit: the screen is gone (or did not go in 2 s): the daemon now */
		rlog("exit: closing the daemon (screen %s)", main_csm ? "still open" : "closed");
		CloseCSM(dmn_id);
		return;
	}
ticking:
	cpu_clock(!quitting && (handle >= 0 || vs_live()));
	if (!quitting) {		/* D157 */
		if (scr_hide && main_csm) {
			scr_hide = 0;
			minimize();
			rlog("screen made again and hidden below idle");
		}
		if (scr_reopen_hidden && !main_csm && !scr_pending) {
			scr_reopen_hidden = 0;
			scr_hide = 1;
			scr_open();
		}
	}
	VS_DIAG(vs_now++;)
	if (handle >= 0 || vs_live())
		be_idle_guard(handle, 0);
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
#ifdef RADIO_LOG
#ifdef RADIO_EMU	/* tests/emu/bg.sh wants the line to end with "scr N" */
#define SEC_HEAP_FMT	""
#define SEC_HEAP
#else			/* FasoleyRadio_ELKA_dbg.elf: free heap */
#define SEC_HEAP_FMT	" | heap %d cpu %d MHz load %d"
#define SEC_HEAP	, GetFreeRamAvail(), (int)GetCPUClock(), (int)GetCPULoad()
#endif
		rlog("sec: pstate %d handle %d ring %u net %d rx %u | seen %u taken %u live %d reads %u nz %u | frames %u err %u gaps %u | msgs %d last %d | scr %d" SEC_HEAP_FMT,
		     pstate, handle, (unsigned)ring_used(&net_ring), radio.state, (unsigned)radio.rx, (unsigned)vs_seen,
		     (unsigned)vs_detects, vs_live(), (unsigned)vs_reads, (unsigned)vs_nonzero, dec ? (unsigned)dec->frames : 0,
		     dec ? (unsigned)dec->errors : 0, dec ? (unsigned)dec->gaps : 0, msg_count, last_cmd, main_csm != NULL
		     SEC_HEAP);
		be_second();
		log_flush();
#endif
	}
	screen_tick();
	menus_sync();
	GBS_StartTimerProc(&tick_tmr, TICK, tick);
}

/* ~0.1 s after the start: a running copy has answered IPC_CLOSE by now */
static void start_proc(GBSTMR *t)
{
	(void)t;
	if (closing_double || !dmn_csm)
		return;
	app_started = 1;
#ifdef RADIO_LOG
	log_sync = 1;		/* the start lines at once */
	log_apply();
#endif
	ring_alloc();
	dec = malloc(sizeof(MP3S));
	if (!net_ring.buf || !dec || !mp3s_init(dec, &net_ring)) {
		note = "Not enough memory";
		fw = VS_FW_NONE;
	} else {
		be_make_stream_file();	/* before the hook (ELKA: it makes our file read-only) */
		fw = be_install();
		rlog("Fasoley Radio %s, elf %s, fw %d (%s), daemon CSM %d", RADIO_BUILD_TAG, elf_dir, fw,
		     vs_fail ? vs_fail : "ok", dmn_id);
#ifdef RADIO_LOG
		be_log_install();
#endif
		be_setup();
		log_flush();
	}
#ifdef RADIO_LOG
	log_sync = be_sync_log;
#endif
	if (be_idle_volkeys || EMU_KEYS) {
		/* D129: first in the list (before the idle's hook); the single "first" slot may be taken (A0673E08's CSM) */
		keyhook = AddKeybMsgHook_end(key_hook) ? 2 : 0;
		if (!keyhook) {
			AddKeybMsgHook(key_hook);
			keyhook = 1;
		}
		rlog("key hook %s", keyhook == 2 ? "first (AddKeybMsgHook_end)" : "appended: the first slot is taken");
	}
	scr_open();
	if (fw && cfg.autoplay)
		play_station(screen_st);
	screen_redraw();
	GBS_StartTimerProc(&tick_tmr, TICK, tick);
}

static void dmn_oncreate(CSM_RAM *data)
{
	dmn_csm = data;
	dmn_id = data->id;
	data->state = CSM_STATE_OPEN;
	data->unk1 = 0;
	ipc_out.name_to = IPC_NAME;
	ipc_out.name_from = (char *)data->id;
	ipc_out.data = 0;
	GBS_SendMessage(MMI_CEPID, MSG_IPC, IPC_CHECK_DOUBLERUN, &ipc_out);
	ipc_out_old = ipc_out;		/* an old Radio.elf still running (minimized) answers to its own name */
	ipc_out_old.name_to = IPC_OLD;
	GBS_SendMessage(MMI_CEPID, MSG_IPC, IPC_CHECK_DOUBLERUN, &ipc_out_old);
	cfg_load();
	volume = cfg.vol;
	apply_retry();
	stations_load();
	screen_st = cfg.def >= 0 && cfg.def < nst ? cfg.def : 0;
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
	if (main_csm && ++n < 600) {	/* the screen CSM still there (it was asked to close) */
		GBS_StartTimerProc(&quit_tmr, TICK, quit_wait);
		return;
	}
	log_events();
	rlog("exit");
	log_flush();
	SUBPROC((void *)op_quit);
}

/* Exit, or the daemon closed by anyone else: everything stops, the screen goes, then the elf */
static void dmn_onclose(CSM_RAM *data)
{
	(void)data;
	dmn_csm = NULL;
	quitting = 1;
	cpu_clock(0);
	if (IsTimerProc(&tick_tmr))
		GBS_StopTimer(&tick_tmr);
	if (keyhook) {
		RemoveKeybMsgHook(key_hook);
		keyhook = 0;
	}
	be_close();
#ifdef RADIO_EMU
	emu_stop();
#endif
	menus_close();
	if (main_csm) {
		if (main_csm->gui_id)
			GeneralFunc_flag1(main_csm->gui_id, 1);
		else
			main_csm->csm.state = CSM_STATE_CLOSED;
	}
	if (dec)
		stop_player();
	log_events();
	rlog("daemon CSM closed%s (waiting for the firmware's stream objects: %d)", dmn_closing ? "" : " NOT by Exit",
	     vs_live());
	log_flush();
	if (!app_started) {
		kill_elf();
		return;
	}
	GBS_StartTimerProc(&quit_tmr, TICK, quit_wait);
}

static int dmn_onmessage(CSM_RAM *data, GBS_MSG *msg)
{
	/* the id compared in 16 bits: elfloader3 sign-extends 0xDEB0 (D92 emulator test) */
	if ((msg->msg & 0xFFFF) == (MSG_IPC & 0xFFFF) && msg->data0) {
		IPC_REQ *ipc = msg->data0;
		if (ipc->name_to && (!strcmp(ipc->name_to, IPC_NAME) || !strcmp(ipc->name_to, IPC_OLD))) {
			if (msg->submess == IPC_CHECK_DOUBLERUN && (int)ipc->name_from != data->id) {
				if (app_started) {	/* we are the running copy: the screen (again) on top */
					rlog("started again: screen %s", main_csm ? "to the top" : "opened");
					scr_reopen_hidden = scr_hide = 0;	/* D157: shown, not hidden */
					scr_open();
				}
				ipc_reply.name_to = IPC_NAME;
				ipc_reply.name_from = ipc->name_from;
				ipc_reply.data = 0;
				GBS_SendMessage(MMI_CEPID, MSG_IPC, IPC_CLOSE, &ipc_reply);
			} else if (msg->submess == IPC_CLOSE && (int)ipc->name_from == data->id && !app_started) {
				closing_double = 1;		/* we are the new copy */
				data->state = CSM_STATE_CLOSED;
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
	{
		int ev = be_on_msg(msg);	/* ELKA: our Obs object's messages (D129) */
		if (ev >= 0) {
			player_event(ev);
			return 1;
		}
	}
	return 1;
}

static struct {
	CSM_DESC desc;
	WSHDR name;
} DMNCSM = {
	.desc = {
		dmn_onmessage,
		dmn_oncreate,
#ifdef NEWSGOLD
		0, 0, 0, 0,
#endif
		dmn_onclose,
		sizeof(CSM_RAM),
		1, &minus11
	},
	.name = { dmn_name_body, NAMECSM_MAGIC1, NAMECSM_MAGIC2, 0x0, 31, 0 },
};

#ifdef RADIO_EMU
/* the phone puts the daemon right above csm.first (A0A149B6: after the CSM handling the current message, set
 * in main); elfloader3 always appends on top: relink it there by hand */
static void emu_to_bottom(int id)
{
	CSMQ *q = CSM_root()->csm_q;
	CSM_RAM *c = FindCSMbyID(id), *first = (CSM_RAM *)q->csm.first;
	if (!c || !first || first == c || first->next == c)
		return;
	((CSM_RAM *)c->prev)->next = c->next;
	if (c->next)
		((CSM_RAM *)c->next)->prev = c->prev;
	else
		q->csm.last = c->prev;
	c->next = first->next;
	c->prev = first;
	if (first->next)
		((CSM_RAM *)first->next)->prev = c;
	else
		q->csm.last = c;
	first->next = c;
}
#endif

int main(char *exe, char *fname, void *p1)
{
	CSM_RAM storage, *save;
	CSMQ *q;
	char *s;
	int id;
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
	/* the daemon at the bottom of the CSM list (as cpumon / btfiles: daemons do) */
	zeromem(&storage, sizeof(storage));
	wsprintf(&DMNCSM.name, APP_NAME " (player)");
	LockSched();
	q = CSM_root()->csm_q;
	save = q->current_msg_processing_csm;
	q->current_msg_processing_csm = q->csm.first;
	id = CreateCSM(&DMNCSM.desc, &storage, 0);
	q->current_msg_processing_csm = save;
	UnlockSched();
#ifdef RADIO_EMU
	emu_to_bottom(id);
#else
	(void)id;
#endif
	return 0;
}

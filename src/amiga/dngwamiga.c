/*
 * dngwamiga.c - dnetd's transport through the decnet-gateway tunnel, see
 * dngwamiga.h. Single task: every call comes from dnetd's main loop (the
 * bsdsocket.library base belongs to the task that opened it).
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <proto/bsdsocket.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>

#include "../dngw/dngw.h"
#include "dngwamiga.h"

/* 1.1.1978 00:00, AmigaDOS day 0, in Unix seconds */
#define AMIGA_EPOCH 252460800UL

struct Library *SocketBase;

static gwt_log_fn logfn;
static LONG sock = -1;
static struct dngw gw;
static uint32_t keyid;
static uint8_t key[32];
static char host[128];
static unsigned port;
static char epochfile[160];
static uint32_t saved_epoch;
static struct sockaddr_in gwaddr;
static BOOL have_addr, alive;
static ULONG last_tx, last_rx;                  /* ms */
static ULONG last_resolve;                      /* ms, when the last lookup ended */
static UBYTE *pkt;                              /* DNGW_MAXPKT */

#define RESOLVE_EVERY_MS 30000UL

/* The gateway's name is looked up in a process of its own: without a
   network gethostbyname() can block for minutes, and dnetd must go on
   serving its links (DN: would hang otherwise). */
struct rsv_msg {
	struct Message msg;
	char host[128];
	ULONG addr;
	BOOL ok;
};
static struct MsgPort *rsv_port;
static struct rsv_msg rsv;
static struct Process *rsv_proc;                /* set while a lookup runs */

static ULONG now_ms(void)
{
	struct timeval tv;

	GetSysTime(&tv);
	return tv.tv_secs * 1000 + tv.tv_micro / 1000;
}

/* the clock as Unix seconds; on an Amiga without a clock this is 1978,
   and the saved epoch takes over */
static uint32_t unix_time(void)
{
	struct DateStamp ds;

	DateStamp(&ds);
	return AMIGA_EPOCH + (uint32_t)ds.ds_Days * 86400 + (uint32_t)ds.ds_Minute * 60 +
	       (uint32_t)ds.ds_Tick / TICKS_PER_SECOND;
}

static BOOL read_file(const char *name, char *buf, LONG size)
{
	BPTR fh = Open((STRPTR)name, MODE_OLDFILE);
	LONG n;

	if (!fh)
		return FALSE;
	n = Read(fh, buf, size - 1);
	Close(fh);
	if (n < 0)
		return FALSE;
	buf[n] = 0;
	return TRUE;
}

static BOOL save_epoch(uint32_t epoch)
{
	char buf[16];
	int i = sizeof buf;
	BPTR fh;
	BOOL ok;

	buf[--i] = '\n';
	do {
		buf[--i] = (char)('0' + epoch % 10);
		epoch /= 10;
	} while (epoch);
	fh = Open((STRPTR)epochfile, MODE_NEWFILE);
	if (!fh) {
		logfn("dnetd: cannot write %s (error %ld)\n", (STRPTR)epochfile, IoErr());
		return FALSE;
	}
	ok = Write(fh, buf + i, sizeof buf - i) == (LONG)(sizeof buf - i);
	if (!Close(fh))
		ok = FALSE;
	if (!ok) {
		logfn("dnetd: cannot write %s (error %ld)\n", (STRPTR)epochfile, IoErr());
		return FALSE;
	}
	saved_epoch = dngw_epoch(&gw);
	return TRUE;
}

static void set_addr(ULONG a)
{
	memset(&gwaddr, 0, sizeof gwaddr);
	gwaddr.sin_len = sizeof gwaddr;
	gwaddr.sin_family = AF_INET;
	gwaddr.sin_port = htons(port);
	gwaddr.sin_addr.s_addr = a;
	have_addr = TRUE;
}

/* the lookup process: one message in, the answer back */
static void rsv_entry(void)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	struct Library *SocketBase;     /* its own opener, not dnetd's */
	struct rsv_msg *m;
	struct hostent *he;

	WaitPort(&me->pr_MsgPort);
	m = (struct rsv_msg *)GetMsg(&me->pr_MsgPort);
	m->ok = FALSE;
#ifdef RSV_TESTDELAY
	Delay(RSV_TESTDELAY * TICKS_PER_SECOND);        /* test: a lookup that hangs */
#endif
	if ((SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4)) != NULL) {
		he = gethostbyname((STRPTR)m->host);
		if (he && he->h_addrtype == AF_INET && he->h_length == 4) {
			memcpy(&m->addr, he->h_addr, 4);
			m->ok = TRUE;
		}
		CloseLibrary(SocketBase);
	}
	Forbid();                       /* gone before dnetd can unload this code */
	ReplyMsg(&m->msg);
}

static void resolve_start(void)
{
	ULONG a;

	if (rsv_proc)
		return;
	a = inet_addr((STRPTR)host);
	if (a != INADDR_NONE) {
		set_addr(a);
		last_resolve = now_ms();
		return;
	}
	strcpy(rsv.host, host);
	rsv.msg.mn_Node.ln_Type = NT_MESSAGE;
	rsv.msg.mn_ReplyPort = rsv_port;
	rsv.msg.mn_Length = sizeof rsv;
	rsv_proc = CreateNewProcTags(NP_Entry, (ULONG)rsv_entry,
	                             NP_Name, (ULONG)"dnetd name lookup",
	                             NP_StackSize, 16384, TAG_END);
	if (!rsv_proc) {
		logfn("dnetd: no process for the name lookup\n");
		last_resolve = now_ms();
		return;
	}
	PutMsg(&rsv_proc->pr_MsgPort, &rsv.msg);
}

/* a finished lookup: take its address; the next one 30 s after it */
static void resolve_done(void)
{
	if (!rsv_proc || !GetMsg(rsv_port))
		return;
	rsv_proc = NULL;
	last_resolve = now_ms();
	if (!rsv.ok) {
		logfn("dnetd: gateway %s: name not found\n", (STRPTR)host);
		return;
	}
	if (!have_addr || gwaddr.sin_addr.s_addr != rsv.addr) {
		set_addr(rsv.addr);
		gwt_send(NULL, 0);          /* say hello at the new address now */
	}
}

void gwt_send(const UBYTE *frame, ULONG len)
{
	ULONG n;

	if (sock < 0 || len > DNGW_MAXFRAME)
		return;
	/* a new epoch (after 2^32 packets) is kept before it is used */
	if (dngw_epoch(&gw) != saved_epoch && !save_epoch(dngw_epoch(&gw)))
		return;
	n = dngw_seal(&gw, frame, len, pkt);
	last_tx = now_ms();
	if (have_addr)
		sendto(sock, pkt, n, 0, (struct sockaddr *)&gwaddr, sizeof gwaddr);
}

int gwt_open(const char *linefile, UWORD want, UWORD *area, UWORD *node, gwt_log_fn log)
{
	char line[256], *e;
	unsigned a, n;
	ULONG saved = 0;
	BOOL have = FALSE;
	uint32_t epoch;
	LONG one = 1;
	int i;

	logfn = log;
	if (!read_file(linefile, line, sizeof line)) {
		logfn("dnetd: cannot read the gateway line %s\n", (STRPTR)linefile);
		return -1;
	}
	if (!dngw_parse_line(line, host, sizeof host, &port, &keyid, &a, &n, key)) {
		memset(line, 0, sizeof line);
		logfn("dnetd: %s is not a DNGW1 client line\n", (STRPTR)linefile);
		return -1;
	}
	memset(line, 0, sizeof line);
	*area = (UWORD)a;
	*node = (UWORD)n;
	if (want && want != (UWORD)((a << 10) | n)) {
		memset(key, 0, sizeof key);
		logfn("dnetd: ADDR %lu.%lu is not the gateway line's %lu.%lu\n",
		      (ULONG)(want >> 10), (ULONG)(want & 0x3ff), (ULONG)a, (ULONG)n);
		return -1;
	}

	strncpy(epochfile, linefile, sizeof epochfile - 6);
	epochfile[sizeof epochfile - 6] = 0;
	strcat(epochfile, "Epoch");
	if (read_file(epochfile, line, 16)) {
		for (e = line; *e >= '0' && *e <= '9'; e++)
			saved = saved * 10 + (ULONG)(*e - '0');
		have = e != line;
	}

	pkt = AllocVec(DNGW_MAXPKT, MEMF_ANY);
	rsv_port = CreateMsgPort();
	if (!pkt || !rsv_port)
		return -1;
	/* at boot the TCP/IP stack may still be starting */
	for (i = 0; i < 30 && !(SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4)); i++)
		Delay(TICKS_PER_SECOND);
	if (!SocketBase) {
		logfn("dnetd: no bsdsocket.library: TCP/IP must run before DECnet\n");
		return -1;
	}
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		logfn("dnetd: no UDP socket (error %ld)\n", Errno());
		return -1;
	}
	IoctlSocket(sock, FIONBIO, (char *)&one);

	epoch = dngw_start(&gw, keyid, key, unix_time(), saved, have);
	memset(key, 0, sizeof key);
	if (!save_epoch(epoch))
		return -1;                  /* never repeat counters */
	logfn("dnetd: tunnel to %s:%lu as %lu.%lu (epoch %lu)\n", (STRPTR)host,
	     (ULONG)port, (ULONG)a, (ULONG)n, (ULONG)epoch);
	last_rx = now_ms();
	resolve_start();                    /* a name: the keepalive follows its address */
	gwt_send(NULL, 0);                  /* keepalive at start (if the address is known) */
	return 0;
}

BOOL gwt_line_addr(const char *linefile, UWORD *area, UWORD *node)
{
	char line[256], h[128];
	unsigned p, a, n;
	uint32_t id;
	uint8_t k[32];
	BOOL ok;

	if (!read_file(linefile, line, sizeof line))
		return FALSE;
	ok = dngw_parse_line(line, h, sizeof h, &p, &id, &a, &n, k) != 0;
	memset(line, 0, sizeof line);
	memset(k, 0, sizeof k);
	*area = (UWORD)a;
	*node = (UWORD)n;
	return ok;
}

void gwt_close(void)
{
	if (rsv_proc) {
		BOOL back;

		/* a lookup still runs: ask it to give up (bsdsocket calls
		   break on Ctrl-C) and wait for it, its code is ours */
		Forbid();
		back = GetMsg(rsv_port) != NULL;
		if (!back)
			Signal(&rsv_proc->pr_Task, SIGBREAKF_CTRL_C);
		Permit();
		if (!back) {
			logfn("dnetd: waiting for the name lookup to end\n");
			while (!GetMsg(rsv_port))
				WaitPort(rsv_port);
		}
		rsv_proc = NULL;
	}
	if (rsv_port)
		DeleteMsgPort(rsv_port);
	rsv_port = NULL;
	if (sock >= 0)
		CloseSocket(sock);
	sock = -1;
	if (SocketBase)
		CloseLibrary(SocketBase);
	SocketBase = NULL;
	if (pkt)
		FreeVec(pkt);
	pkt = NULL;
	memset(&gw, 0, sizeof gw);
}

BOOL gwt_wait(ULONG *sigs)
{
	fd_set rd;
	LONG r;

	FD_ZERO(&rd);
	FD_SET(sock, &rd);
	r = WaitSelect(sock + 1, &rd, NULL, NULL, NULL, sigs);
	return r > 0 || r < 0;              /* on an error, try to read anyway */
}

BOOL gwt_recv(UBYTE **frame, ULONG *len)
{
	LONG n;
	unsigned l;

	while ((n = recv(sock, pkt, DNGW_MAXPKT, 0)) > 0) {
		if (!dngw_open(&gw, pkt, (unsigned)n, frame, &l))
			continue;               /* silently, as the protocol says */
		last_rx = now_ms();
		if (!alive) {
			logfn("dnetd: gateway %s answers\n", (STRPTR)host);
			alive = TRUE;
		}
		*len = l;
		return TRUE;
	}
	return FALSE;
}

void gwt_tick(void)
{
	ULONG t = now_ms();

	if (t - last_tx >= DNGW_KEEPALIVE_S * 1000UL)
		gwt_send(NULL, 0);
	if (alive && t - last_rx > DNGW_LOST_S * 1000UL) {
		logfn("dnetd: gateway %s not answering\n", (STRPTR)host);
		alive = FALSE;
	}
	/* no address yet (DNS not up at boot), or the gateway lost: a home
	   line's address may have changed. Keepalives go on to the old
	   address meanwhile, so the tunnel is back as soon as the net is. */
	resolve_done();
	if (!rsv_proc && t - last_resolve >= RESOLVE_EVERY_MS &&
	    (!have_addr || (!alive && t - last_rx > DNGW_LOST_S * 1000UL)))
		resolve_start();
}

const char *gwt_host(void)
{
	return host;
}

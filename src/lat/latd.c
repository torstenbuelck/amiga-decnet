/*
 * latd - LAT terminal sessions for AmigaOS (outgoing).
 *
 * Usage: latd [VERBOSE/N] [LOG=file]
 *        latd LIST
 *        latd STOP
 *
 *   VERBOSE  1: circuits and sessions, 2: messages, 3: every run message
 *   LOG      append the messages to file, opened for each line, instead
 *            of the output
 *   LIST     show the services the running latd knows
 *   STOP     end the running latd (as Ctrl-C) and wait until it is gone;
 *            return code 5 if none runs
 *
 * Gets the LAT frames from dnetd (DNC_TAPOPEN, dnetd 0.7 or later), runs
 * the portable LAT core (lat.c) and serves programs on its public port
 * "LAT" (see latmsg.h): dnterm.device's LAT units, and dnmaint, which
 * offers a service that terminal servers log in to. Ends with dnetd. Our
 * LAT node name is ENV:DECnet/Name (else AMIGA).
 *
 * Requires AmigaOS 2.04 (V37), runs on a 68000.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../amiga/dnmsg.h"
#include "lat.h"
#include "latmsg.h"

static const char version[] = "$VER: latd 0.1 (25.10.2026)";

#define TICK_US     40000       /* LAT core clock */
#define FRAMEBUF    1600
#define RING        8192        /* terminal output not yet read, per client */
#define MAXCL       (LAT_MAXCIRC * LAT_MAXSESS)
#define CONNECT_MS  30000UL
#define ANSWER_MS   30000UL     /* an incoming session waits this long */
#define MAXQUEUED   2           /* incoming sessions not yet taken */

struct Device *TimerBase;

struct client {
	int used;
	ULONG id;
	int sid;                        /* LAT session, -1: none (any more) */
	int gone;                       /* the session ended */
	LONG reason;
	struct DnMsg *pconnect;
	struct LatConnect want;         /* while waiting for the service */
	ULONG connect_deadline;
	struct DnMsg *pread;
	ULONG read_deadline;
	struct DnMsg *pwrite;
	LONG woff;
	int incoming;                   /* 1: waits for LTC_ACCEPT, 2: for LTC_ANSWER */
	ULONG answer_deadline;
	char node[17], port[17];
	UBYTE ring[RING];
	ULONG rhead, rcount;
};

static struct MsgPort *pubport, *netport, *wport, *tport;
static struct timerequest *treq;
static BOOL timer_open, tpending;
static struct DnMsg rmsg, wmsg;
static ULONG tap;
static BOOL read_out, dnetd_gone;
static UBYTE rframe[FRAMEBUF], wframe[FRAMEBUF];
static struct client *cls;       /* MAXCL of them, large: allocated */

/* the offered service (LTC_OFFER) */
static ULONG offer_id;          /* 0: none */
static struct DnMsg *paccept;   /* waiting LTC_ACCEPT */
static ULONG accept_deadline;
static char our_node[LAT_NAMELEN + 1];
static ULONG generation, now;
static const char *logfile;
static int verbose;

static void mlog(const char *fmt, ...)
{
	char buf[200];
	va_list ap;
	BPTR fh;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (logfile && (fh = Open((STRPTR)logfile, MODE_READWRITE)) != 0) {
		Seek(fh, 0, OFFSET_END);
		Write(fh, buf, strlen(buf));
		Close(fh);
	} else
		PutStr((STRPTR)buf);
}

/* ---- dnetd ---------------------------------------------------------------- */

static BOOL put_dnetd(struct DnMsg *m, struct MsgPort *reply)
{
	struct MsgPort *dn;

	m->dm_Msg.mn_Node.ln_Type = NT_MESSAGE;
	m->dm_Msg.mn_ReplyPort = reply;
	m->dm_Msg.mn_Length = sizeof(struct DnMsg);
	Forbid();
	dn = FindPort((STRPTR)DN_PORTNAME);
	if (dn)
		PutMsg(dn, &m->dm_Msg);
	Permit();
	return dn != NULL;
}

/* send and wait for the reply (dnetd answers these at once) */
static BOOL transact(struct DnMsg *m)
{
	if (!put_dnetd(m, wport)) {
		dnetd_gone = TRUE;
		return FALSE;
	}
	while (!GetMsg(wport))
		WaitPort(wport);
	return TRUE;
}

static void issue_read(void)
{
	if (read_out || dnetd_gone)
		return;
	rmsg.dm_Cmd = DNC_TAPREAD;
	rmsg.dm_Link = tap;
	rmsg.dm_Buf = rframe;
	rmsg.dm_Len = FRAMEBUF;
	rmsg.dm_Timeout = 0;
	read_out = put_dnetd(&rmsg, netport);
	if (!read_out)
		dnetd_gone = TRUE;
}

/* ---- LAT core callbacks ------------------------------------------------- */

static void env_send(void *ctx, const unsigned char *dst, const unsigned char *payload,
                     unsigned len)
{
	(void)ctx;
	if (dnetd_gone || len + 6 > FRAMEBUF)
		return;
	memcpy(wframe, dst, 6);
	memcpy(wframe + 6, payload, len);
	wmsg.dm_Cmd = DNC_TAPWRITE;
	wmsg.dm_Link = tap;
	wmsg.dm_Buf = wframe;
	wmsg.dm_Len = (LONG)len + 6;
	transact(&wmsg);
}

static struct client *client_of(int sid)
{
	int i;

	for (i = 0; i < MAXCL; i++)
		if (cls[i].used && cls[i].sid == sid)
			return &cls[i];
	return NULL;
}

static void reply(struct DnMsg *m, LONG result, LONG reason)
{
	m->dm_Result = result;
	m->dm_Reason = reason;
	ReplyMsg(&m->dm_Msg);
}

static void serve_read(struct client *c)
{
	struct DnMsg *m = c->pread;
	ULONG n, k;

	if (!m)
		return;
	if (c->rcount) {
		n = c->rcount < (ULONG)m->dm_Len ? c->rcount : (ULONG)m->dm_Len;
		/* in up to two pieces around the end of the ring */
		k = RING - c->rhead < n ? RING - c->rhead : n;
		CopyMem(c->ring + c->rhead, m->dm_Buf, k);
		if (k < n)
			CopyMem(c->ring, (UBYTE *)m->dm_Buf + k, n - k);
		c->rhead = (c->rhead + n) % RING;
		c->rcount -= n;
		c->pread = NULL;
		reply(m, (LONG)n, 0);
	} else if (c->gone) {
		c->pread = NULL;
		reply(m, DNR_CLOSED, c->reason);
	}
}

static void serve_write(struct client *c)
{
	struct DnMsg *m = c->pwrite;
	int n;

	if (!m)
		return;
	if (c->gone) {
		c->pwrite = NULL;
		reply(m, -1, c->reason);
		return;
	}
	if (c->sid < 0)
		return;
	n = lat_write(c->sid, (const unsigned char *)m->dm_Buf + c->woff,
	              (unsigned)(m->dm_Len - c->woff));
	if (n < 0) {
		c->pwrite = NULL;
		reply(m, -1, c->reason);
		return;
	}
	c->woff += n;
	if (c->woff >= m->dm_Len) {
		c->pwrite = NULL;
		reply(m, 0, 0);
	}
}

static void env_data(void *ctx, int sid, const unsigned char *buf, unsigned len)
{
	struct client *c = client_of(sid);
	ULONG pos, k;

	(void)ctx;
	if (verbose >= 2)
		mlog("latd: %ld bytes for session %ld (%s)\n", (LONG)len, (LONG)sid,
		     (STRPTR)(c ? (c->pread ? "read waiting" : "no read") : "no client"));
	if (!c)
		return;
	if (len > RING - c->rcount) {
		mlog("latd: session %ld: %ld bytes of output lost (not read)\n",
		     (LONG)sid, (LONG)(len - (RING - c->rcount)));
		len = RING - c->rcount;
	}
	pos = (c->rhead + c->rcount) % RING;
	k = RING - pos < len ? RING - pos : len;
	CopyMem((APTR)buf, c->ring + pos, k);
	if (k < len)
		CopyMem((APTR)(buf + k), c->ring, len - k);
	c->rcount += len;
	serve_read(c);
}

static void env_event(void *ctx, int sid, int ev, int reason)
{
	struct client *c = client_of(sid);

	(void)ctx;
	if (!c)
		return;
	if (ev == LAT_EV_UP) {
		if (c->pconnect) {
			c->pconnect->dm_Link = c->id;
			reply(c->pconnect, 0, 0);
			c->pconnect = NULL;
		}
		return;
	}
	c->sid = -1;
	if (c->incoming == 1) {
		c->used = 0;                    /* nobody took it */
		return;
	}
	if (c->pconnect) {
		/* nobody will ever see this id */
		reply(c->pconnect, -1, reason);
		c->used = 0;
		return;
	}
	c->gone = TRUE;
	c->reason = reason;
	serve_read(c);
	serve_write(c);
}

/* give a waiting incoming session to the waiting LTC_ACCEPT */
static void hand_out(struct client *c)
{
	struct DnMsg *m = paccept;
	struct LatIncoming *li = m->dm_Buf;

	paccept = NULL;
	memset(li, 0, sizeof *li);
	strcpy(li->li_Node, c->node);
	strcpy(li->li_Port, c->port);
	c->incoming = 2;
	m->dm_Id = c->id;
	reply(m, 1, 0);                 /* 0 would read as DNR_TIMEOUT */
}

static struct client *new_client(void)
{
	int i;

	for (i = 0; i < MAXCL; i++)
		if (!cls[i].used) {
			struct client *c = &cls[i];
			memset(c, 0, sizeof *c);
			c->used = 1;
			c->id = (ULONG)i | (++generation << 8);
			c->sid = -1;
			return c;
		}
	return NULL;
}

/* a terminal server starts a session to the offered service */
static void env_incoming(void *ctx, int sid, const char *node, const char *port)
{
	struct client *c;
	int i, queued = 0;

	(void)ctx;
	for (i = 0; i < MAXCL; i++)
		if (cls[i].used && cls[i].incoming == 1)
			queued++;
	if (!offer_id || queued >= MAXQUEUED || !(c = new_client())) {
		lat_answer(sid, offer_id ? 5 : LAT_R_NOSVC);
		return;
	}
	c->sid = sid;
	c->incoming = 1;
	c->answer_deadline = now + ANSWER_MS;
	strncpy(c->node, node, 16);
	strncpy(c->port, port, 16);
	mlog("latd: incoming session from %s/%s\n", (STRPTR)node, (STRPTR)port);
	if (paccept)
		hand_out(c);
}

static void env_log(void *ctx, const char *msg)
{
	(void)ctx;
	mlog("latd: %s\n", msg);
}

/* ---- requests ------------------------------------------------------------- */

static struct client *find_client(ULONG id)
{
	struct client *c = &cls[id & 0xff];

	if ((id & 0xff) >= MAXCL || !c->used || c->id != id)
		return NULL;
	return c;
}

/* start the session once the service is known; FALSE: not yet */
static BOOL try_connect(struct client *c)
{
	char *w = c->want.lc_Service;
	int sid;

	if (!lat_find_service(w)) {
		/* "ATDT HOWIE" from a phone book: the T (or P) is not part
		   of the name */
		if ((w[0] == 'T' || w[0] == 't' || w[0] == 'P' || w[0] == 'p') &&
		    lat_find_service(w[1] == ' ' ? w + 2 : w + 1))
			memmove(w, w[1] == ' ' ? w + 2 : w + 1, strlen(w) - (w[1] == ' ' ? 1 : 0));
		else
			return FALSE;
	}
	sid = lat_connect(c->want.lc_Service, c->want.lc_Port);
	if (sid < 0) {
		reply(c->pconnect, -1, -sid);
		c->pconnect = NULL;
		c->used = 0;
		return TRUE;
	}
	c->sid = sid;
	return TRUE;
}

static void do_connect(struct DnMsg *m)
{
	struct LatConnect *lc = m->dm_Buf;
	struct client *c;

	if (!lc || !lc->lc_Service[0]) {
		reply(m, -1, LAT_R_NOSVC);
		return;
	}
	c = new_client();
	if (!c) {
		reply(m, -1, 5);
		return;
	}
	c->want = *lc;
	c->want.lc_Service[16] = 0;
	c->want.lc_Port[16] = 0;
	c->pconnect = m;
	c->connect_deadline = now + (m->dm_Timeout > 0 ? (ULONG)m->dm_Timeout : CONNECT_MS);
	try_connect(c);
}

static void do_close(struct client *c)
{
	if (c->sid >= 0)
		lat_disconnect(c->sid);
	if (c->pread)
		reply(c->pread, DNR_CLOSED, LAT_R_USER);
	if (c->pwrite)
		reply(c->pwrite, -1, LAT_R_USER);
	c->used = 0;
}

/* LIST: one line per service */
static void do_list(struct DnMsg *m)
{
	const struct lat_service *v;
	char line[120];
	LONG n = 0, k;
	int i;

	for (i = 0; (v = lat_service(i)) != NULL; i++) {
		sprintf(line, "%-16s %-16s %3d  %s\n", v->name, v->node, v->rating, v->ident);
		k = (LONG)strlen(line);
		if (n + k > m->dm_Len)
			break;
		CopyMem(line, (UBYTE *)m->dm_Buf + n, k);
		n += k;
	}
	reply(m, n, 0);
}

/* withdraw the offer: reject what waits, stop announcing */
static void unoffer(void)
{
	int i;

	for (i = 0; i < MAXCL; i++)
		if (cls[i].used && cls[i].incoming) {
			if (cls[i].sid >= 0)
				lat_answer(cls[i].sid, 5);
			cls[i].used = 0;
		}
	if (paccept)
		reply(paccept, -1, LAT_R_NOSVC);
	paccept = NULL;
	offer_id = 0;
	lat_offer(NULL, NULL);
	mlog("latd: service withdrawn\n");
}

static void do_offer(struct DnMsg *m)
{
	const char *name = m->dm_Buf;

	if (offer_id) {
		reply(m, -1, 6);                /* in use */
		return;
	}
	if (!name || !*name)
		name = our_node;
	offer_id = 0x8000UL | (++generation << 16);
	lat_offer(name, "Amiga maintenance login");
	mlog("latd: offering service %s\n", (STRPTR)name);
	m->dm_Id = offer_id;
	reply(m, 0, 0);
}

static void do_accept(struct DnMsg *m)
{
	int i;

	if (!offer_id || m->dm_Link != offer_id || paccept || !m->dm_Buf) {
		reply(m, -1, LAT_R_NOSVC);
		return;
	}
	paccept = m;
	accept_deadline = now + (m->dm_Timeout > 0 ? (ULONG)m->dm_Timeout : 0x7fffffffUL);
	for (i = 0; i < MAXCL; i++)
		if (cls[i].used && cls[i].incoming == 1) {
			hand_out(&cls[i]);
			break;
		}
}

static void handle(struct DnMsg *m)
{
	struct client *c;

	switch (m->dm_Cmd) {
	case DNC_CONNECT:
		do_connect(m);
		return;
	case LTC_LIST:
		do_list(m);
		return;
	case LTC_OFFER:
		do_offer(m);
		return;
	case LTC_ACCEPT:
		do_accept(m);
		return;
	case LTC_UNOFFER:
		if (offer_id && m->dm_Link == offer_id)
			unoffer();
		reply(m, 0, 0);
		return;
	}
	c = find_client(m->dm_Link);
	if (verbose >= 2)
		mlog("latd: request %ld for %08lx%s, %ld bytes waiting\n", m->dm_Cmd, m->dm_Link,
		     (STRPTR)(c ? "" : " (unknown)"), c ? (LONG)c->rcount : 0L);
	if (!c) {
		reply(m, -1, LAT_R_USER);
		return;
	}
	switch (m->dm_Cmd) {
	case DNC_READ:
		if (c->pread) {
			reply(m, -1, 5);
			break;
		}
		c->pread = m;
		c->read_deadline = now + (m->dm_Timeout > 0 ? (ULONG)m->dm_Timeout : 0x7fffffffUL);
		serve_read(c);
		break;
	case DNC_WRITE:
		if (c->pwrite || m->dm_Len < 0) {
			reply(m, -1, 5);
			break;
		}
		c->pwrite = m;
		c->woff = 0;
		serve_write(c);
		break;
	case DNC_CLOSE:
		do_close(c);
		reply(m, 0, 0);
		break;
	case DNC_REASON:
		reply(m, 0, c->reason);
		break;
	case LTC_BREAK:
		reply(m, c->sid >= 0 && !lat_break(c->sid) ? 0 : -1, 0);
		break;
	case LTC_ANSWER:
		if (c->incoming != 2 || c->sid < 0) {
			reply(m, -1, c->reason);
			break;
		}
		c->incoming = 0;
		lat_answer(c->sid, (int)m->dm_Reason);
		if (m->dm_Reason)
			c->used = 0;            /* rejected: gone */
		reply(m, 0, 0);
		break;
	default:
		reply(m, -1, 5);
	}
}

static void tick(unsigned ms)
{
	struct client *c;
	int i;

	now += ms;
	lat_tick(ms);
	if (paccept && (LONG)(now - accept_deadline) >= 0) {
		struct DnMsg *m = paccept;
		paccept = NULL;
		reply(m, DNR_TIMEOUT, 0);
	}
	for (i = 0; i < MAXCL; i++) {
		c = &cls[i];
		if (!c->used)
			continue;
		if (c->incoming && (LONG)(now - c->answer_deadline) >= 0) {
			/* nobody took or answered it in time */
			if (c->sid >= 0)
				lat_answer(c->sid, 5);
			c->used = 0;
			continue;
		}
		if (c->pconnect && c->sid < 0 && try_connect(c))
			continue;
		if (c->pconnect && (LONG)(now - c->connect_deadline) >= 0) {
			struct DnMsg *m = c->pconnect;
			c->pconnect = NULL;
			if (c->sid >= 0)
				lat_disconnect(c->sid);
			c->used = 0;
			reply(m, -1, c->sid >= 0 ? LAT_R_NOANSWER : LAT_R_NOSVC);
			continue;
		}
		if (c->pread && (LONG)(now - c->read_deadline) >= 0) {
			struct DnMsg *m = c->pread;
			c->pread = NULL;
			reply(m, DNR_TIMEOUT, 0);
		}
		serve_write(c);
	}
}

/* ---- start and end ------------------------------------------------------ */

static BOOL latd_running(void)
{
	BOOL r;

	Forbid();
	r = FindPort((STRPTR)LAT_PORTNAME) != NULL;
	Permit();
	return r;
}

static int stop_latd(void)
{
	struct MsgPort *p;
	int i;

	Forbid();
	p = FindPort((STRPTR)LAT_PORTNAME);
	if (p && p->mp_SigTask)
		Signal((struct Task *)p->mp_SigTask, SIGBREAKF_CTRL_C);
	Permit();
	if (!p) {
		PutStr((STRPTR)"latd is not running\n");
		return RETURN_WARN;
	}
	for (i = 0; i < 200 && latd_running(); i++)
		Delay(5);
	if (latd_running()) {
		PutStr((STRPTR)"latd does not stop\n");
		return RETURN_ERROR;
	}
	PutStr((STRPTR)"latd stopped\n");
	return RETURN_OK;
}

/* LIST: ask the running latd */
static int list_services(void)
{
	static char text[4000];
	struct MsgPort *rp, *p;
	struct DnMsg m;

	rp = CreateMsgPort();
	if (!rp)
		return RETURN_FAIL;
	memset(&m, 0, sizeof m);
	m.dm_Msg.mn_Node.ln_Type = NT_MESSAGE;
	m.dm_Msg.mn_ReplyPort = rp;
	m.dm_Msg.mn_Length = sizeof m;
	m.dm_Cmd = LTC_LIST;
	m.dm_Buf = text;
	m.dm_Len = sizeof text - 1;
	Forbid();
	p = FindPort((STRPTR)LAT_PORTNAME);
	if (p)
		PutMsg(p, &m.dm_Msg);
	Permit();
	if (!p) {
		DeleteMsgPort(rp);
		PutStr((STRPTR)"latd is not running\n");
		return RETURN_WARN;
	}
	while (!GetMsg(rp))
		WaitPort(rp);
	DeleteMsgPort(rp);
	if (m.dm_Result <= 0) {
		PutStr((STRPTR)"No LAT services known (yet)\n");
		return RETURN_WARN;
	}
	text[m.dm_Result] = 0;
	PutStr((STRPTR)text);
	return RETURN_OK;
}

static void node_name(char *out)
{
	char buf[40];
	LONG n;
	int i, k = 0;

	n = GetVar((STRPTR)"DECnet/Name", (STRPTR)buf, sizeof buf, GVF_GLOBAL_ONLY);
	for (i = 0; n > 0 && i < n && k < LAT_NAMELEN; i++) {
		char ch = buf[i];
		if (ch >= 'a' && ch <= 'z')
			ch -= 32;
		if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '$' || ch == '_')
			out[k++] = ch;
	}
	if (!k)
		strcpy(out, "AMIGA");
	else
		out[k] = 0;
}

int main(void)
{
	static const char template[] = "VERBOSE/N,LOG/K,LIST/S,STOP/S";
	LONG args[4] = { 0, 0, 0, 0 };
	struct RDArgs *rda;
	struct DnTapInfo ti;
	struct lat_env env;
	struct timeval last, tv;
	struct DnMsg *m;
	int i, rc = RETURN_FAIL;
	BOOL stopping = FALSE;

	rda = ReadArgs((STRPTR)template, args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (STRPTR)"latd");
		return RETURN_FAIL;
	}
	if (args[2] || args[3]) {
		rc = args[3] ? stop_latd() : list_services();
		FreeArgs(rda);
		return rc;
	}
	if (latd_running()) {
		PutStr((STRPTR)"latd: already running\n");
		FreeArgs(rda);
		return RETURN_WARN;
	}
	logfile = (const char *)args[1];
	if (logfile) {
		BPTR fh = Open((STRPTR)logfile, MODE_NEWFILE);
		if (fh)
			Close(fh);
	}

	cls = AllocVec(sizeof(struct client) * MAXCL, MEMF_CLEAR);
	netport = CreateMsgPort();
	wport = CreateMsgPort();
	tport = CreateMsgPort();
	pubport = CreateMsgPort();
	treq = tport ? (struct timerequest *)CreateIORequest(tport, sizeof *treq) : NULL;
	if (!cls || !netport || !wport || !tport || !pubport || !treq)
		goto out;
	if (OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)treq, 0) != 0)
		goto out;
	timer_open = TRUE;
	TimerBase = treq->tr_node.io_Device;

	/* the LAT frames from dnetd, and the LAT multicast address */
	memset(&ti, 0, sizeof ti);
	ti.dti_Multicast[0] = 0x09;
	ti.dti_Multicast[2] = 0x2b;
	ti.dti_Multicast[5] = 0x0f;
	memset(&wmsg, 0, sizeof wmsg);
	wmsg.dm_Cmd = DNC_TAPOPEN;
	wmsg.dm_Len = LAT_ETHERTYPE;
	wmsg.dm_Buf = &ti;
	if (!transact(&wmsg)) {
		mlog("latd: DECnet is not running (start dnetd first)\n");
		goto out;
	}
	if (wmsg.dm_Result < 0) {
		mlog("latd: dnetd does not pass LAT frames (dnetd 0.7 or later needed,"
		     " or another latd runs)\n");
		goto out;
	}
	tap = wmsg.dm_Id;

	memset(&env, 0, sizeof env);
	env.send_frame = env_send;
	env.data = env_data;
	env.event = env_event;
	env.incoming = env_incoming;
	env.log = env_log;
	env.verbose = verbose = args[0] ? (int)*(LONG *)args[0] : 0;
	memcpy(env.mac, ti.dti_Address, 6);
	node_name(env.node);
	strcpy(our_node, env.node);
	strcpy(env.ident, "Amiga DECnet-Term");
	GetSysTime(&last);
	lat_init(&env, (unsigned)(last.tv_secs ^ last.tv_micro));

	Forbid();
	if (FindPort((STRPTR)LAT_PORTNAME)) {
		Permit();
		mlog("latd: already running\n");
		goto out;
	}
	pubport->mp_Node.ln_Name = (char *)LAT_PORTNAME;
	pubport->mp_Node.ln_Pri = 0;
	AddPort(pubport);
	Permit();
	mlog("latd: LAT node %s, Ctrl-C or latd STOP ends\n", (STRPTR)env.node);

	rc = RETURN_OK;
	issue_read();
	while (!dnetd_gone) {
		ULONG sig;
		struct DnMsg *r;

		if (!tpending) {
			treq->tr_node.io_Command = TR_ADDREQUEST;
			treq->tr_time.tv_secs = 0;
			treq->tr_time.tv_micro = TICK_US;
			SendIO((struct IORequest *)treq);
			tpending = TRUE;
		}
		sig = Wait((1UL << netport->mp_SigBit) | (1UL << tport->mp_SigBit) |
		           (1UL << pubport->mp_SigBit) | SIGBREAKF_CTRL_C);
		if (sig & SIGBREAKF_CTRL_C)
			break;
		while ((r = (struct DnMsg *)GetMsg(netport)) != NULL) {
			if (r != &rmsg)
				continue;
			read_out = FALSE;
			if (rmsg.dm_Result == DNR_CLOSED || rmsg.dm_Result < 0) {
				dnetd_gone = TRUE;
				break;
			}
			if (rmsg.dm_Result > 14)
				lat_input(rframe + 6, rframe, rframe + 14,
				          (unsigned)rmsg.dm_Result - 14);
			issue_read();
		}
		while ((m = (struct DnMsg *)GetMsg(pubport)) != NULL)
			handle(m);
		if (GetMsg(tport)) {
			ULONG ms;

			tpending = FALSE;
			GetSysTime(&tv);
			ms = (tv.tv_secs - last.tv_secs) * 1000 +
			     (tv.tv_micro / 1000) - (last.tv_micro / 1000);
			if ((LONG)ms < 0 || ms > 10000)
				ms = TICK_US / 1000;
			last = tv;
			tick((unsigned)ms);
		}
	}
	stopping = TRUE;
	mlog(dnetd_gone ? "latd: DECnet stopped, ending\n" : "latd: stopping\n");

out:
	if (pubport && pubport->mp_Node.ln_Name) {
		Forbid();
		RemPort(pubport);
		Permit();
	}
	if (stopping) {
		/* end the sessions properly while dnetd is still there */
		for (i = 0; i < MAXCL; i++)
			if (cls[i].used && cls[i].sid >= 0)
				lat_disconnect(cls[i].sid);
		for (i = 0; i < 10 && !dnetd_gone; i++) {
			Delay(2);
			lat_tick(40);
		}
		if (!dnetd_gone)
			lat_shutdown();
	}
	if (pubport)
		while ((m = (struct DnMsg *)GetMsg(pubport)) != NULL)
			reply(m, -1, LAT_R_USER);
	if (paccept)
		reply(paccept, -1, LAT_R_USER);
	paccept = NULL;
	if (cls)
		for (i = 0; i < MAXCL; i++) {
			struct client *c = &cls[i];
			if (!c->used)
				continue;
			if (c->pconnect)
				reply(c->pconnect, -1, LAT_R_USER);
			if (c->pread)
				reply(c->pread, DNR_CLOSED, LAT_R_USER);
			if (c->pwrite)
				reply(c->pwrite, -1, LAT_R_USER);
		}
	if (read_out) {
		/* dnetd gives the read back when the tap closes */
		if (!dnetd_gone) {
			wmsg.dm_Cmd = DNC_TAPCLOSE;
			wmsg.dm_Link = tap;
			transact(&wmsg);
		}
		if (!dnetd_gone)
			while (read_out) {
				WaitPort(netport);
				while ((m = (struct DnMsg *)GetMsg(netport)) != NULL)
					if (m == &rmsg)
						read_out = FALSE;
			}
	} else if (tap && !dnetd_gone) {
		wmsg.dm_Cmd = DNC_TAPCLOSE;
		wmsg.dm_Link = tap;
		transact(&wmsg);
	}
	if (tpending) {
		AbortIO((struct IORequest *)treq);
		WaitIO((struct IORequest *)treq);
	}
	if (timer_open)
		CloseDevice((struct IORequest *)treq);
	if (treq)
		DeleteIORequest((struct IORequest *)treq);
	if (tport)
		DeleteMsgPort(tport);
	if (pubport)
		DeleteMsgPort(pubport);
	if (wport)
		DeleteMsgPort(wport);
	if (netport && !read_out)
		DeleteMsgPort(netport);
	if (cls)
		FreeVec(cls);
	FreeArgs(rda);
	(void)version;
	return rc;
}

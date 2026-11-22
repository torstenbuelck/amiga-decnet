/*
 * dnetd - DECnet Phase IV endnode for AmigaOS: the portable core on a
 * SANA-II device.
 *
 * Usage: dnetd ADDR=area.node [DEVICE=uaenet.device] [UNIT=n] [VERBOSE]
 *              [LOG=file] [ROUTER=area.node]
 *        dnetd TRANSPORT=UDP [GATEWAY=file] [ADDR=area.node] ...
 *        dnetd CHECK
 *        dnetd STOP
 *   LOG appends each message to file, opened only for that line, so the
 *   log can be read while dnetd runs.
 *   ROUTER is the router to use while no router hello is heard, for
 *   networks that do not deliver DECnet multicast (many WLANs). A router
 *   heard on the network always wins. Empty = none.
 *   TRANSPORT=UDP: no network card, the frames go through Hans Hübner's
 *   decnet-gateway tunnel over the TCP/IP stack (bsdsocket.library, which
 *   must run first). GATEWAY is the file with the DNGW1 client line
 *   (default ENVARC:DECnet/Gateway); the counter epoch is kept in the
 *   same name + "Epoch". The node gets the line's address; an ADDR given
 *   as well must be the same. DEVICE and UNIT are not used.
 *   TRANSPORT=DIRECT (default) is the network card.
 *   CHECK: return code 0 if a dnetd runs, 5 (WARN) if not.
 *   STOP: ends the running dnetd (as Ctrl-C), however it was started,
 *   and waits until it is gone; 5 (WARN) if none runs.
 *   Both look for dnetd's public port, not for a command name, so they
 *   also find a dnetd started with a path.
 *
 * The device should accept the DECnet station address in
 * S2_CONFIGINTERFACE. If it keeps its own address (e.g. an unpatched
 * Amiberry uaenet.device), dnetd reopens it promiscuous and sends and
 * receives raw frames instead.
 *
 * Requires AmigaOS 2.04 (V37), runs on a 68000.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <devices/sana2.h>
#include <devices/timer.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <clib/alib_protos.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../core/dn.h"
#include "dnmsg.h"
#include "dnserver.h"
#include "s2buff.h"
#include "dngwamiga.h"

#define NREAD   32      /* a2065.device: VMS bursts of 9+ frames within 25 us */
#define NTREAD  8       /* reads for the tap's ethertype (LAT) */
#define BUFLEN  1518
#define TICK_US 100000

static const char version[] = "$VER: dnetd 0.9 (22.11.2026)";

struct Device *TimerBase;

static struct MsgPort *port, *wport, *tport;
static struct IOSana2Req *ctl, *wr, *rd[NREAD], *trd[NTREAD];
static UBYTE *rbuf[NREAD], *trbuf[NTREAD], *wbuf, tframe[BUFLEN];
static UWORD tap_type;          /* ethertype passed to a program, 0: none */
static struct timerequest *treq;
static BOOL dev_open, timer_open, tpending;
static BOOL rawmode;
static UBYTE mymac[6];
static ULONG started_secs;
static const char *logfile;
static struct TagItem *buffertags;
static const char *devname = "uaenet.device";
static BOOL udpmode;            /* TRANSPORT=UDP: the gateway tunnel */
static UBYTE uframe[BUFLEN];

/* All dnetd output goes through here: console or LOG file. */
static void out(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	BPTR fh;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (logfile && (fh = Open((STRPTR)logfile, MODE_READWRITE)) != 0) {
		Seek(fh, 0, OFFSET_END);
		Write(fh, buf, strlen(buf));
		Close(fh);
	} else {
		PutStr((STRPTR)buf);
	}
}

/* ---- core environment ---------------------------------------------------- */

/* Short frames are padded with zeros here: drivers fill up to the
   Ethernet minimum with whatever their buffers hold (a2065.device sent
   pieces of earlier frames, dn-handler's connect data among them) */
#define MINDATA 46

static void send_type(UWORD type, const UBYTE *dst, const UBYTE *payload, ULONG len)
{
	ULONG n = len < MINDATA ? MINDATA : len;

	if (len + 14 > BUFLEN)
		return;
	if (udpmode) {                  /* no padding: the gateway's LAN side pads */
		memcpy(uframe, dst, 6);
		memcpy(uframe + 6, mymac, 6);
		uframe[12] = type >> 8;
		uframe[13] = type & 0xff;
		memcpy(uframe + 14, payload, len);
		gwt_send(uframe, len + 14);
		return;
	}
	wr->ios2_Req.io_Command = CMD_WRITE;
	wr->ios2_PacketType = type;
	memcpy(wr->ios2_DstAddr, dst, 6);
	if (rawmode) {
		memcpy(wbuf, dst, 6);
		memcpy(wbuf + 6, mymac, 6);
		wbuf[12] = type >> 8;
		wbuf[13] = type & 0xff;
		memcpy(wbuf + 14, payload, len);
		memset(wbuf + 14 + len, 0, n - len);
		wr->ios2_Req.io_Flags = SANA2IOF_RAW;
		wr->ios2_DataLength = n + 14;
	} else {
		memcpy(wbuf, payload, len);
		memset(wbuf + len, 0, n - len);
		wr->ios2_Req.io_Flags = 0;
		wr->ios2_DataLength = n;
	}
	wr->ios2_Data = wbuf;
	BeginIO((struct IORequest *)wr);
	WaitIO((struct IORequest *)wr);
	if (wr->ios2_Req.io_Error)
		out("dnetd: write error %ld/%ld\n", (LONG)wr->ios2_Req.io_Error,
		       (LONG)wr->ios2_WireError);
}

static void env_send(void *ctx, const dn_u8 *dst, const dn_u8 *payload, unsigned len)
{
	(void)ctx;
	send_type(DN_ETHERTYPE, dst, payload, len);
}

static void *env_alloc(void *ctx, unsigned n)
{
	(void)ctx;
	return AllocVec(n, MEMF_ANY);
}

static void env_free(void *ctx, void *p)
{
	(void)ctx;
	FreeVec(p);
}

static void env_log(void *ctx, int level, const char *msg)
{
	struct timeval tv;
	ULONG s;

	(void)ctx;
	(void)level;
	GetSysTime(&tv);
	s = tv.tv_secs - started_secs;
	out("%5ld.%03ld %s\n", (LONG)s, (LONG)(tv.tv_micro / 1000), (STRPTR)msg);
}

/* ---- device handling ----------------------------------------------------------- */

static void queue_req(struct IOSana2Req *r, UBYTE *buf, UWORD type)
{
	r->ios2_Req.io_Command = CMD_READ;
	r->ios2_Req.io_Flags = rawmode ? SANA2IOF_RAW : 0;
	r->ios2_PacketType = type;
	r->ios2_DataLength = BUFLEN;
	r->ios2_Data = buf;
	BeginIO((struct IORequest *)r);       /* not SendIO: it clears io_Flags */
}

static void queue_read(int i)
{
	queue_req(rd[i], rbuf[i], DN_ETHERTYPE);
}

static BOOL open_device(const char *name, LONG unit, ULONG flags)
{
	ctl->ios2_BufferManagement = buffertags;
	if (OpenDevice((STRPTR)name, unit, (struct IORequest *)ctl, flags) != 0) {
		out("dnetd: cannot open %s unit %ld (error %ld)\n", (STRPTR)name, unit,
		       (LONG)ctl->ios2_Req.io_Error);
		return FALSE;
	}
	dev_open = TRUE;
	return TRUE;
}

/* Configure the DECnet address. FALSE if the device keeps its own. */
static BOOL configure(void)
{
	ctl->ios2_Req.io_Command = S2_CONFIGINTERFACE;
	memcpy(ctl->ios2_SrcAddr, mymac, 6);
	DoIO((struct IORequest *)ctl);          /* fails if already configured */
	ctl->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
	if (DoIO((struct IORequest *)ctl) != 0)
		return FALSE;
	return memcmp(ctl->ios2_SrcAddr, mymac, 6) == 0;
}

static void add_multicast(const UBYTE *mac)
{
	ctl->ios2_Req.io_Command = S2_ADDMULTICASTADDRESS;
	memcpy(ctl->ios2_SrcAddr, mac, 6);
	if (DoIO((struct IORequest *)ctl) != 0)
		out("dnetd: S2_ADDMULTICASTADDRESS: error %ld/%ld\n",
		       (LONG)ctl->ios2_Req.io_Error, (LONG)ctl->ios2_WireError);
}

/* a frame of the tap's ethertype: to the program as dst src type payload */
static void handle_tap_frame(struct IOSana2Req *r, UBYTE *buf)
{
	ULONG len = r->ios2_DataLength;

	if (rawmode) {
		if (len < 14 || (memcmp(buf, mymac, 6) != 0 && !(buf[0] & 1)) ||
		    memcmp(buf + 6, mymac, 6) == 0)
			return;
		srv_tap_input(buf, len);
		return;
	}
	if (len + 14 > BUFLEN)
		return;
	memcpy(tframe, r->ios2_DstAddr, 6);
	memcpy(tframe + 6, r->ios2_SrcAddr, 6);
	tframe[12] = tap_type >> 8;
	tframe[13] = tap_type & 0xff;
	memcpy(tframe + 14, buf, len);
	srv_tap_input(tframe, len + 14);
}

/* dnserver: start passing frames of another ethertype (once; further
   calls only check the type) and receive a multicast address */
int dnetd_tap_enable(UWORD type, const UBYTE *mcast)
{
	static const UBYTE zero[6];
	static UBYTE added[6];
	int i;

	if (type == DN_ETHERTYPE || (tap_type && tap_type != type))
		return -1;
	if (udpmode) {                  /* the gateway sends every DEC multicast */
		if (!tap_type)
			out("dnetd: passing ethertype %04lx to a program\n", (ULONG)type);
		tap_type = type;
		return 0;
	}
	if (!tap_type) {
		for (i = 0; i < NTREAD; i++) {
			struct IOSana2Req *r;
			UBYTE *b;

			/* only requests that went to the device are kept:
			   cleanup() waits for each of them */
			r = (struct IOSana2Req *)CreateIORequest(port, sizeof(struct IOSana2Req));
			b = AllocVec(BUFLEN, MEMF_ANY);
			if (!r || !b) {
				if (r)
					DeleteIORequest((struct IORequest *)r);
				if (b)
					FreeVec(b);
				break;
			}
			CopyMem(ctl, r, sizeof(struct IOSana2Req));
			r->ios2_Req.io_Message.mn_ReplyPort = port;
			trd[i] = r;
			trbuf[i] = b;
			queue_req(r, b, type);
		}
		if (i == 0)
			return -1;
		tap_type = type;
		out("dnetd: passing ethertype %04lx to a program\n", (ULONG)type);
	}
	if (mcast && memcmp(mcast, zero, 6) && memcmp(mcast, added, 6)) {
		add_multicast(mcast);
		memcpy(added, mcast, 6);
	}
	return 0;
}

void dnetd_tap_send(const UBYTE *dst, const UBYTE *payload, ULONG len)
{
	if (tap_type)
		send_type(tap_type, dst, payload, len);
}

const UBYTE *dnetd_address(void)
{
	return mymac;
}

static void handle_frame(struct IOSana2Req *r, UBYTE *buf)
{
	ULONG len = r->ios2_DataLength;

	if (rawmode) {
		/* promiscuous: keep only frames for us or multicasts */
		if (len < 14)
			return;
		if (memcmp(buf, mymac, 6) != 0 && !(buf[0] & 1))
			return;
		if (memcmp(buf + 6, mymac, 6) == 0)
			return;
		dn_input(buf + 6, buf, buf + 14, len - 14);
	} else {
		dn_input(r->ios2_SrcAddr, r->ios2_DstAddr, buf, len);
	}
}

/* TRUE if s is word, any case (ASCII) */
static BOOL is_word(const char *s, const char *word)
{
	for (; *s && *word; s++, word++)
		if ((*s | 0x20) != (*word | 0x20))
			return FALSE;
	return !*s && !*word;
}

/* a frame from the gateway: dst src type payload */
static void handle_gw_frame(UBYTE *f, ULONG len)
{
	UWORD type;

	if (len < 14 || (memcmp(f, mymac, 6) != 0 && !(f[0] & 1)) ||
	    memcmp(f + 6, mymac, 6) == 0)
		return;
	type = (f[12] << 8) | f[13];
	if (type == DN_ETHERTYPE)
		dn_input(f + 6, f, f + 14, len - 14);
	else if (tap_type && type == tap_type)
		srv_tap_input(f, len);
}

static BOOL parse_addr(const char *a, dn_u16 *out)
{
	LONG area = -1, v = 0;

	for (; *a; a++) {
		if (*a == '.' && area < 0) {
			area = v;
			v = 0;
		} else if (*a >= '0' && *a <= '9') {
			v = v * 10 + (*a - '0');
		} else {
			return FALSE;
		}
	}
	if (area < 1 || area > 63 || v < 1 || v > 1023)
		return FALSE;
	*out = (dn_u16)((area << 10) | v);
	return TRUE;
}

static void cleanup(void)
{
	BOOL stuck = FALSE;
	int i;

	if (tpending) {
		AbortIO((struct IORequest *)treq);
		WaitIO((struct IORequest *)treq);
		tpending = FALSE;
	}
	/* A read the driver does not give back (a2065.device) stays with it
	   for good: then its request, buffer, port, the hook stub and the
	   device stay open/allocated, see s2buff.h. */
	for (i = 0; i < NREAD; i++) {
		if (rd[i]) {
			if (!s2_abort((struct IORequest *)rd[i])) {
				stuck = TRUE;
				continue;
			}
			DeleteIORequest((struct IORequest *)rd[i]);
			rd[i] = NULL;
		}
		if (rbuf[i]) {
			FreeVec(rbuf[i]);
			rbuf[i] = NULL;
		}
	}
	for (i = 0; i < NTREAD; i++) {
		if (trd[i]) {
			if (!s2_abort((struct IORequest *)trd[i])) {
				stuck = TRUE;
				continue;
			}
			DeleteIORequest((struct IORequest *)trd[i]);
			trd[i] = NULL;
		}
		if (trbuf[i]) {
			FreeVec(trbuf[i]);
			trbuf[i] = NULL;
		}
	}
	if (stuck)
		out("dnetd: %s did not give back aborted reads, leaving them with it\n",
		    (STRPTR)devname);
	if (wr) {
		DeleteIORequest((struct IORequest *)wr);
		wr = NULL;
	}
	if (dev_open && !stuck) {
		CloseDevice((struct IORequest *)ctl);
		dev_open = FALSE;
	}
	gwt_close();
	if (timer_open)
		CloseDevice((struct IORequest *)treq);
	if (ctl && !stuck)
		DeleteIORequest((struct IORequest *)ctl);
	if (treq)
		DeleteIORequest((struct IORequest *)treq);
	if (wbuf)
		FreeVec(wbuf);
	if (tport)
		DeleteMsgPort(tport);
	if (wport)
		DeleteMsgPort(wport);
	if (stuck) {
		s2_orphan_port(port);
	} else {
		if (port)
			DeleteMsgPort(port);
		if (buffertags)
			s2buff_free(buffertags);
	}
}

static BOOL running(void)
{
	BOOL r;

	Forbid();
	r = FindPort((STRPTR)DN_PORTNAME) != NULL;
	Permit();
	return r;
}

/* STOP: Ctrl-C to the running dnetd, then wait up to 10 s for its end */
static int stop_dnetd(void)
{
	struct MsgPort *p;
	int i;

	Forbid();
	p = FindPort((STRPTR)DN_PORTNAME);
	if (p && p->mp_SigTask)
		Signal((struct Task *)p->mp_SigTask, SIGBREAKF_CTRL_C);
	Permit();
	if (!p) {
		PutStr((STRPTR)"dnetd is not running\n");
		return RETURN_WARN;
	}
	for (i = 0; i < 200 && running(); i++)
		Delay(5);
	if (running()) {
		PutStr((STRPTR)"dnetd does not stop\n");
		return RETURN_ERROR;
	}
	PutStr((STRPTR)"dnetd stopped\n");
	return RETURN_OK;
}

int main(void)
{
	static const char template[] = "ADDR,DEVICE/K,UNIT/K/N,VERBOSE/S,LOG/K,CHECK/S,STOP/S,ROUTER/K,TRANSPORT/K,GATEWAY/K";
	LONG args[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	const char *gwfile = "ENVARC:DECnet/Gateway";
	UWORD garea, gnode;
	struct RDArgs *rda;
	LONG unit = 0;
	dn_u16 addr = 0, router = 0;
	struct dn_env env;
	struct timeval last, now;
	ULONG srvsig;
	int i, rc = RETURN_FAIL;

	rda = ReadArgs((STRPTR)template, args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (STRPTR)"dnetd");
		return RETURN_FAIL;
	}
	if (args[5] || args[6]) {
		rc = args[6] ? stop_dnetd() : running() ? RETURN_OK : RETURN_WARN;
		FreeArgs(rda);
		return rc;
	}
	/* before anything else, so a second start leaves the log alone */
	if (running()) {
		PutStr((STRPTR)"dnetd: already running\n");
		FreeArgs(rda);
		return RETURN_WARN;
	}
	if (args[8] && *(const char *)args[8]) {
		if (is_word((const char *)args[8], "UDP"))
			udpmode = TRUE;
		else if (!is_word((const char *)args[8], "DIRECT")) {
			PutStr((STRPTR)"dnetd: TRANSPORT is DIRECT or UDP\n");
			FreeArgs(rda);
			return RETURN_FAIL;
		}
	}
	if (args[9] && *(const char *)args[9])
		gwfile = (const char *)args[9];
	if (!args[0] && !udpmode) {
		PutStr((STRPTR)"dnetd: ADDR is required\n");
		FreeArgs(rda);
		return RETURN_FAIL;
	}
	logfile = (const char *)args[4];
	if (logfile) {
		BPTR fh = Open((STRPTR)logfile, MODE_NEWFILE);     /* start empty */
		if (fh)
			Close(fh);
	}
	if (args[0] && !parse_addr((const char *)args[0], &addr)) {
		out("dnetd: bad ADDR, use area.node (e.g. 1.10)\n");
		FreeArgs(rda);
		return RETURN_FAIL;
	}
	/* ROUTER="" (DECnet-Start without a router) means none */
	if (args[7] && *(const char *)args[7] &&
	    !parse_addr((const char *)args[7], &router)) {
		out("dnetd: bad ROUTER, use area.node (e.g. 1.1) or leave it empty\n");
		FreeArgs(rda);
		return RETURN_FAIL;
	}
	if (args[1])
		devname = (const char *)args[1];
	if (args[2])
		unit = *(LONG *)args[2];

	port = CreateMsgPort();
	wport = CreateMsgPort();
	tport = CreateMsgPort();
	if (!port || !wport || !tport)
		goto out;
	ctl = (struct IOSana2Req *)CreateIORequest(port, sizeof(struct IOSana2Req));
	treq = (struct timerequest *)CreateIORequest(tport, sizeof(struct timerequest));
	wbuf = AllocVec(BUFLEN, MEMF_ANY);
	buffertags = s2buff_create();
	if (!ctl || !treq || !wbuf || !buffertags)
		goto out;
	if (OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)treq, 0) != 0) {
		out("dnetd: cannot open timer.device\n");
		goto out;
	}
	timer_open = TRUE;
	TimerBase = treq->tr_node.io_Device;
	GetSysTime(&last);
	started_secs = last.tv_secs;

	if (udpmode) {
		if (gwt_open(gwfile, addr, &garea, &gnode, out))
			goto out;
		addr = (dn_u16)((garea << 10) | gnode);
		dn_node_mac(mymac, addr);
		goto core;
	}
	dn_node_mac(mymac, addr);
	if (!open_device(devname, unit, 0))
		goto out;
	if (!configure()) {
		out("dnetd: %s keeps its own station address, using promiscuous raw mode\n",
		       (STRPTR)devname);
		CloseDevice((struct IORequest *)ctl);
		dev_open = FALSE;
		if (!open_device(devname, unit, SANA2OPF_PROM))
			goto out;
		rawmode = TRUE;
	}
	add_multicast(dn_all_endnodes);

	wr = (struct IOSana2Req *)CreateIORequest(wport, sizeof(struct IOSana2Req));
	if (!wr)
		goto out;
	CopyMem(ctl, wr, sizeof(struct IOSana2Req));
	wr->ios2_Req.io_Message.mn_ReplyPort = wport;

	for (i = 0; i < NREAD; i++) {
		rd[i] = (struct IOSana2Req *)CreateIORequest(port, sizeof(struct IOSana2Req));
		rbuf[i] = AllocVec(BUFLEN, MEMF_ANY);
		if (!rd[i] || !rbuf[i])
			goto out;
		CopyMem(ctl, rd[i], sizeof(struct IOSana2Req));
		rd[i]->ios2_Req.io_Message.mn_ReplyPort = port;
		queue_read(i);
	}

core:
	memset(&env, 0, sizeof env);
	env.send_frame = env_send;
	env.alloc = env_alloc;
	env.free = env_free;
	env.log = env_log;
	env.loglevel = args[3] ? DN_LOG_DEBUG : DN_LOG_INFO;
	dn_init(&env, addr);
	if (router)
		dn_set_router(router);
	dn_seed_links((dn_u16)(last.tv_secs ^ (last.tv_micro >> 4)));
	dn_mirror_init();
	srvsig = srv_init();
	if (!srvsig) {
		out("dnetd: already running (or out of memory)\n");
		dn_shutdown();
		goto out;
	}
	if (udpmode)
		out("dnetd: node %s through gateway %s, Ctrl-C to stop\n",
		    (STRPTR)dn_ntoa(addr), (STRPTR)gwt_host());
	else
		out("dnetd: node %s on %s unit %ld%s, Ctrl-C to stop\n", (STRPTR)dn_ntoa(addr),
		    (STRPTR)devname, unit, rawmode ? " (raw)" : "");

	rc = RETURN_OK;
	for (;;) {
		ULONG sig;
		struct IOSana2Req *r;

		if (!tpending) {
			treq->tr_node.io_Command = TR_ADDREQUEST;
			treq->tr_time.tv_secs = 0;
			treq->tr_time.tv_micro = TICK_US;
			SendIO((struct IORequest *)treq);
			tpending = TRUE;
		}

		sig = (1UL << port->mp_SigBit) | (1UL << tport->mp_SigBit) | srvsig |
		      SIGBREAKF_CTRL_C;
		if (udpmode) {
			UBYTE *f;
			ULONG flen;

			if (gwt_wait(&sig))
				while (gwt_recv(&f, &flen))
					if (flen)
						handle_gw_frame(f, flen);
		} else {
			sig = Wait(sig);
		}
		if (sig & SIGBREAKF_CTRL_C) {
			out("dnetd: stopping\n");
			break;
		}
		while ((r = (struct IOSana2Req *)GetMsg(port)) != NULL) {
			for (i = 0; i < NREAD; i++)
				if (rd[i] == r)
					break;
			if (i == NREAD) {
				for (i = 0; i < NTREAD; i++)
					if (trd[i] == r)
						break;
				if (i == NTREAD)
					continue;
				if (r->ios2_Req.io_Error == 0)
					handle_tap_frame(r, trbuf[i]);
				queue_req(r, trbuf[i], tap_type);
				continue;
			}
			if (r->ios2_Req.io_Error == 0)
				handle_frame(r, rbuf[i]);
			queue_read(i);
		}
		if (sig & srvsig)
			srv_handle();
		srv_poll();
		if (GetMsg(tport)) {
			ULONG ms;

			tpending = FALSE;
			GetSysTime(&now);
			ms = (now.tv_secs - last.tv_secs) * 1000 +
			     (now.tv_micro / 1000) - (last.tv_micro / 1000);
			if ((LONG)ms < 0 || ms > 10000)
				ms = TICK_US / 1000;
			last = now;
			dn_tick((unsigned)ms);
			srv_tick((unsigned)ms);
			if (udpmode)
				gwt_tick();
		}
	}

	srv_shutdown();
	dn_shutdown();
out:
	cleanup();
	FreeArgs(rda);
	(void)version;
	return rc;
}

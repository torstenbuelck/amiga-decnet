/*
 * dnetd - DECnet Phase IV endnode for AmigaOS: the portable core on a
 * SANA-II device.
 *
 * Usage: dnetd ADDR=area.node [DEVICE=uaenet.device] [UNIT=n] [VERBOSE]
 *              [LOG=file]
 *   LOG appends each message to file, opened only for that line, so the
 *   log can be read while dnetd runs.
 *
 * The device should accept the DECnet station address in
 * S2_CONFIGINTERFACE. If it keeps its own address (e.g. an unpatched
 * Amiberry uaenet.device), dnetd reopens it promiscuous and sends and
 * receives raw frames instead.
 *
 * Requires AmigaOS 2.04 (V37), runs on a 68000.
 *
 * Copyright (C) 2026  Torsten
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
#include "dnserver.h"
#include "s2buff.h"

#define NREAD   32      /* a2065.device: VMS bursts of 9+ frames within 25 us */
#define BUFLEN  1518
#define TICK_US 100000

static const char version[] = "$VER: dnetd 0.4 (23.10.2026)";

struct Device *TimerBase;

static struct MsgPort *port, *wport, *tport;
static struct IOSana2Req *ctl, *wr, *rd[NREAD];
static UBYTE *rbuf[NREAD], *wbuf;
static struct timerequest *treq;
static BOOL dev_open, timer_open, tpending;
static BOOL rawmode;
static UBYTE mymac[6];
static ULONG started_secs;
static const char *logfile;
static struct TagItem *buffertags;
static const char *devname = "uaenet.device";

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

static void env_send(void *ctx, const dn_u8 *dst, const dn_u8 *payload, unsigned len)
{
	(void)ctx;
	if (len + 14 > BUFLEN)
		return;
	wr->ios2_Req.io_Command = CMD_WRITE;
	wr->ios2_PacketType = DN_ETHERTYPE;
	memcpy(wr->ios2_DstAddr, dst, 6);
	if (rawmode) {
		memcpy(wbuf, dst, 6);
		memcpy(wbuf + 6, mymac, 6);
		wbuf[12] = DN_ETHERTYPE >> 8;
		wbuf[13] = DN_ETHERTYPE & 0xff;
		memcpy(wbuf + 14, payload, len);
		wr->ios2_Req.io_Flags = SANA2IOF_RAW;
		wr->ios2_DataLength = len + 14;
	} else {
		memcpy(wbuf, payload, len);
		wr->ios2_Req.io_Flags = 0;
		wr->ios2_DataLength = len;
	}
	wr->ios2_Data = wbuf;
	BeginIO((struct IORequest *)wr);
	WaitIO((struct IORequest *)wr);
	if (wr->ios2_Req.io_Error)
		out("dnetd: write error %ld/%ld\n", (LONG)wr->ios2_Req.io_Error,
		       (LONG)wr->ios2_WireError);
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

static void queue_read(int i)
{
	rd[i]->ios2_Req.io_Command = CMD_READ;
	rd[i]->ios2_Req.io_Flags = rawmode ? SANA2IOF_RAW : 0;
	rd[i]->ios2_PacketType = DN_ETHERTYPE;
	rd[i]->ios2_DataLength = BUFLEN;
	rd[i]->ios2_Data = rbuf[i];
	BeginIO((struct IORequest *)rd[i]);   /* not SendIO: it clears io_Flags */
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

int main(void)
{
	static const char template[] = "ADDR/A,DEVICE/K,UNIT/K/N,VERBOSE/S,LOG/K";
	LONG args[5] = { 0, 0, 0, 0, 0 };
	struct RDArgs *rda;
	LONG unit = 0;
	dn_u16 addr;
	struct dn_env env;
	struct timeval last, now;
	ULONG srvsig;
	int i, rc = RETURN_FAIL;

	rda = ReadArgs((STRPTR)template, args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (STRPTR)"dnetd");
		return RETURN_FAIL;
	}
	logfile = (const char *)args[4];
	if (logfile) {
		BPTR fh = Open((STRPTR)logfile, MODE_NEWFILE);     /* start empty */
		if (fh)
			Close(fh);
	}
	if (!parse_addr((const char *)args[0], &addr)) {
		out("dnetd: bad ADDR, use area.node (e.g. 23.202)\n");
		FreeArgs(rda);
		return RETURN_FAIL;
	}
	if (args[1])
		devname = (const char *)args[1];
	if (args[2])
		unit = *(LONG *)args[2];
	dn_node_mac(mymac, addr);

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

	memset(&env, 0, sizeof env);
	env.send_frame = env_send;
	env.alloc = env_alloc;
	env.free = env_free;
	env.log = env_log;
	env.loglevel = args[3] ? DN_LOG_DEBUG : DN_LOG_INFO;
	dn_init(&env, addr);
	dn_mirror_init();
	srvsig = srv_init();
	if (!srvsig) {
		out("dnetd: already running (or out of memory)\n");
		dn_shutdown();
		goto out;
	}
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

		sig = Wait((1UL << port->mp_SigBit) | (1UL << tport->mp_SigBit) | srvsig |
		           SIGBREAKF_CTRL_C);
		if (sig & SIGBREAKF_CTRL_C) {
			out("dnetd: stopping\n");
			break;
		}
		while ((r = (struct IOSana2Req *)GetMsg(port)) != NULL) {
			for (i = 0; i < NREAD; i++)
				if (rd[i] == r)
					break;
			if (i == NREAD)
				continue;
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

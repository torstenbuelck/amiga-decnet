/*
 * dnhello - DECnet Phase IV endnode hello test for a SANA-II device.
 *
 * Configures the DECnet station address AA-00-04-00-xx-xx, listens for
 * router hellos on AB-00-00-04-00-00 (all endnodes), picks the designated
 * router and sends an Ethernet endnode hello to AB-00-00-03-00-00 (all
 * routers) every TIMER seconds, naming that router as neighbor. This is the
 * seed of the endnode routing layer (DECnet Routing Layer Functional
 * Specification V2.0.0, Ethernet endnode).
 *
 * Usage: dnhello ADDR=area.node [DEVICE=uaenet.device] [UNIT=n] [TIMER=s]
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
#include <clib/alib_protos.h>

#include <string.h>

#define NREAD      4
#define BUFLEN     1518
#define ETH_DECNET 0x6003
#define BLKSIZE    1498        /* Ethernet: 1500 minus the length field */

static const char version[] = "$VER: dnhello 0.1 (21.10.2026)";

static const UBYTE all_routers[6]  = { 0xab, 0x00, 0x00, 0x03, 0x00, 0x00 };
static const UBYTE all_endnodes[6] = { 0xab, 0x00, 0x00, 0x04, 0x00, 0x00 };

static BOOL CopyToBuff(register APTR to __asm("a0"),
                       register APTR from __asm("a1"),
                       register ULONG n __asm("d0"))
{
	CopyMem(from, to, n);
	return TRUE;
}

static BOOL CopyFromBuff(register APTR to __asm("a0"),
                         register APTR from __asm("a1"),
                         register ULONG n __asm("d0"))
{
	CopyMem(from, to, n);
	return TRUE;
}

static struct TagItem buffertags[] = {
	{ S2_CopyToBuff, (ULONG)CopyToBuff },
	{ S2_CopyFromBuff, (ULONG)CopyFromBuff },
	{ TAG_DONE, 0 }
};

/* Designated router as seen from this endnode. */
static struct {
	UWORD addr;         /* 0 = none */
	UBYTE priority;
	UWORD timer;        /* router's hello timer, seconds */
	ULONG lastheard;    /* our seconds counter */
	UWORD blksize;
} router;

static UBYTE mymac[6];
static UWORD myaddr;
static BOOL rawmode;    /* device ignores our station address */

static void node_mac(UBYTE *m, UWORD a)
{
	m[0] = 0xaa; m[1] = 0x00; m[2] = 0x04; m[3] = 0x00;
	m[4] = a & 0xff; m[5] = a >> 8;
}

static void print_node(UWORD a)
{
	Printf("%ld.%ld", (LONG)(a >> 10), (LONG)(a & 0x3ff));
}

static void print_mac(const UBYTE *m)
{
	Printf("%02lx-%02lx-%02lx-%02lx-%02lx-%02lx",
	       (ULONG)m[0], (ULONG)m[1], (ULONG)m[2],
	       (ULONG)m[3], (ULONG)m[4], (ULONG)m[5]);
}

/* Build the endnode hello. Returns the number of bytes in buf, which starts
   with the 2 byte little endian length field. */
static ULONG build_hello(UBYTE *buf, UWORD timer)
{
	UBYTE *m = buf + 2;
	ULONG n = 0;

	m[n++] = 0x0d;                          /* control, type 6: endnode hello */
	m[n++] = 2; m[n++] = 0; m[n++] = 0;     /* routing layer version 2.0.0 */
	memcpy(m + n, mymac, 6); n += 6;        /* ID */
	m[n++] = 0x03;                          /* IINFO: endnode */
	m[n++] = BLKSIZE & 0xff; m[n++] = BLKSIZE >> 8;
	m[n++] = 0;                             /* AREA (reserved) */
	memset(m + n, 0, 8); n += 8;            /* SEED (verification) */
	if (router.addr)
		node_mac(m + n, router.addr);   /* NEIGHBOR: designated router */
	else
		memset(m + n, 0, 6);
	n += 6;
	m[n++] = timer & 0xff; m[n++] = timer >> 8;
	m[n++] = 0;                             /* MPD (reserved) */
	m[n++] = 2; m[n++] = 0xaa; m[n++] = 0xaa;  /* test data */

	buf[0] = n & 0xff;
	buf[1] = n >> 8;
	return n + 2;
}

/* Handle a received routing layer message (starting at the length field).
   now = seconds since start. */
static void handle_routing(const UBYTE *p, ULONG len, ULONG now)
{
	ULONG i = 2;
	UBYTE flags;
	UWORD id, timer;
	UBYTE prio;

	if (len < 3)
		return;
	flags = p[2];
	if (flags & 0x80) {                     /* padding */
		i += flags & 0x7f;
		if (i >= len)
			return;
		flags = p[i];
	}
	if ((flags & 0x0f) != 0x0b)             /* only router hellos */
		return;
	/* flags, tiver[3], id[6], iinfo, blksize[2], priority, area, timer[2] */
	if (i + 18 > len)
		return;
	if (p[i + 4] != 0xaa || p[i + 5] != 0x00 || p[i + 6] != 0x04 || p[i + 7] != 0x00)
		return;
	id = p[i + 8] | (p[i + 9] << 8);
	prio = p[i + 13];
	timer = p[i + 15] | (p[i + 16] << 8);

	/* Designated router: highest priority, then highest address. */
	if (router.addr == 0 || id == router.addr || prio > router.priority ||
	    (prio == router.priority && id > router.addr)) {
		if (id != router.addr) {
			Printf("router ");
			print_node(id);
			Printf(" (prio %ld, hello %ld s, blksize %ld) is designated router\n",
			       (LONG)prio, (LONG)timer, (LONG)(p[i + 11] | (p[i + 12] << 8)));
		}
		router.addr = id;
		router.priority = prio;
		router.timer = timer ? timer : 15;
		router.blksize = p[i + 11] | (p[i + 12] << 8);
		router.lastheard = now;
	}
}

static void queue_read(struct IOSana2Req *r, UBYTE *buf)
{
	r->ios2_Req.io_Command = CMD_READ;
	r->ios2_Req.io_Flags = 0;
	r->ios2_PacketType = ETH_DECNET;
	r->ios2_DataLength = BUFLEN;
	r->ios2_Data = buf;
	BeginIO((struct IORequest *)r);
}

static LONG send_hello(struct IOSana2Req *w, UBYTE *buf, UWORD timer)
{
	ULONG len = build_hello(rawmode ? buf + 14 : buf, timer);

	w->ios2_Req.io_Command = CMD_WRITE;
	w->ios2_PacketType = ETH_DECNET;
	memcpy(w->ios2_DstAddr, all_routers, 6);
	if (rawmode) {
		/* Full frame with our DECnet MAC as source. */
		memcpy(buf, all_routers, 6);
		memcpy(buf + 6, mymac, 6);
		buf[12] = ETH_DECNET >> 8;
		buf[13] = ETH_DECNET & 0xff;
		len += 14;
		w->ios2_Req.io_Flags = SANA2IOF_RAW;
	} else {
		w->ios2_Req.io_Flags = 0;
	}
	w->ios2_DataLength = len;
	w->ios2_Data = buf;
	BeginIO((struct IORequest *)w);
	WaitIO((struct IORequest *)w);
	return w->ios2_Req.io_Error;
}

static BOOL parse_addr(const char *a, UWORD *out)
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
	*out = (UWORD)((area << 10) | v);
	return TRUE;
}

int main(void)
{
	static const char template[] = "ADDR/A,DEVICE/K,UNIT/K/N,TIMER/K/N";
	LONG args[4] = { 0, 0, 0, 0 };
	struct RDArgs *rda;
	const char *devname = "uaenet.device";
	LONG unit = 0;
	UWORD hellotimer = 15;
	struct MsgPort *port = NULL, *tport = NULL;
	struct IOSana2Req *ctl = NULL, *wr = NULL, *rd[NREAD];
	UBYTE *rbuf[NREAD], *wbuf = NULL;
	struct timerequest *treq = NULL;
	BOOL opened = FALSE, topened = FALSE, tpending = FALSE;
	ULONG now = 0, nexthello = 0, sent = 0;
	int i, rc = RETURN_FAIL;

	memset(rd, 0, sizeof rd);
	memset(rbuf, 0, sizeof rbuf);

	rda = ReadArgs((STRPTR)template, args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (STRPTR)"dnhello");
		return RETURN_FAIL;
	}
	if (!parse_addr((const char *)args[0], &myaddr)) {
		Printf("dnhello: bad ADDR, use area.node (e.g. 23.202)\n");
		goto out;
	}
	if (args[1])
		devname = (const char *)args[1];
	if (args[2])
		unit = *(LONG *)args[2];
	if (args[3])
		hellotimer = (UWORD)*(LONG *)args[3];
	node_mac(mymac, myaddr);

	port = CreateMsgPort();
	tport = CreateMsgPort();
	ctl = port ? (struct IOSana2Req *)CreateIORequest(port, sizeof(struct IOSana2Req)) : NULL;
	treq = tport ? (struct timerequest *)CreateIORequest(tport, sizeof(struct timerequest)) : NULL;
	wbuf = AllocVec(BUFLEN, MEMF_ANY | MEMF_CLEAR);
	if (!ctl || !treq || !wbuf) {
		Printf("dnhello: out of memory\n");
		goto out;
	}
	if (OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)treq, 0) != 0) {
		Printf("dnhello: cannot open timer.device\n");
		goto out;
	}
	topened = TRUE;

	ctl->ios2_BufferManagement = buffertags;
	if (OpenDevice((STRPTR)devname, unit, (struct IORequest *)ctl, 0) != 0) {
		Printf("dnhello: cannot open %s unit %ld (error %ld)\n",
		       devname, unit, (LONG)ctl->ios2_Req.io_Error);
		goto out;
	}
	opened = TRUE;

	/* Station address := DECnet MAC. */
	ctl->ios2_Req.io_Command = S2_CONFIGINTERFACE;
	memcpy(ctl->ios2_SrcAddr, mymac, 6);
	DoIO((struct IORequest *)ctl);       /* may fail if already configured */
	ctl->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
	if (DoIO((struct IORequest *)ctl) != 0 || memcmp(ctl->ios2_SrcAddr, mymac, 6) != 0) {
		Printf("station address is ");
		print_mac(ctl->ios2_SrcAddr);
		Printf(", device ignores ours: sending raw frames\n");
		rawmode = TRUE;
	}

	ctl->ios2_Req.io_Command = S2_ADDMULTICASTADDRESS;
	memcpy(ctl->ios2_SrcAddr, all_endnodes, 6);
	if (DoIO((struct IORequest *)ctl) != 0)
		Printf("S2_ADDMULTICASTADDRESS: error %ld/%ld\n",
		       (LONG)ctl->ios2_Req.io_Error, (LONG)ctl->ios2_WireError);

	wr = (struct IOSana2Req *)CreateIORequest(port, sizeof(struct IOSana2Req));
	if (!wr)
		goto out;
	CopyMem(ctl, wr, sizeof(struct IOSana2Req));
	wr->ios2_Req.io_Message.mn_ReplyPort = CreateMsgPort();   /* own port: synchronous */
	if (!wr->ios2_Req.io_Message.mn_ReplyPort)
		goto out;

	for (i = 0; i < NREAD; i++) {
		rd[i] = (struct IOSana2Req *)CreateIORequest(port, sizeof(struct IOSana2Req));
		rbuf[i] = AllocVec(BUFLEN, MEMF_ANY);
		if (!rd[i] || !rbuf[i]) {
			Printf("dnhello: out of memory\n");
			goto out;
		}
		CopyMem(ctl, rd[i], sizeof(struct IOSana2Req));
		rd[i]->ios2_Req.io_Message.mn_ReplyPort = port;
		queue_read(rd[i], rbuf[i]);
	}

	Printf("node ");
	print_node(myaddr);
	Printf(" (");
	print_mac(mymac);
	Printf(") on %s unit %ld, hello every %ld s, Ctrl-C to stop\n",
	       devname, unit, (LONG)hellotimer);

	rc = RETURN_OK;
	for (;;) {
		ULONG sig;
		struct IOSana2Req *r;

		if (!tpending) {
			treq->tr_node.io_Command = TR_ADDREQUEST;
			treq->tr_time.tv_secs = 1;
			treq->tr_time.tv_micro = 0;
			SendIO((struct IORequest *)treq);
			tpending = TRUE;
		}

		if (now >= nexthello) {
			LONG err = send_hello(wr, wbuf, hellotimer);
			sent++;
			if (err)
				Printf("hello %ld: write error %ld/%ld\n", (LONG)sent,
				       (LONG)err, (LONG)wr->ios2_WireError);
			else if (sent <= 3 || sent % 20 == 0) {
				Printf("hello %ld sent, router ", (LONG)sent);
				if (router.addr)
					print_node(router.addr);
				else
					Printf("none");
				Printf("\n");
			}
			nexthello = now + hellotimer;
		}

		sig = Wait((1UL << port->mp_SigBit) | (1UL << tport->mp_SigBit) | SIGBREAKF_CTRL_C);
		if (sig & SIGBREAKF_CTRL_C) {
			Printf("*** break\n");
			rc = RETURN_WARN;
			break;
		}
		if (GetMsg(tport)) {
			tpending = FALSE;
			now++;
			/* Router listener timer: 3 times its hello timer. */
			if (router.addr && now - router.lastheard > 3UL * router.timer) {
				Printf("router ");
				print_node(router.addr);
				Printf(" lost\n");
				router.addr = 0;
				nexthello = now;   /* tell the routers at once */
			}
		}
		while ((r = (struct IOSana2Req *)GetMsg(port)) != NULL) {
			for (i = 0; i < NREAD; i++)
				if (rd[i] == r)
					break;
			if (i == NREAD)
				continue;
			if (r->ios2_Req.io_Error == 0) {
				UWORD before = router.addr;
				handle_routing(rbuf[i], r->ios2_DataLength, now);
				if (router.addr != before)
					nexthello = now;   /* announce the new router */
			} else {
				Printf("read error %ld/%ld\n",
				       (LONG)r->ios2_Req.io_Error, (LONG)r->ios2_WireError);
			}
			queue_read(r, rbuf[i]);
		}
	}

out:
	if (tpending) {
		AbortIO((struct IORequest *)treq);
		WaitIO((struct IORequest *)treq);
	}
	for (i = 0; i < NREAD; i++) {
		if (rd[i] && opened && rd[i]->ios2_Req.io_Device && !CheckIO((struct IORequest *)rd[i])) {
			AbortIO((struct IORequest *)rd[i]);
			WaitIO((struct IORequest *)rd[i]);
		}
		if (rd[i])
			DeleteIORequest((struct IORequest *)rd[i]);
		if (rbuf[i])
			FreeVec(rbuf[i]);
	}
	if (wr) {
		if (wr->ios2_Req.io_Message.mn_ReplyPort && wr->ios2_Req.io_Message.mn_ReplyPort != port)
			DeleteMsgPort(wr->ios2_Req.io_Message.mn_ReplyPort);
		DeleteIORequest((struct IORequest *)wr);
	}
	if (opened)
		CloseDevice((struct IORequest *)ctl);
	if (topened)
		CloseDevice((struct IORequest *)treq);
	if (ctl)
		DeleteIORequest((struct IORequest *)ctl);
	if (treq)
		DeleteIORequest((struct IORequest *)treq);
	if (wbuf)
		FreeVec(wbuf);
	if (tport)
		DeleteMsgPort(tport);
	if (port)
		DeleteMsgPort(port);
	FreeArgs(rda);
	(void)version;
	return rc;
}

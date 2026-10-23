/*
 * s2dump - list Ethernet frames received by a SANA-II device.
 *
 * Opens the device promiscuous and reads raw frames with S2_READORPHAN, so
 * it sees every frame nobody else claims (Amiberry's uaenet.device drops
 * multicasts unless the unit is promiscuous). DECnet frames (0x6003 routing,
 * 0x6002 MOP) get a short decode.
 *
 * Usage: s2dump [DEVICE=uaenet.device] [UNIT=n] [COUNT=n] [ALL]
 *               [ADDR=area.node] [NOPROM]
 *   ALL     also list non-DECnet frames (default: only ethertypes 0x60xx)
 *   ADDR    configure the DECnet station address AA-00-04-00-xx-xx
 *   NOPROM  open non-promiscuous and register the DECnet multicasts
 *           (AB-00-00-03-00-00 all routers, AB-00-00-04-00-00 all endnodes)
 *
 * Copyright (C) 2026  Torsten
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <devices/sana2.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>

#include <string.h>

#define NREQ     4
#define FRAMELEN 1518

static const char version[] = "$VER: s2dump 0.2 (21.10.2026)";

/* SANA-II buffer management hooks: plain memcpy between our buffers and
   the device. Called by the device with arguments in registers. */
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

struct frame {
	struct IOSana2Req *req;
	UBYTE *buf;
};

static void print_mac(const UBYTE *m)
{
	Printf("%02lx-%02lx-%02lx-%02lx-%02lx-%02lx",
	       (ULONG)m[0], (ULONG)m[1], (ULONG)m[2],
	       (ULONG)m[3], (ULONG)m[4], (ULONG)m[5]);
}

/* DECnet node address from an AA-00-04-00-xx-xx MAC, or 0. */
static UWORD decnet_addr(const UBYTE *m)
{
	if (m[0] == 0xaa && m[1] == 0x00 && m[2] == 0x04 && m[3] == 0x00)
		return (UWORD)(m[4] | (m[5] << 8));
	return 0;
}

static void print_node(UWORD a)
{
	Printf("%ld.%ld", (LONG)(a >> 10), (LONG)(a & 0x3ff));
}

/* Short decode of a DECnet routing layer message (ethertype 0x6003).
   p points behind the Ethernet header: 2 byte length (LE), then the
   routing message, optionally preceded by padding (flag bit 7). */
static void decode_routing(const UBYTE *p, ULONG len)
{
	ULONG mlen, i = 2;
	UBYTE flags;

	if (len < 3)
		return;
	mlen = p[0] | (p[1] << 8);
	flags = p[2];
	if (flags & 0x80) {             /* padding */
		i += flags & 0x7f;
		if (i >= len)
			return;
		flags = p[i];
	}
	if (flags & 0x01) {             /* control message */
		switch ((flags >> 1) & 0x07) {
		case 0: Printf(" init"); break;
		case 1: Printf(" verify"); break;
		case 2: Printf(" hello-test"); break;
		case 3: Printf(" L1-routing"); break;
		case 4: Printf(" L2-routing"); break;
		case 5:
			Printf(" router-hello");
			/* flags, version[3], id[6], iinfo, blksize[2], priority, ... */
			if (i + 13 <= len)
				Printf(" v%ld.%ld.%ld blksize=%ld prio=%ld",
				       (LONG)p[i + 1], (LONG)p[i + 2], (LONG)p[i + 3],
				       (LONG)(p[i + 11] | (p[i + 12] << 8)),
				       i + 14 <= len ? (LONG)p[i + 13] : -1L);
			break;
		case 6:
			Printf(" endnode-hello");
			break;
		default:
			Printf(" control type %ld", (LONG)((flags >> 1) & 0x07));
		}
	} else {
		switch (flags & 0x07) {
		case 2: Printf(" data (short)"); break;
		case 6: Printf(" data (long)"); break;
		default: Printf(" data flags=%02lx", (ULONG)flags);
		}
	}
	Printf(" [len %ld]", (LONG)mlen);
}

static void show_frame(const UBYTE *f, ULONG len, BOOL all)
{
	UWORD type, a;

	if (len < 14)
		return;
	type = (f[12] << 8) | f[13];
	if (!all && (type & 0xff00) != 0x6000)
		return;

	print_mac(f + 6);
	if ((a = decnet_addr(f + 6)) != 0) {
		Printf(" (");
		print_node(a);
		Printf(")");
	}
	Printf(" > ");
	print_mac(f);
	Printf(" %04lx", (ULONG)type);
	switch (type) {
	case 0x6003:
		Printf(" DECnet");
		decode_routing(f + 14, len - 14);
		break;
	case 0x6002:
		Printf(" MOP-RC");
		break;
	case 0x6001:
		Printf(" MOP-DL");
		break;
	case 0x6004:
		Printf(" LAT");
		break;
	}
	Printf(" %ld bytes\n", (LONG)len);
}

static void queue_read(struct frame *fr)
{
	fr->req->ios2_Req.io_Command = S2_READORPHAN;
	fr->req->ios2_Req.io_Flags = SANA2IOF_RAW;
	fr->req->ios2_DataLength = FRAMELEN;
	fr->req->ios2_Data = fr->buf;
	/* BeginIO, not SendIO: SendIO clears io_Flags and loses SANA2IOF_RAW. */
	BeginIO((struct IORequest *)fr->req);
}

int main(void)
{
	static const char template[] = "DEVICE/K,UNIT/K/N,COUNT/K/N,ALL/S,ADDR/K,NOPROM/S";
	LONG args[6] = { 0, 0, 0, 0, 0, 0 };
	struct RDArgs *rda;
	const char *devname = "uaenet.device";
	LONG unit = 0, count = -1;
	BOOL all, noprom;
	LONG area = -1, node = -1;
	struct MsgPort *port = NULL;
	struct IOSana2Req *ctl = NULL;
	struct frame fr[NREQ];
	struct Sana2DeviceQuery query;
	UBYTE addr[SANA2_MAX_ADDR_BYTES];
	BOOL opened = FALSE;
	int i, nqueued = 0, rc = RETURN_FAIL;
	ULONG seen = 0;

	memset(fr, 0, sizeof fr);

	rda = ReadArgs((STRPTR)template, args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (STRPTR)"s2dump");
		return RETURN_FAIL;
	}
	if (args[0])
		devname = (const char *)args[0];
	if (args[1])
		unit = *(LONG *)args[1];
	if (args[2])
		count = *(LONG *)args[2];
	all = args[3] != 0;
	noprom = args[5] != 0;
	if (args[4]) {
		const char *a = (const char *)args[4];
		LONG v = 0;
		area = -1;
		for (; *a; a++) {
			if (*a == '.' && area < 0) {
				area = v;
				v = 0;
			} else if (*a >= '0' && *a <= '9') {
				v = v * 10 + (*a - '0');
			} else {
				area = -1;
				break;
			}
		}
		node = v;
		if (area < 1 || area > 63 || node < 1 || node > 1023) {
			Printf("s2dump: bad ADDR, use area.node (e.g. 23.202)\n");
			FreeArgs(rda);
			return RETURN_FAIL;
		}
	}

	port = CreateMsgPort();
	ctl = port ? (struct IOSana2Req *)CreateIORequest(port, sizeof(struct IOSana2Req)) : NULL;
	if (!ctl) {
		Printf("s2dump: out of memory\n");
		goto out;
	}
	ctl->ios2_BufferManagement = buffertags;

	if (OpenDevice((STRPTR)devname, unit, (struct IORequest *)ctl, noprom ? 0 : SANA2OPF_PROM) != 0) {
		Printf("s2dump: cannot open %s unit %ld (error %ld)\n",
		       devname, unit, (LONG)ctl->ios2_Req.io_Error);
		goto out;
	}
	opened = TRUE;

	memset(&query, 0, sizeof query);
	query.SizeAvailable = sizeof query;
	ctl->ios2_Req.io_Command = S2_DEVICEQUERY;
	ctl->ios2_StatData = &query;
	if (DoIO((struct IORequest *)ctl) == 0)
		Printf("%s unit %ld: hw type %ld, addr %ld bits, MTU %ld, %ld bps\n",
		       devname, unit, (LONG)query.HardwareType, (LONG)query.AddrFieldSize,
		       (LONG)query.MTU, (LONG)query.BPS);

	ctl->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
	if (DoIO((struct IORequest *)ctl) == 0) {
		memcpy(addr, ctl->ios2_SrcAddr, sizeof addr);
		Printf("station address ");
		print_mac(ctl->ios2_SrcAddr);
		Printf(", default ");
		print_mac(ctl->ios2_DstAddr);
		Printf("\n");

		/* A fresh unit must be configured before it goes online. */
		if (area > 0) {
			UWORD a = (UWORD)((area << 10) | node);
			addr[0] = 0xaa; addr[1] = 0x00; addr[2] = 0x04; addr[3] = 0x00;
			addr[4] = a & 0xff; addr[5] = a >> 8;
		}
		ctl->ios2_Req.io_Command = S2_CONFIGINTERFACE;
		memcpy(ctl->ios2_SrcAddr, addr, sizeof addr);
		if (DoIO((struct IORequest *)ctl) != 0)
			Printf("S2_CONFIGINTERFACE: error %ld/%ld\n", (LONG)ctl->ios2_Req.io_Error,
			       (LONG)ctl->ios2_WireError);
		ctl->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
		if (DoIO((struct IORequest *)ctl) == 0) {
			Printf("station address now ");
			print_mac(ctl->ios2_SrcAddr);
			Printf("\n");
		}
	}

	if (noprom) {
		static const UBYTE mc[2][6] = {
			{ 0xab, 0x00, 0x00, 0x03, 0x00, 0x00 },
			{ 0xab, 0x00, 0x00, 0x04, 0x00, 0x00 }
		};
		for (i = 0; i < 2; i++) {
			ctl->ios2_Req.io_Command = S2_ADDMULTICASTADDRESS;
			memcpy(ctl->ios2_SrcAddr, mc[i], 6);
			if (DoIO((struct IORequest *)ctl) != 0)
				Printf("S2_ADDMULTICASTADDRESS: error %ld/%ld\n",
				       (LONG)ctl->ios2_Req.io_Error, (LONG)ctl->ios2_WireError);
		}
	}

	for (i = 0; i < NREQ; i++) {
		fr[i].req = (struct IOSana2Req *)CreateIORequest(port, sizeof(struct IOSana2Req));
		fr[i].buf = AllocVec(FRAMELEN, MEMF_ANY);
		if (!fr[i].req || !fr[i].buf) {
			Printf("s2dump: out of memory\n");
			goto out;
		}
		/* Clone the opened request so all share the device and unit. */
		CopyMem(ctl, fr[i].req, sizeof(struct IOSana2Req));
		fr[i].req->ios2_Req.io_Message.mn_ReplyPort = port;
		queue_read(&fr[i]);
		nqueued++;
	}

	Printf("listening, Ctrl-C to stop\n");
	rc = RETURN_OK;
	while (count < 0 || seen < (ULONG)count) {
		ULONG sig = Wait((1UL << port->mp_SigBit) | SIGBREAKF_CTRL_C);
		struct IOSana2Req *r;

		if (sig & SIGBREAKF_CTRL_C) {
			Printf("*** break\n");
			rc = RETURN_WARN;
			break;
		}
		while ((r = (struct IOSana2Req *)GetMsg(port)) != NULL) {
			for (i = 0; i < NREQ; i++)
				if (fr[i].req == r)
					break;
			if (i == NREQ)
				continue;
			if (r->ios2_Req.io_Error == 0) {
				seen++;
				show_frame(fr[i].buf, r->ios2_DataLength, all);
			} else {
				Printf("read error %ld/%ld\n",
				       (LONG)r->ios2_Req.io_Error, (LONG)r->ios2_WireError);
			}
			if (count < 0 || seen < (ULONG)count)
				queue_read(&fr[i]);
			else
				nqueued--;
		}
	}

out:
	for (i = 0; i < NREQ; i++) {
		if (fr[i].req && opened && !CheckIO((struct IORequest *)fr[i].req)) {
			AbortIO((struct IORequest *)fr[i].req);
			WaitIO((struct IORequest *)fr[i].req);
		}
		if (fr[i].req)
			DeleteIORequest((struct IORequest *)fr[i].req);
		if (fr[i].buf)
			FreeVec(fr[i].buf);
	}
	if (opened)
		CloseDevice((struct IORequest *)ctl);
	if (ctl)
		DeleteIORequest((struct IORequest *)ctl);
	if (port)
		DeleteMsgPort(port);
	FreeArgs(rda);
	(void)nqueued;
	(void)version;
	return rc;
}

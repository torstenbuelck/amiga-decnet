/*
 * dncheck - check whether a SANA-II device can carry DECnet.
 *
 * Usage: dncheck DEVICE=name ADDR=area.node [UNIT=n] [WAIT=seconds]
 *
 * Tests, in this order, what dnetd needs from the driver:
 *   1. the device opens and is Ethernet
 *   2. S2_CONFIGINTERFACE takes the DECnet station address AA-00-04-00-xx-xx
 *      (best case: dnetd runs normally)
 *   3. S2_ADDMULTICASTADDRESS takes the DECnet "all endnodes" multicast
 *   4. if the address was not taken: the device opens promiscuous, which is
 *      dnetd's fallback (raw frames with our own source address)
 *   5. DECnet frames (ethertype 0x6003, e.g. router hellos) actually arrive
 *      within WAIT seconds (default 20, 0 skips the test; skipped as well
 *      while dnetd runs, which takes the frames itself)
 *
 * A SANA-II unit can be configured only once per boot, and whoever comes
 * first sets the address. Run dncheck before AmiTCP or any other network
 * software has started. After dncheck the card keeps the DECnet address
 * until the next reboot, which is what dnetd would do anyway.
 *
 * Return codes, for the installer:
 *    0  the driver takes the DECnet address, DECnet frames arrive
 *    5  usable with limits (fallback mode, unit configured by other
 *       software, or no DECnet frames seen); see the output
 *   10  the device cannot carry DECnet
 *   20  the device cannot be opened, or bad arguments
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
#include <dos/rdargs.h>
#include <devices/sana2.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>

#include <string.h>

#define DN_ETHERTYPE 0x6003
#define FRAMELEN     1518

static const char version[] = "$VER: dncheck 0.2 (23.10.2026)";

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

static struct MsgPort *port;
static struct IOSana2Req *req;
static BOOL dev_open;
static UBYTE mymac[6];
static UBYTE *buf;

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

static void print_error(void)
{
	Printf("error %ld/%ld", (LONG)req->ios2_Req.io_Error, (LONG)req->ios2_WireError);
}

static BOOL open_device(const char *name, LONG unit, ULONG flags)
{
	req->ios2_BufferManagement = buffertags;
	if (OpenDevice((STRPTR)name, unit, (struct IORequest *)req, flags) != 0)
		return FALSE;
	dev_open = TRUE;
	return TRUE;
}

static void close_device(void)
{
	if (dev_open)
		CloseDevice((struct IORequest *)req);
	dev_open = FALSE;
}

static BOOL station_address(UBYTE *current, UBYTE *factory)
{
	req->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
	if (DoIO((struct IORequest *)req) != 0)
		return FALSE;
	memcpy(current, req->ios2_SrcAddr, 6);
	memcpy(factory, req->ios2_DstAddr, 6);
	return TRUE;
}

/* Waits up to secs seconds for one DECnet frame. Returns TRUE and fills
   src with its sender address if one arrived. */
static BOOL wait_frame(LONG secs, BOOL raw, UBYTE *src)
{
	LONG ticks;
	BOOL got = FALSE;

	req->ios2_Req.io_Command = CMD_READ;
	req->ios2_Req.io_Flags = raw ? SANA2IOF_RAW : 0;
	req->ios2_PacketType = DN_ETHERTYPE;
	req->ios2_DataLength = FRAMELEN;
	req->ios2_Data = buf;
	BeginIO((struct IORequest *)req);   /* not SendIO: it clears io_Flags */

	for (ticks = 0; ticks < secs * 50; ticks += 10) {
		if (CheckIO((struct IORequest *)req))
			break;
		if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
			Printf("    ***Break\n");
			break;
		}
		Delay(10);
	}
	if (!CheckIO((struct IORequest *)req))
		AbortIO((struct IORequest *)req);
	WaitIO((struct IORequest *)req);
	if (req->ios2_Req.io_Error == 0) {
		got = TRUE;
		if (raw && req->ios2_DataLength >= 14)
			memcpy(src, buf + 6, 6);
		else
			memcpy(src, req->ios2_SrcAddr, 6);
	}
	return got;
}

int main(void)
{
	static const char template[] = "DEVICE/A,ADDR/A,UNIT/K/N,WAIT/K/N";
	LONG args[4] = { 0, 0, 0, 0 };
	struct RDArgs *rda;
	const char *devname;
	LONG unit = 0, wait = 20;
	UWORD addr;
	struct Sana2DeviceQuery query;
	UBYTE current[6], factory[6], src[6];
	BOOL taken = FALSE, foreign = FALSE, multicast = FALSE;
	BOOL fallback = FALSE, framed = FALSE, dnetd;
	int rc = 20;

	(void)version;
	rda = ReadArgs((STRPTR)template, args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (STRPTR)"dncheck");
		return 20;
	}
	devname = (const char *)args[0];
	if (!parse_addr((const char *)args[1], &addr)) {
		Printf("dncheck: bad ADDR, expected area.node (area 1-63, node 1-1023)\n");
		FreeArgs(rda);
		return 20;
	}
	if (args[2])
		unit = *(LONG *)args[2];
	if (args[3])
		wait = *(LONG *)args[3];
	mymac[0] = 0xaa; mymac[1] = 0x00; mymac[2] = 0x04; mymac[3] = 0x00;
	mymac[4] = addr & 0xff;
	mymac[5] = addr >> 8;

	port = CreateMsgPort();
	req = port ? (struct IOSana2Req *)CreateIORequest(port, sizeof(struct IOSana2Req)) : NULL;
	buf = AllocVec(FRAMELEN, MEMF_ANY);
	if (!req || !buf) {
		Printf("dncheck: out of memory\n");
		goto out;
	}

	/* 1. open and query */
	Printf("1. Opening %s unit %ld: ", (STRPTR)devname, unit);
	if (!open_device(devname, unit, 0)) {
		Printf("FAILED (error %ld)\n", (LONG)req->ios2_Req.io_Error);
		Printf("   Check the device name in DEVS:Networks/ and the unit number.\n");
		goto out;
	}
	Printf("ok\n");
	rc = 10;

	memset(&query, 0, sizeof query);
	query.SizeAvailable = sizeof query;
	req->ios2_Req.io_Command = S2_DEVICEQUERY;
	req->ios2_StatData = &query;
	if (DoIO((struct IORequest *)req) == 0) {
		Printf("   Wire type %ld, MTU %ld, %ld bit/s, address %ld bits\n",
		       query.HardwareType, query.MTU, query.BPS, (LONG)query.AddrFieldSize);
		if (query.HardwareType != S2WireType_Ethernet) {
			Printf("   Not an Ethernet device; DECnet needs Ethernet.\n");
			goto out;
		}
	} else {
		Printf("   S2_DEVICEQUERY not supported (");
		print_error();
		Printf("), continuing\n");
	}
	if (station_address(current, factory)) {
		Printf("   Factory address ");
		print_mac(factory);
		Printf(", current ");
		print_mac(current);
		Printf("\n");
	}

	/* 2. set the DECnet address */
	Printf("2. Setting DECnet address ");
	print_mac(mymac);
	Printf(": ");
	req->ios2_Req.io_Command = S2_CONFIGINTERFACE;
	memcpy(req->ios2_SrcAddr, mymac, 6);
	if (DoIO((struct IORequest *)req) != 0 &&
	    (req->ios2_WireError == S2WERR_IS_CONFIGURED ||
	     req->ios2_Req.io_Error == S2ERR_BAD_STATE))
		foreign = TRUE;
	if (station_address(current, factory) && memcmp(current, mymac, 6) == 0) {
		taken = TRUE;
		foreign = FALSE;
		Printf("ok\n");
	} else if (foreign) {
		Printf("unit was already configured\n");
		Printf("   Other network software (AmiTCP?) has started first and set ");
		print_mac(current);
		Printf(".\n   Reboot, run dncheck before the network starts.\n");
	} else {
		Printf("the driver keeps its own address\n");
	}

	/* 3. multicast */
	Printf("3. Registering DECnet multicast ");
	print_mac(all_endnodes);
	Printf(": ");
	req->ios2_Req.io_Command = S2_ADDMULTICASTADDRESS;
	memcpy(req->ios2_SrcAddr, all_endnodes, 6);
	if (DoIO((struct IORequest *)req) == 0) {
		multicast = TRUE;
		Printf("ok\n");
	} else {
		Printf("FAILED (");
		print_error();
		Printf(")\n");
	}

	/* 4. fallback: promiscuous */
	if (!taken || !multicast) {
		Printf("4. Reopening promiscuous (dnetd's fallback): ");
		close_device();
		if (!open_device(devname, unit, SANA2OPF_PROM)) {
			Printf("FAILED (error %ld)\n", (LONG)req->ios2_Req.io_Error);
			Printf("   This device cannot carry DECnet.\n");
			goto out;
		}
		fallback = TRUE;
		Printf("ok\n");
	} else {
		Printf("4. Promiscuous fallback not needed\n");
	}

	/* 5. listen. A running dnetd takes the DECnet frames itself (some drivers,
	   e.g. Amiberry's uaenet.device, give each frame to one reader only). */
	Forbid();
	dnetd = FindPort((STRPTR)"DECnet") != NULL;
	Permit();
	if (dnetd && wait > 0) {
		Printf("5. Traffic test skipped: dnetd is running and receives them\n");
		wait = 0;
	} else if (wait > 0) {
		Printf("5. Waiting up to %ld s for DECnet traffic (Ctrl-C stops)... ", wait);
		Flush(Output());
		framed = wait_frame(wait, fallback, src);
		if (framed) {
			UWORD a = decnet_addr(src);

			Printf("received from ");
			print_mac(src);
			if (a)
				Printf(" (node %ld.%ld)", (LONG)(a >> 10), (LONG)(a & 0x3ff));
			Printf("\n");
		} else {
			Printf("nothing\n");
			Printf("   No DECnet router on this LAN, or the frames are not delivered.\n");
		}
	} else {
		Printf("5. Traffic test skipped (WAIT=0)\n");
	}

	/* summary */
	Printf("\nResult: ");
	if (taken && multicast && (framed || wait == 0)) {
		Printf("the driver fully supports DECnet.\n");
		rc = 0;
	} else if (foreign) {
		Printf("not decided yet, the unit was configured by other software.\n"
		       "        Reboot and run dncheck before the network starts.\n");
		rc = 5;
	} else if (fallback) {
		Printf("usable in fallback mode (promiscuous, raw frames).\n"
		       "        Whether the driver sends our source address unchanged\n"
		       "        only shows once dnetd runs; it costs some CPU time.\n");
		rc = 5;
	} else {
		Printf("the driver takes the address, but no DECnet traffic was seen.\n"
		       "        Check the cable and that a DECnet router is on the LAN.\n");
		rc = 5;
	}
	if (taken)
		Printf("        The card keeps this address until the next reboot.\n");

out:
	close_device();
	if (buf)
		FreeVec(buf);
	if (req)
		DeleteIORequest((struct IORequest *)req);
	if (port)
		DeleteMsgPort(port);
	FreeArgs(rda);
	return rc;
}

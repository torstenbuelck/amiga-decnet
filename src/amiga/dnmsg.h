/*
 * dnmsg.h - private protocol between decnet.library and dnetd.
 *
 * Every library call is one DnMsg sent to dnetd's public port; the caller
 * waits for the reply. dnetd copies data in and out while it holds the
 * message, so the caller's buffers are safe.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DNMSG_H
#define DNMSG_H

#include <exec/ports.h>
#include <libraries/decnet.h>

#define DN_PORTNAME "DECnet"

enum {
	DNC_CONNECT = 1,    /* dm_Req -> dm_Link or dm_Result -1, dm_Reason */
	DNC_READ,           /* dm_Buf/dm_Len/dm_Timeout -> dm_Result */
	DNC_WRITE,          /* dm_Buf/dm_Len -> dm_Result 0 / -1 */
	DNC_CLOSE,          /* dm_Link */
	DNC_REASON,         /* dm_Link -> dm_Reason */
	/* incoming links (decnet.library 1.2) */
	DNC_LISTEN,         /* dm_Len objnum, dm_Buf name -> dm_Id listener */
	DNC_ACCEPT,         /* dm_Link listener, dm_Buf DnConnInfo, dm_Timeout
	                       -> dm_Id link (still to be answered) */
	DNC_ANSWER,         /* dm_Link, dm_Reason (0 accept), dm_Buf/dm_Len data */
	DNC_UNLISTEN,       /* dm_Link listener */
	/* frames of another protocol on dnetd's device (dnetd 0.7, for
	   latd): one program per ethertype */
	DNC_TAPOPEN,        /* dm_Len ethertype, dm_Buf struct DnTapInfo
	                       -> dm_Id tap */
	DNC_TAPREAD,        /* dm_Link tap, dm_Buf/dm_Len/dm_Timeout -> dm_Result
	                       frame length: dst(6) src(6) type(2) payload */
	DNC_TAPWRITE,       /* dm_Link tap, dm_Buf dst(6) payload, dm_Len */
	DNC_TAPCLOSE        /* dm_Link tap */
};

/* DNC_TAPOPEN */
struct DnTapInfo {
	UBYTE dti_Multicast[6];     /* in: multicast address to receive too
	                               (all zero: none) */
	UBYTE dti_Address[6];       /* out: the station address */
};

struct DnMsg {
	struct Message dm_Msg;
	LONG   dm_Cmd;
	ULONG  dm_Link;         /* dnetd's link id */
	APTR   dm_Buf;
	LONG   dm_Len;
	LONG   dm_Timeout;      /* ms */
	struct DnConnectReq *dm_Req;
	LONG   dm_Result;
	LONG   dm_Reason;
	ULONG  dm_Id;           /* new link / listener id (DNC_LISTEN, DNC_ACCEPT) */
};

/* What the library hands out as struct DnLink. */
struct DnLink {
	struct MsgPort *dl_Port;    /* reply port of the owning task */
	ULONG  dl_Id;
	LONG   dl_Reason;
	struct DnMsg dl_Msg;
};

/* What the library hands out as struct DnListener. */
struct DnListener {
	struct MsgPort *dn_Port;
	ULONG  dn_Id;
	struct DnMsg dn_Msg;
};

#endif

/*
 * dnmsg.h - private protocol between decnet.library and dnetd.
 *
 * Every library call is one DnMsg sent to dnetd's public port; the caller
 * waits for the reply. dnetd copies data in and out while it holds the
 * message, so the caller's buffers are safe.
 *
 * Copyright (C) 2026  Torsten
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
	DNC_REASON          /* dm_Link -> dm_Reason */
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
};

/* What the library hands out as struct DnLink. */
struct DnLink {
	struct MsgPort *dl_Port;    /* reply port of the owning task */
	ULONG  dl_Id;
	LONG   dl_Reason;
	struct DnMsg dl_Msg;
};

#endif

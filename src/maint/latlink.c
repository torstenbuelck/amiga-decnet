/*
 * latlink.c - dnmaint's side of latd's service offer (see latmsg.h).
 *
 * latd announces our service while the offer stands and hands incoming
 * sessions over (LTC_ACCEPT); dnmaint checks the terminal server's node
 * name and answers (LTC_ANSWER). The session itself then runs through
 * session.c like a CTERM one. Every call waits for latd's reply, which
 * comes at once except for LTC_ACCEPT (at most its timeout).
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <dos/dos.h>

#include <proto/exec.h>

#include <string.h>

#include "../amiga/dnmsg.h"
#include "../lat/latmsg.h"
#include "dnmaint.h"

/* send to latd and wait; FALSE: latd does not run */
static BOOL call(struct DnMsg *m)
{
	struct MsgPort *rp, *p;

	rp = CreateMsgPort();
	if (!rp)
		return FALSE;
	m->dm_Msg.mn_Node.ln_Type = NT_MESSAGE;
	m->dm_Msg.mn_ReplyPort = rp;
	m->dm_Msg.mn_Length = sizeof *m;
	Forbid();
	p = FindPort((STRPTR)LAT_PORTNAME);
	if (p)
		PutMsg(p, &m->dm_Msg);
	Permit();
	if (p)
		while (!GetMsg(rp))
			WaitPort(rp);
	DeleteMsgPort(rp);
	return p != NULL;
}

/* offer the service named after the node; 0: latd does not run (or
   already has an offer) */
ULONG lt_offer(void)
{
	struct DnMsg m;

	memset(&m, 0, sizeof m);
	m.dm_Cmd = LTC_OFFER;
	if (!call(&m) || m.dm_Result < 0)
		return 0;
	return m.dm_Id;
}

/* wait up to timeout ms for an incoming session: >0 its id (to be
   answered), 0 none came, <0 the offer is gone (latd stopped) */
LONG lt_accept(ULONG offer, char *node, char *port, LONG timeout)
{
	struct LatIncoming li;
	struct DnMsg m;

	memset(&m, 0, sizeof m);
	m.dm_Cmd = LTC_ACCEPT;
	m.dm_Link = offer;
	m.dm_Buf = &li;
	m.dm_Timeout = timeout;
	if (!call(&m))
		return -1;
	if (m.dm_Result == DNR_TIMEOUT)
		return 0;
	if (m.dm_Result != 1)
		return -1;
	li.li_Node[16] = li.li_Port[16] = 0;
	strcpy(node, li.li_Node);
	strcpy(port, li.li_Port);
	return (LONG)m.dm_Id;
}

int lt_answer(ULONG id, LONG reason)
{
	struct DnMsg m;

	memset(&m, 0, sizeof m);
	m.dm_Cmd = LTC_ANSWER;
	m.dm_Link = id;
	m.dm_Reason = reason;
	if (!call(&m) || m.dm_Result < 0)
		return -1;
	return 0;
}

void lt_close(ULONG id)
{
	struct DnMsg m;

	memset(&m, 0, sizeof m);
	m.dm_Cmd = DNC_CLOSE;
	m.dm_Link = id;
	call(&m);
}

void lt_unoffer(ULONG offer)
{
	struct DnMsg m;

	memset(&m, 0, sizeof m);
	m.dm_Cmd = LTC_UNOFFER;
	m.dm_Link = offer;
	call(&m);
}

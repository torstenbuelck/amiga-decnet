/*
 * lattest - test latd without a terminal program (not distributed).
 *
 * Usage: lattest SERVICE [LINE ...]
 *   Connects through latd, prints the output, sends each LINE (with CR)
 *   after 4 s, then waits 5 s and disconnects. A LINE "^C" sends Ctrl-C,
 *   "BREAK" a BREAK, "DA" the answer of a VT100 to a terminal query.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#include "../amiga/dnmsg.h"
#include "latmsg.h"

static struct MsgPort *rp;

static LONG call(struct DnMsg *m)
{
	struct MsgPort *p;

	m->dm_Msg.mn_Node.ln_Type = NT_MESSAGE;
	m->dm_Msg.mn_ReplyPort = rp;
	m->dm_Msg.mn_Length = sizeof *m;
	Forbid();
	p = FindPort((STRPTR)LAT_PORTNAME);
	if (p)
		PutMsg(p, &m->dm_Msg);
	Permit();
	if (!p)
		return -2;
	while (!GetMsg(rp))
		WaitPort(rp);
	return m->dm_Result;
}

/* print what comes within ms */
static int drain(ULONG link, LONG ms)
{
	static char buf[1024];
	struct DnMsg m;
	LONG n;

	for (;;) {
		memset(&m, 0, sizeof m);
		m.dm_Cmd = DNC_READ;
		m.dm_Link = link;
		m.dm_Buf = buf;
		m.dm_Len = sizeof buf;
		m.dm_Timeout = ms;
		n = call(&m);
		if (n == DNR_CLOSED || n < 0) {
			Printf((STRPTR)"\n[session closed, reason %ld]\n", m.dm_Reason);
			return 1;
		}
		if (n == DNR_TIMEOUT)
			return 0;
		Write(Output(), buf, n);
		if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
			return 1;
	}
}

int main(int argc, char **argv)
{
	struct LatConnect lc;
	struct DnMsg m;
	ULONG link;
	int i, closed = 0;

	if (argc < 2) {
		PutStr((STRPTR)"usage: lattest SERVICE [LINE ...]\n");
		return RETURN_FAIL;
	}
	rp = CreateMsgPort();
	if (!rp)
		return RETURN_FAIL;
	memset(&lc, 0, sizeof lc);
	strncpy(lc.lc_Service, argv[1], 16);
	strcpy(lc.lc_Port, "LATTEST");
	memset(&m, 0, sizeof m);
	m.dm_Cmd = DNC_CONNECT;
	m.dm_Buf = &lc;
	m.dm_Timeout = 70000;
	if (call(&m) < 0) {
		Printf((STRPTR)"connect failed: %ld\n", m.dm_Reason);
		DeleteMsgPort(rp);
		return RETURN_ERROR;
	}
	link = m.dm_Link;
	PutStr((STRPTR)"[connected]\n");
	for (i = 2; i <= argc && !closed; i++) {
		closed = drain(link, i < argc ? 4000 : 5000);
		if (closed || i == argc)
			break;
		memset(&m, 0, sizeof m);
		m.dm_Link = link;
		if (!strcmp(argv[i], "BREAK")) {
			m.dm_Cmd = LTC_BREAK;
		} else {
			static char line[200];
			if (!strcmp(argv[i], "^C"))
				strcpy(line, "\003");
			else if (!strcmp(argv[i], "DA"))
				strcpy(line, "\033[?1;2c");
			else {
				strncpy(line, argv[i], sizeof line - 2);
				line[sizeof line - 2] = 0;
				strcat(line, "\r");
			}
			m.dm_Cmd = DNC_WRITE;
			m.dm_Buf = line;
			m.dm_Len = strlen(line);
		}
		call(&m);
	}
	memset(&m, 0, sizeof m);
	m.dm_Cmd = DNC_CLOSE;
	m.dm_Link = link;
	call(&m);
	PutStr((STRPTR)"\n[done]\n");
	DeleteMsgPort(rp);
	return RETURN_OK;
}

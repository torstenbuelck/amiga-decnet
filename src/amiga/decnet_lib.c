/*
 * decnet_lib.c - decnet.library: application interface to dnetd.
 *
 * A thin library: every call becomes a DnMsg to dnetd's public port
 * "DECnet" and waits for the reply. Built without the C library
 * (-nostartfiles -nostdlib); AmigaOS 2.04, 68000.
 *
 * Copyright (C) 2026  Torsten
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <exec/resident.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "dnmsg.h"

#define LIBNAME   "decnet.library"
#define VERSION   1
#define REVISION  0
#define IDSTRING  "decnet.library 1.0 (21.10.2026)\r\n"

#define REG(r, decl) register decl __asm(#r)

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
static BPTR seglist;

/* Must be the first code in the file: running the library as a program
   just returns. */
LONG LibNoRun(void)
{
	return -1;
}

static const char libname[] = LIBNAME;
static const char idstring[] = IDSTRING;

/* ---- tiny libc replacements (gcc may emit calls to these) ------------------ */

void *memset(void *d, int c, unsigned long n)
{
	UBYTE *p = d;
	while (n--)
		*p++ = (UBYTE)c;
	return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
	UBYTE *p = d;
	const UBYTE *q = s;
	while (n--)
		*p++ = *q++;
	return d;
}

/* ---- transport ------------------------------------------------------------------ */

/* Send m to dnetd and wait for the reply on port. FALSE if dnetd is gone. */
static BOOL transact(struct DnMsg *m, struct MsgPort *port)
{
	struct MsgPort *dn;

	m->dm_Msg.mn_Node.ln_Type = NT_MESSAGE;
	m->dm_Msg.mn_ReplyPort = port;
	m->dm_Msg.mn_Length = sizeof(struct DnMsg);
	Forbid();
	dn = FindPort((STRPTR)DN_PORTNAME);
	if (dn)
		PutMsg(dn, &m->dm_Msg);
	Permit();
	if (!dn)
		return FALSE;
	while (!GetMsg(port))
		WaitPort(port);
	return TRUE;
}

/* ---- library functions -------------------------------------------------------- */

struct DnLink *LibDnConnect(REG(a0, struct DnConnectReq *req), REG(a6, struct Library *base))
{
	struct DnLink *l;

	(void)base;
	req->dcr_Reason = DNREASON_RESOURCES;
	l = AllocVec(sizeof(struct DnLink), MEMF_PUBLIC | MEMF_CLEAR);
	if (!l)
		return NULL;
	l->dl_Port = CreateMsgPort();
	if (!l->dl_Port) {
		FreeVec(l);
		return NULL;
	}
	l->dl_Msg.dm_Cmd = DNC_CONNECT;
	l->dl_Msg.dm_Req = req;
	if (!transact(&l->dl_Msg, l->dl_Port)) {
		req->dcr_Reason = DNREASON_NODNETD;
	} else if (l->dl_Msg.dm_Result == 0) {
		l->dl_Id = l->dl_Msg.dm_Link;
		req->dcr_Reason = 0;
		return l;
	} else {
		req->dcr_Reason = l->dl_Msg.dm_Reason;
	}
	DeleteMsgPort(l->dl_Port);
	FreeVec(l);
	return NULL;
}

LONG LibDnRead(REG(a0, struct DnLink *l), REG(a1, APTR buf), REG(d0, LONG len),
               REG(d1, LONG timeout), REG(a6, struct Library *base))
{
	(void)base;
	if (!l)
		return DNR_CLOSED;
	l->dl_Msg.dm_Cmd = DNC_READ;
	l->dl_Msg.dm_Link = l->dl_Id;
	l->dl_Msg.dm_Buf = buf;
	l->dl_Msg.dm_Len = len;
	l->dl_Msg.dm_Timeout = timeout;
	if (!transact(&l->dl_Msg, l->dl_Port)) {
		l->dl_Reason = DNREASON_NODNETD;
		return DNR_CLOSED;
	}
	if (l->dl_Msg.dm_Result < 0)
		l->dl_Reason = l->dl_Msg.dm_Reason;
	return l->dl_Msg.dm_Result;
}

LONG LibDnWrite(REG(a0, struct DnLink *l), REG(a1, CONST_APTR buf), REG(d0, LONG len),
                REG(a6, struct Library *base))
{
	(void)base;
	if (!l)
		return -1;
	l->dl_Msg.dm_Cmd = DNC_WRITE;
	l->dl_Msg.dm_Link = l->dl_Id;
	l->dl_Msg.dm_Buf = (APTR)buf;
	l->dl_Msg.dm_Len = len;
	if (!transact(&l->dl_Msg, l->dl_Port)) {
		l->dl_Reason = DNREASON_NODNETD;
		return -1;
	}
	if (l->dl_Msg.dm_Result < 0)
		l->dl_Reason = l->dl_Msg.dm_Reason;
	return l->dl_Msg.dm_Result;
}

VOID LibDnClose(REG(a0, struct DnLink *l), REG(a6, struct Library *base))
{
	(void)base;
	if (!l)
		return;
	l->dl_Msg.dm_Cmd = DNC_CLOSE;
	l->dl_Msg.dm_Link = l->dl_Id;
	transact(&l->dl_Msg, l->dl_Port);
	DeleteMsgPort(l->dl_Port);
	FreeVec(l);
}

LONG LibDnLinkReason(REG(a0, struct DnLink *l), REG(a6, struct Library *base))
{
	(void)base;
	return l ? l->dl_Reason : DNREASON_NOLINK;
}

/* "23.208" or a name from ENV:DECnet/nodes ("HOWIE 23.208" per line).
   Returns the address or -1. */
static LONG parse_addr(const UBYTE *s, const UBYTE **end)
{
	LONG area = -1, v = 0;
	int digits = 0;

	for (; *s; s++) {
		if (*s == '.' && area < 0 && digits) {
			area = v;
			v = 0;
			digits = 0;
		} else if (*s >= '0' && *s <= '9') {
			v = v * 10 + (*s - '0');
			digits++;
		} else {
			break;
		}
	}
	if (end)
		*end = s;
	if (area < 1 || area > 63 || !digits || v < 1 || v > 1023)
		return -1;
	return (area << 10) | v;
}

static UBYTE upper(UBYTE c)
{
	return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

LONG LibDnNodeAddr(REG(a0, CONST_STRPTR name), REG(a6, struct Library *base))
{
	const UBYTE *end;
	UBYTE *buf, *p;
	LONG a, n, result = -1;
	BPTR fh;

	(void)base;
	a = parse_addr((const UBYTE *)name, &end);
	if (a >= 0 && *end == 0)
		return a;
	if (!DOSBase || !(fh = Open((STRPTR)"ENV:DECnet/nodes", MODE_OLDFILE)))
		return -1;
	buf = AllocVec(4097, MEMF_ANY);
	if (!buf) {
		Close(fh);
		return -1;
	}
	n = Read(fh, buf, 4096);
	Close(fh);
	buf[n > 0 ? n : 0] = 0;
	for (p = buf; *p && result < 0; ) {
		const UBYTE *q = (const UBYTE *)name;
		while (*p == ' ' || *p == '\t')
			p++;
		while (*q && upper(*p) == upper(*q)) {
			p++;
			q++;
		}
		if (!*q && (*p == ' ' || *p == '\t')) {
			while (*p == ' ' || *p == '\t')
				p++;
			result = parse_addr(p, NULL);
		}
		while (*p && *p != '\n')
			p++;
		if (*p)
			p++;
	}
	FreeVec(buf);
	return result;
}

/* ---- library housekeeping ----------------------------------------------------- */

static BPTR LibExpunge(REG(a6, struct Library *base))
{
	BPTR seg;

	if (base->lib_OpenCnt) {
		base->lib_Flags |= LIBF_DELEXP;
		return 0;
	}
	seg = seglist;
	Remove(&base->lib_Node);
	if (DOSBase)
		CloseLibrary((struct Library *)DOSBase);
	FreeMem((UBYTE *)base - base->lib_NegSize, base->lib_NegSize + base->lib_PosSize);
	return seg;
}

static struct Library *LibOpen(REG(a6, struct Library *base))
{
	base->lib_OpenCnt++;
	base->lib_Flags &= ~LIBF_DELEXP;
	return base;
}

static BPTR LibClose(REG(a6, struct Library *base))
{
	if (--base->lib_OpenCnt == 0 && (base->lib_Flags & LIBF_DELEXP))
		return LibExpunge(base);
	return 0;
}

static LONG LibReserved(void)
{
	return 0;
}

static struct Library *LibInit(REG(d0, struct Library *base), REG(a0, BPTR seg),
                               REG(a6, struct ExecBase *sysbase))
{
	SysBase = sysbase;
	seglist = seg;
	base->lib_Node.ln_Type = NT_LIBRARY;
	base->lib_Node.ln_Name = (char *)libname;
	base->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
	base->lib_Version = VERSION;
	base->lib_Revision = REVISION;
	base->lib_IdString = (APTR)idstring;
	DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);
	return base;
}

static const APTR functable[] = {
	(APTR)LibOpen,
	(APTR)LibClose,
	(APTR)LibExpunge,
	(APTR)LibReserved,
	(APTR)LibDnConnect,
	(APTR)LibDnRead,
	(APTR)LibDnWrite,
	(APTR)LibDnClose,
	(APTR)LibDnLinkReason,
	(APTR)LibDnNodeAddr,
	(APTR)-1
};

static const ULONG inittable[] = {
	sizeof(struct Library),
	(ULONG)functable,
	0,
	(ULONG)LibInit
};

const struct Resident romtag = {
	RTC_MATCHWORD,
	(struct Resident *)&romtag,
	(APTR)(&romtag + 1),
	RTF_AUTOINIT,
	VERSION,
	NT_LIBRARY,
	0,
	(char *)libname,
	(char *)idstring,
	(APTR)inittable
};

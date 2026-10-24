/*
 * dnaccept - test tool for incoming links (decnet.library 1.2).
 *
 * Usage: dnaccept OBJECT/A,COUNT/N,REJECT/S
 *
 *   OBJECT   object number (1-255) or name to serve
 *   COUNT    sessions to serve before it ends (default 1)
 *   REJECT   refuse every connect (access reason) instead of accepting
 *
 * Each accepted session gets a greeting message, then every message is
 * sent back until the other end disconnects. Ctrl-C ends the tool.
 * A test tool only; it is not part of the distribution.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/decnet.h>

#include <string.h>

static const char version[] = "$VER: dnaccept 0.1 (24.10.2026)";

struct Library *DECnetBase;
static BOOL stop;

static BOOL broken(void)
{
	if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
		stop = TRUE;
	return stop;
}

static void serve(struct DnLink *l)
{
	static const char hello[] = "Hello from the Amiga (dnaccept)";
	UBYTE buf[1500];
	LONG n;

	if (DnWrite(l, hello, sizeof hello - 1)) {
		Printf("write failed, reason %ld\n", DnLinkReason(l));
		return;
	}
	for (;;) {
		n = DnRead(l, buf, sizeof buf, 1000);
		if (n == DNR_TIMEOUT) {
			if (broken())
				return;
			continue;
		}
		if (n < 0) {
			Printf("link gone, reason %ld\n", DnLinkReason(l));
			return;
		}
		Printf("<- %ld bytes\n", n);
		if (DnWrite(l, buf, n)) {
			Printf("write failed, reason %ld\n", DnLinkReason(l));
			return;
		}
	}
}

int main(void)
{
	LONG args[3] = { 0, 0, 0 };
	struct RDArgs *rd;
	struct DnListener *ls;
	struct DnConnInfo ci;
	struct DnLink *l;
	LONG num = 0, count = 1, done = 0, rc = RETURN_FAIL;
	const char *obj;

	(void)version;
	rd = ReadArgs((STRPTR)"OBJECT/A,COUNT/N,REJECT/S", args, NULL);
	if (!rd) {
		PrintFault(IoErr(), (STRPTR)"dnaccept");
		return RETURN_FAIL;
	}
	obj = (const char *)args[0];
	if (StrToLong((STRPTR)obj, &num) < 0 || num < 0 || num > 255)
		num = 0;
	if (args[1])
		count = *(LONG *)args[1];
	DECnetBase = OpenLibrary((STRPTR)"decnet.library", 1);
	if (!DECnetBase || DECnetBase->lib_Version < 1 ||
	    (DECnetBase->lib_Version == 1 && DECnetBase->lib_Revision < 2)) {
		PutStr((STRPTR)"dnaccept: needs decnet.library 1.2\n");
		goto out;
	}
	ls = DnListen(num, num ? NULL : (CONST_STRPTR)obj);
	if (!ls) {
		Printf("dnaccept: cannot serve object %s (taken, or dnetd not running)\n", (LONG)obj);
		goto out;
	}
	Printf("serving object %s, Ctrl-C ends\n", (LONG)obj);
	while (done < count && !broken()) {
		l = DnAccept(ls, &ci, 1000);
		if (!l)
			continue;
		Printf("connect from %ld.%ld to %ld \"%s\", source \"%s\", user \"%s\", %ld bytes data\n",
		       (LONG)(ci.dci_Node >> 10), (LONG)(ci.dci_Node & 1023), (LONG)ci.dci_ObjNum, (LONG)ci.dci_ObjName,
		       (LONG)ci.dci_SrcName, (LONG)ci.dci_User, (LONG)ci.dci_DataLen);
		if (args[2]) {
			DnAnswer(l, DNREASON_ACCESS, NULL, 0);
			PutStr((STRPTR)"rejected\n");
		} else if (DnAnswer(l, 0, NULL, 0) == 0) {
			serve(l);
		} else
			Printf("answer failed, reason %ld\n", DnLinkReason(l));
		DnClose(l);
		done++;
	}
	DnUnlisten(ls);
	rc = RETURN_OK;
out:
	if (DECnetBase)
		CloseLibrary(DECnetBase);
	FreeArgs(rd);
	return rc;
}

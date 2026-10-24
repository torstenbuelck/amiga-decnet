/*
 * dnmaint - maintenance login service (CTERM host on object 42).
 *
 * Usage: dnmaint VERBOSE/S
 *
 *   VERBOSE  log the CTERM messages (ctermh) to the output
 *
 * Started from DECnet-Start when the installer enabled it. Waits for
 * connects on object 42, rejects every node but the configured one
 * (DNREASON_ACCESS), then runs one session at a time (see session.c);
 * further connects wait at dnetd meanwhile. Ctrl-C ends it, also
 * during a session.
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

#include "../amiga/dnmsg.h"
#include "dnmaint.h"

static const char version[] = "$VER: dnmaint 0.1 (24.10.2026)";

struct Library *DECnetBase;

static struct mcfg cfg;
static struct msession sess;    /* large: keep it off the stack */

/* one ENV: variable (GetVar stops at the end of the first line);
   trailing blanks removed. 0 = not there or empty */
static long env(const char *name, char *buf, long size)
{
	long n;

	n = GetVar((STRPTR)name, (STRPTR)buf, size, GVF_GLOBAL_ONLY);
	if (n < 0) {
		buf[0] = 0;
		return 0;
	}
	while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t' || buf[n - 1] == '\r'))
		buf[--n] = 0;
	return n;
}

/* read ENV:DECnet/maint-node and maint-password; 0 = both set.
   Read again for every connect, so a change counts without a restart */
int cfg_load(struct mcfg *cfg)
{
	char node[64];
	LONG a;

	memset(cfg, 0, sizeof *cfg);
	env("DECnet/maint-password", cfg->password, sizeof cfg->password);
	if (env("DECnet/maint-node", node, sizeof node)) {
		/* area.node, or a name from ENV:DECnet/nodes */
		a = DnNodeAddr((CONST_STRPTR)node);
		if (a > 0 && (a >> 10) >= 1 && (a & 1023) >= 1)
			cfg->node = (UWORD)a;
	}
	return cfg->node && cfg->password[0] ? 0 : -1;
}

/* the connect comes from the maintenance node */
BOOL node_allowed(const struct mcfg *cfg, UWORD node)
{
	return cfg->node != 0 && node == cfg->node;
}

static BOOL dnetd_running(void)
{
	BOOL r;

	Forbid();
	r = FindPort((STRPTR)DN_PORTNAME) != NULL;
	Permit();
	return r;
}

static BOOL broken(void)
{
	return (SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0;
}

int main(void)
{
	LONG args[1] = { 0 };
	struct RDArgs *rd;
	struct DnListener *ls;
	struct DnConnInfo ci;
	struct DnLink *l;
	LONG rc = RETURN_FAIL;
	BOOL ok;

	(void)version;
	rd = ReadArgs((STRPTR)"VERBOSE/S", args, NULL);
	if (!rd) {
		PrintFault(IoErr(), (STRPTR)"dnmaint");
		return RETURN_FAIL;
	}
	DECnetBase = OpenLibrary((STRPTR)"decnet.library", 1);
	if (!DECnetBase || DECnetBase->lib_Version < 1 ||
	    (DECnetBase->lib_Version == 1 && DECnetBase->lib_Revision < 2)) {
		PutStr((STRPTR)"dnmaint: needs decnet.library 1.2\n");
		goto out;
	}
	if (cfg_load(&cfg))
		PutStr((STRPTR)"dnmaint: ENV:DECnet/maint-node or maint-password not set, every connect is refused\n");
	ls = DnListen(MAINT_OBJECT, NULL);
	if (!ls) {
		PutStr((STRPTR)"dnmaint: cannot serve object 42 (taken, or dnetd not running)\n");
		goto out;
	}
	while (!broken()) {
		l = DnAccept(ls, &ci, 1000);
		if (!l) {
			/* NULL after a timeout, or at once when dnetd is gone:
			   then end, instead of spinning (a new dnetd would not
			   know our listener anyway) */
			if (!dnetd_running()) {
				PutStr((STRPTR)"dnmaint: dnetd has stopped, ending\n");
				break;
			}
			continue;
		}
		ok = cfg_load(&cfg) == 0 && node_allowed(&cfg, ci.dci_Node);
		Printf("dnmaint: connect from %ld.%ld %s\n",
		       (LONG)(ci.dci_Node >> 10), (LONG)(ci.dci_Node & 1023),
		       (LONG)(ok ? "accepted" : "refused"));
		if (!ok) {
			DnAnswer(l, DNREASON_ACCESS, NULL, 0);
			DnClose(l);
			continue;
		}
		if (DnAnswer(l, 0, NULL, 0)) {
			DnClose(l);
			continue;
		}
		if (session_init(&sess, &cfg, l) == 0) {
			sess.cth.verbose = args[0] != 0;
			session_run(&sess);
		}
		session_end(&sess);             /* closes the link */
		PutStr((STRPTR)"dnmaint: session ended\n");
	}
	DnUnlisten(ls);
	rc = RETURN_OK;
out:
	memset(&cfg, 0, sizeof cfg);    /* no password left in memory */
	if (DECnetBase)
		CloseLibrary(DECnetBase);
	FreeArgs(rd);
	return rc;
}

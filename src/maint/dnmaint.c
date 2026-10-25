/*
 * dnmaint - maintenance login service (CTERM host on object 42).
 *
 * Usage: dnmaint [VERBOSE] [LOG=file]
 *        dnmaint STOP
 *
 *   VERBOSE  log the CTERM messages (ctermh) as well
 *   LOG      append the messages to file, opened for each line (as
 *            dnetd does), instead of the output
 *   STOP     end the running dnmaint (as Ctrl-C) and wait until it is
 *            gone; return code 5 if none runs
 *
 * One dnmaint at a time: its public port "DECnet-Maint" tells STOP and a
 * second start that one runs (the second ends at once, before it touches
 * the log).
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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../amiga/dnmsg.h"
#include "dnmaint.h"

static const char version[] = "$VER: dnmaint 0.3 (25.10.2026)";

#define MAINT_PORTNAME "DECnet-Maint"

struct Library *DECnetBase;

static const char *logfile;
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

/* a message to the log file or the output */
void mlog(const char *fmt, ...)
{
	char buf[200];
	va_list ap;
	BPTR fh;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (logfile && (fh = Open((STRPTR)logfile, MODE_READWRITE)) != 0) {
		Seek(fh, 0, OFFSET_END);
		Write(fh, buf, strlen(buf));
		Close(fh);
	} else
		PutStr((STRPTR)buf);
}

/* STOP: Ctrl-C to the running dnmaint, then wait up to 30 s (a session
   may take a while to end: con_stop waits up to 20 s for the Shell) */
static int stop_dnmaint(void)
{
	struct MsgPort *p;
	BOOL found;
	int i;

	Forbid();
	p = FindPort((STRPTR)MAINT_PORTNAME);
	if (p && p->mp_SigTask)
		Signal((struct Task *)p->mp_SigTask, SIGBREAKF_CTRL_C);
	Permit();
	if (!p) {
		PutStr((STRPTR)"dnmaint is not running\n");
		return RETURN_WARN;
	}
	for (i = 0; i < 600; i++) {
		Forbid();
		found = FindPort((STRPTR)MAINT_PORTNAME) != NULL;
		Permit();
		if (!found) {
			PutStr((STRPTR)"dnmaint stopped\n");
			return RETURN_OK;
		}
		Delay(5);
	}
	PutStr((STRPTR)"dnmaint does not stop\n");
	return RETURN_ERROR;
}

static BOOL broken(void)
{
	return (SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0;
}

int main(void)
{
	LONG args[3] = { 0, 0, 0 };
	struct RDArgs *rd;
	struct MsgPort *port = NULL;
	struct DnListener *ls;
	struct DnConnInfo ci;
	struct DnLink *l;
	LONG rc = RETURN_FAIL;
	BOOL ok;
	BPTR fh;

	(void)version;
	rd = ReadArgs((STRPTR)"VERBOSE/S,LOG/K,STOP/S", args, NULL);
	if (!rd) {
		PrintFault(IoErr(), (STRPTR)"dnmaint");
		return RETURN_FAIL;
	}
	if (args[2]) {
		rc = stop_dnmaint();
		FreeArgs(rd);
		return rc;
	}
	/* one at a time: the port first, before the log is touched */
	port = CreateMsgPort();
	if (!port) {
		FreeArgs(rd);
		return RETURN_FAIL;
	}
	Forbid();
	if (FindPort((STRPTR)MAINT_PORTNAME)) {
		Permit();
		PutStr((STRPTR)"dnmaint: already running\n");
		DeleteMsgPort(port);
		FreeArgs(rd);
		return RETURN_WARN;
	}
	port->mp_Node.ln_Name = (char *)MAINT_PORTNAME;
	port->mp_Node.ln_Pri = -128;
	AddPort(port);
	Permit();
	logfile = (const char *)args[1];
	if (logfile && (fh = Open((STRPTR)logfile, MODE_NEWFILE)) != 0)
		Close(fh);                      /* start empty */

	DECnetBase = OpenLibrary((STRPTR)"decnet.library", 1);
	if (!DECnetBase || DECnetBase->lib_Version < 1 ||
	    (DECnetBase->lib_Version == 1 && DECnetBase->lib_Revision < 2)) {
		mlog("dnmaint: needs decnet.library 1.2\n");
		goto out;
	}
	if (cfg_load(&cfg))
		mlog("dnmaint: ENV:DECnet/maint-node or maint-password not set, every connect is refused\n");
	ls = DnListen(MAINT_OBJECT, NULL);
	if (!ls) {
		mlog("dnmaint: cannot serve object 42 (taken, or dnetd not running)\n");
		goto out;
	}
	mlog("dnmaint: serving object 42, Ctrl-C or dnmaint STOP ends\n");
	while (!broken()) {
		l = DnAccept(ls, &ci, 1000);
		if (!l) {
			/* NULL after a timeout, or at once when dnetd is gone:
			   then end, instead of spinning (a new dnetd would not
			   know our listener anyway) */
			if (!dnetd_running()) {
				mlog("dnmaint: dnetd has stopped, ending\n");
				break;
			}
			continue;
		}
		ok = cfg_load(&cfg) == 0 && node_allowed(&cfg, ci.dci_Node);
		mlog("dnmaint: connect from %ld.%ld %s\n",
		     (long)(ci.dci_Node >> 10), (long)(ci.dci_Node & 1023),
		     ok ? "accepted" : "refused");
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
		mlog("dnmaint: session ended\n");
	}
	DnUnlisten(ls);
	rc = RETURN_OK;
out:
	memset(&cfg, 0, sizeof cfg);    /* no password left in memory */
	if (DECnetBase)
		CloseLibrary(DECnetBase);
	RemPort(port);
	DeleteMsgPort(port);
	FreeArgs(rd);
	return rc;
}

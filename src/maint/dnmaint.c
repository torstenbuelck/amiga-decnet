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
 * connects on object 42, rejects every node but the configured ones
 * (DNREASON_ACCESS), then runs one session at a time (see session.c);
 * further connects wait at dnetd meanwhile. With LAT nodes configured
 * and latd running it also offers the service named after the node:
 * terminal servers not in the list are rejected (access denied). Ctrl-C
 * ends it, also during a session.
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

static const char version[] = "$VER: dnmaint 0.5 (26.10.2026)";

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

/* the separators in the node lists */
static BOOL sep(char c)
{
	return c == 0 || c == ' ' || c == ',' || c == '\t';
}

/* read ENV:DECnet/maint-node, maint-latnodes and maint-password;
   0 = password and at least one CTERM node set.
   Read again for every connect, so a change counts without a restart */
int cfg_load(struct mcfg *cfg)
{
	char list[120], name[24];
	const char *p;
	LONG a;
	int n;

	memset(cfg, 0, sizeof *cfg);
	env("DECnet/maint-password", cfg->password, sizeof cfg->password);
	env("DECnet/maint-latnodes", cfg->latnodes, sizeof cfg->latnodes);
	env("DECnet/maint-node", list, sizeof list);
	/* area.node or names from ENV:DECnet/nodes, blank or comma
	   separated */
	for (p = list; *p; ) {
		while (*p && sep(*p))
			p++;
		for (n = 0; *p && !sep(*p); p++)
			if (n < (int)sizeof name - 1)
				name[n++] = *p;
		name[n] = 0;
		if (!n)
			break;
		a = DnNodeAddr((CONST_STRPTR)name);
		if (a > 0 && (a >> 10) >= 1 && (a & 1023) >= 1) {
			if (cfg->nnodes < MAINT_NODES)
				cfg->nodes[cfg->nnodes++] = (UWORD)a;
		} else if (!cfg->badnode[0])
			strcpy(cfg->badnode, name);
	}
	return cfg->nnodes && cfg->password[0] ? 0 : -1;
}

/* the connect comes from a maintenance node */
BOOL node_allowed(const struct mcfg *cfg, UWORD node)
{
	int i;

	for (i = 0; i < cfg->nnodes; i++)
		if (node == cfg->nodes[i])
			return TRUE;
	return FALSE;
}

static int upcase(int c)
{
	return c >= 'a' && c <= 'z' ? c - 32 : c;
}

/* the LAT node (terminal server) is in maint-latnodes: entries are
   NAME (every port of that terminal server) or NAME/PORT, separated
   by blanks or commas, case does not matter */
/* the entry at p starts with name; returns the rest of the
   entry after it, or NULL */
static const char *match(const char *p, const char *name)
{
	while (*name && upcase(*p) == upcase(*name))
		p++, name++;
	return *name ? NULL : p;
}

BOOL latnode_allowed(const struct mcfg *cfg, const char *node, const char *port)
{
	const char *p = cfg->latnodes, *q;

	if (!*node)
		return FALSE;
	while (*p) {
		while (*p && sep(*p))
			p++;
		if (!*p)
			break;
		q = match(p, node);
		if (q && sep(*q))
			return TRUE;            /* NAME: any port */
		if (q && *q == '/' && (q = match(q + 1, port)) != NULL && sep(*q) && *port)
			return TRUE;            /* NAME/PORT */
		while (!sep(*p))
			p++;
	}
	return FALSE;
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
	LONG rc = RETURN_FAIL, lid;
	ULONG offer = 0;
	char lnode[17], lport[17];
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
	cfg_load(&cfg);
	if (!cfg.password[0])
		mlog("dnmaint: ENV:DECnet/maint-password not set, every connect is refused\n");
	else if (!cfg.nnodes && !cfg.latnodes[0])
		mlog("dnmaint: ENV:DECnet/maint-node and maint-latnodes not set, every connect is refused\n");
	else if (!cfg.nnodes)
		mlog("dnmaint: ENV:DECnet/maint-node not set, LAT only\n");
	if (cfg.badnode[0])
		mlog("dnmaint: maint-node: %s is no known node, ignored\n", (STRPTR)cfg.badnode);
	ls = DnListen(MAINT_OBJECT, NULL);
	if (!ls) {
		mlog("dnmaint: cannot serve object 42 (taken, or dnetd not running)\n");
		goto out;
	}
	mlog("dnmaint: serving object 42, Ctrl-C or dnmaint STOP ends\n");
	while (!broken()) {
		/* LAT: offered while LAT nodes are configured and latd runs
		   (it may start later, or again) */
		cfg_load(&cfg);
		if (!offer && cfg.latnodes[0] && cfg.password[0]) {
			offer = lt_offer();
			if (offer)
				mlog("dnmaint: offered over LAT\n");
		} else if (offer && !cfg.latnodes[0]) {
			lt_unoffer(offer);
			offer = 0;
			mlog("dnmaint: no LAT nodes any more, LAT offer withdrawn\n");
		}
		if (offer) {
			lid = lt_accept(offer, lnode, lport, 500);
			if (lid < 0) {
				offer = 0;              /* latd has stopped */
			} else if (lid > 0) {
				ok = cfg.password[0] && latnode_allowed(&cfg, lnode, lport);
				mlog("dnmaint: LAT connect from %s/%s %s\n", (STRPTR)lnode,
				     (STRPTR)lport, ok ? "accepted" : "refused");
				if (!ok) {
					lt_answer((ULONG)lid, 14);   /* access denied; gone */
					continue;
				}
				if (lt_answer((ULONG)lid, 0)) {
					lt_close((ULONG)lid);   /* ended meanwhile */
					continue;
				}
				if (session_init_lat(&sess, &cfg, (ULONG)lid) == 0) {
					sess.verbose = args[0] != 0;
					session_run(&sess);
				}
				session_end(&sess);     /* closes the session */
				mlog("dnmaint: session ended\n");
				continue;
			}
		}
		l = DnAccept(ls, &ci, offer ? 500 : 1000);
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
			sess.verbose = sess.cth.verbose = args[0] != 0;
			session_run(&sess);
		}
		session_end(&sess);             /* closes the link */
		mlog("dnmaint: session ended\n");
	}
	if (offer)
		lt_unoffer(offer);
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

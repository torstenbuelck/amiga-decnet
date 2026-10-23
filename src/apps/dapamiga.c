/*
 * dapamiga.c - DAP client over decnet.library (shared by dndir, dncopy).
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <exec/memory.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/decnet.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dapamiga.h"
#include "../common/nodecfg.h"

struct Library *DECnetBase;
char dapa_error[200];
static struct DnLink *link;

/* print a failure and keep its text in dapa_error */
static void say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(dapa_error, sizeof dapa_error, fmt, ap);
	va_end(ap);
	Printf("%s\n", (STRPTR)dapa_error);
}

static long link_read(void *ctx, unsigned char *buf, long max, long timeout)
{
	(void)ctx;
	return DnRead(link, buf, max, timeout);
}

static int link_write(void *ctx, const unsigned char *buf, long n)
{
	(void)ctx;
	return DnWrite(link, buf, n) == 0 ? 0 : -1;
}

static void dap_log(void *ctx, const char *msg)
{
	(void)ctx;
	Printf("dap: %s\n", (STRPTR)msg);
}

struct dap *dapa_open(const char *node, const char *user, const char *password,
                      int blocking, int verbose)
{
	struct DnConnectReq req;
	struct nodecfg cfg;
	struct dap *d;
	LONG addr;

	DECnetBase = OpenLibrary((STRPTR)DECNETNAME, DECNET_VERSION);
	if (!DECnetBase) {
		say("cannot open %s", DECNETNAME);
		return NULL;
	}
	/* access data and NOBLOCK from ENV:DECnet/nodes, as DN: uses them;
	   a USER on the command line brings its own PASSWORD */
	memset(&cfg, 0, sizeof cfg);
	nc_lookup(node, &cfg, NULL, NULL);
	if (!user) {
		if (cfg.user[0])
			user = cfg.user;
		if (!password && cfg.pass[0])
			password = cfg.pass;
	}
	if (cfg.noblock)
		blocking = 0;
	addr = DnNodeAddr((CONST_STRPTR)node);
	if (addr < 0) {
		say("unknown node %s", node);
		goto fail;
	}
	memset(&req, 0, sizeof req);
	req.dcr_Node = (UWORD)addr;
	req.dcr_ObjNum = DAP_OBJECT;
	req.dcr_SrcName = (CONST_STRPTR)"DNCOPY";
	req.dcr_User = (CONST_STRPTR)user;
	req.dcr_Password = (CONST_STRPTR)password;
	link = DnConnect(&req);
	if (!link) {
		if (req.dcr_Reason == DNREASON_NODNETD)
			say("dnetd is not running");
		else
			say("connect to %s failed: %s (%ld)", node,
			    dap_reason_text(req.dcr_Reason), (long)req.dcr_Reason);
		goto fail;
	}
	d = AllocVec(sizeof(struct dap), MEMF_ANY | MEMF_CLEAR);
	if (!d) {
		say("out of memory");
		DnClose(link);
		goto fail;
	}
	d->read = link_read;
	d->write = link_write;
	d->log = dap_log;
	d->verbose = verbose;
	if (dap_config(d, blocking) == 0)
		return d;
	say("%s", d->error);
	FreeVec(d);
	DnClose(link);
fail:
	CloseLibrary(DECnetBase);
	DECnetBase = NULL;
	return NULL;
}

void dapa_close(struct dap *d)
{
	FreeVec(d);
	DnClose(link);
	link = NULL;
	CloseLibrary(DECnetBase);
	DECnetBase = NULL;
}

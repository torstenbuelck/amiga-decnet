/*
 * dapamiga.c - DAP client over decnet.library (shared by dndir, dncopy).
 *
 * Copyright (C) 2026  Torsten
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <exec/memory.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/decnet.h>

#include <string.h>

#include "dapamiga.h"

struct Library *DECnetBase;
static struct DnLink *link;

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
	struct dap *d;
	LONG addr;

	DECnetBase = OpenLibrary((STRPTR)DECNETNAME, DECNET_VERSION);
	if (!DECnetBase) {
		Printf("cannot open %s\n", (STRPTR)DECNETNAME);
		return NULL;
	}
	addr = DnNodeAddr((CONST_STRPTR)node);
	if (addr < 0) {
		Printf("unknown node %s\n", (STRPTR)node);
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
			Printf("dnetd is not running\n");
		else
			Printf("connect to %s failed: %s (%ld)\n", (STRPTR)node,
			       (STRPTR)dap_reason_text(req.dcr_Reason), req.dcr_Reason);
		goto fail;
	}
	d = AllocVec(sizeof(struct dap), MEMF_ANY | MEMF_CLEAR);
	if (!d) {
		Printf("out of memory\n");
		DnClose(link);
		goto fail;
	}
	d->read = link_read;
	d->write = link_write;
	d->log = dap_log;
	d->verbose = verbose;
	if (dap_config(d, blocking) == 0)
		return d;
	Printf("%s\n", (STRPTR)d->error);
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

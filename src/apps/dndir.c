/*
 * dndir - list files on a DECnet node (DAP / FAL).
 *
 * Usage: dndir NODE/A,SPEC,USER/K,PASSWORD/K,NOBLOCK/S,VERBOSE/S
 *   SPEC default *.*; NOBLOCK for servers like FOTOFX
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "dapamiga.h"

static const char version[] = "$VER: dndir 0.1 (21.10.2026)";
unsigned long __stack = 16384;

static int show(void *ud, const struct dap_dirent *e)
{
	LONG *count = ud;

	if (*count == 0)
		Printf("Directory of %s%s\n\n", (STRPTR)e->volume, (STRPTR)e->directory);
	Printf("%-30s %8ld  %-18s  %s\n", (STRPTR)e->name, e->blocks, (STRPTR)e->created,
	       (STRPTR)e->owner);
	(*count)++;
	return (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) ? 1 : 0;
}

int main(void)
{
	static const char template[] = "NODE/A,SPEC,USER/K,PASSWORD/K,NOBLOCK/S,VERBOSE/S";
	LONG args[6] = { 0, 0, 0, 0, 0, 0 };
	struct RDArgs *rda;
	struct dap *d;
	LONG count = 0;
	int rc = RETURN_FAIL;

	rda = ReadArgs((STRPTR)template, args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (STRPTR)"dndir");
		return RETURN_FAIL;
	}
	d = dapa_open((const char *)args[0], (const char *)args[2], (const char *)args[3],
	              !args[4], args[5] != 0);
	if (d) {
		if (dap_directory(d, args[1] ? (const char *)args[1] : "*.*", show, &count) == 0) {
			Printf("\nTotal of %ld file%s.\n", count, (STRPTR)(count == 1 ? "" : "s"));
			rc = RETURN_OK;
		} else {
			Printf("%s\n", (STRPTR)d->error);
		}
		dapa_close(d);
	}
	FreeArgs(rda);
	(void)version;
	return rc;
}

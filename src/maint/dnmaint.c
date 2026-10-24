/*
 * dnmaint - maintenance login service (CTERM host on object 42).
 *
 * Usage: dnmaint
 *
 * Started from DECnet-Start when the installer enabled it. Waits for
 * connects on object 42, rejects every node but the configured one,
 * then runs one session at a time (see session.c). Ctrl-C ends it.
 *
 * Framework only: the bodies are still empty.
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

#include "dnmaint.h"

static const char version[] = "$VER: dnmaint 0.0 (24.10.2026)";

struct Library *DECnetBase;

/* read ENV:DECnet/maint-node and maint-password; 0 = both set */
int cfg_load(struct mcfg *cfg)
{
	/* TODO */
	return -1;
}

/* the connect comes from the maintenance node */
BOOL node_allowed(const struct mcfg *cfg, UWORD node)
{
	/* TODO */
	return FALSE;
}

int main(void)
{
	(void)version;
	/* TODO: open decnet.library 1.2, cfg_load, DnListen(MAINT_OBJECT),
	   DnAccept loop: node_allowed -> DnAnswer, session_init/run/end;
	   otherwise DnAnswer(DNREASON_ACCESS) */
	return RETURN_FAIL;
}

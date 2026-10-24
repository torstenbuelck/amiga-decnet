/*
 * session.c - one maintenance session: CTERM host over the DECnet link.
 *
 * Pumps messages between the link (DnRead) and ctermh, and between
 * ctermh and the console handler the Shell uses (console.c).
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

/* set up the session and ctermh callbacks, send BIND */
int session_init(struct msession *s, struct mcfg *cfg, struct DnLink *l)
{
	/* TODO */
	return -1;
}

/* main loop until the session is over; 0 = ended normally */
int session_run(struct msession *s)
{
	/* TODO */
	return -1;
}

/* stop the Shell and the console, unbind, close the link */
void session_end(struct msession *s)
{
	/* TODO */
}

/* ctermh: one message to the terminal */
int session_net_write(void *ctx, const unsigned char *buf, long n)
{
	/* TODO */
	return -1;
}

/* ctermh: a read is done; to login.c or the console by state */
void session_input(void *ctx, const char *buf, long n, int term)
{
	/* TODO */
}

/* ctermh: ^C / ^Y */
void session_interrupt(void *ctx, int ch)
{
	/* TODO */
}

/* ctermh: debug log */
void session_log(void *ctx, const char *msg)
{
	/* TODO */
}

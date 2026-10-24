/*
 * console.c - DOS console handler between the Shell and the session.
 *
 * The Shell gets our port as its console: ACTION_READ is served from
 * CTERM reads (line reads in cooked mode, char reads in raw mode),
 * ACTION_WRITE goes out through ctermh_write. Translations: LF -> CR LF
 * out, CR -> LF in, CSI 0x9B <-> ESC [.
 *
 * Framework only: the bodies are still empty.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#include "dnmaint.h"

/* create the handler port */
int con_start(struct msession *s)
{
	/* TODO */
	return -1;
}

/* one packet from the Shell (FINDINPUT/OUTPUT, READ, WRITE,
   SCREEN_MODE, WAIT_CHAR, END, ...) */
void con_packet(struct msession *s, struct DosPacket *dp)
{
	/* TODO */
}

/* input from the terminal for the Shell */
int con_input(struct msession *s, const char *buf, long n)
{
	/* TODO */
	return -1;
}

/* ^C / ^Y: break signal to the Shell */
void con_interrupt(struct msession *s, int ch)
{
	/* TODO */
}

/* answer open packets, remove the port */
void con_stop(struct msession *s)
{
	/* TODO */
}

/* start the Shell with our handler as console */
int shell_start(struct msession *s)
{
	/* TODO */
	return -1;
}

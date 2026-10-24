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
#include <dos/dostags.h>

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

/* a file handle on our port, built by hand: Open() would send
   ACTION_FINDINPUT to our own port and wait for the answer that only
   this process can give */
static BPTR con_fh(struct msession *s)
{
	struct FileHandle *fh;

	fh = AllocDosObject(DOS_FILEHANDLE, NULL);
	if (!fh)
		return 0;
	fh->fh_Type = s->conport;               /* packets go to us */
	fh->fh_Port = (struct MsgPort *)DOSTRUE; /* interactive */
	fh->fh_Args = (LONG)s;                  /* dp_Arg1 of READ/WRITE/END */
	return MKBADDR(fh);
}

/* start the Shell with our handler as console */
int shell_start(struct msession *s)
{
	BPTR in, out;

	in = con_fh(s);
	out = con_fh(s);
	if (!in || !out)
		goto fail;
	/* asynchronous: the Shell closes both handles when it ends, which
	   arrives here as ACTION_END */
	if (SystemTags((STRPTR)"",
	               SYS_Input, in,
	               SYS_Output, out,
	               SYS_Asynch, TRUE,
	               SYS_UserShell, TRUE,
	               NP_ConsoleTask, (ULONG)s->conport,
	               NP_WindowPtr, -1,
	               NP_Name, (ULONG)"dnmaint Shell",
	               TAG_DONE) == -1)
		goto fail;
	return 0;
fail:
	/* not Close(): that would send ACTION_END to ourselves */
	if (in)
		FreeDosObject(DOS_FILEHANDLE, BADDR(in));
	if (out)
		FreeDosObject(DOS_FILEHANDLE, BADDR(out));
	return -1;
}

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
	switch (dp->dp_Type) {
	case ACTION_END:
		/* one handle on us closed; the Shell is gone when the last
		   one is (its own two, and any "*" opened from it) */
		ReplyPkt(dp, DOSTRUE, 0);
		if (s->handles > 0 && --s->handles == 0)
			s->shell_gone = TRUE;
		break;
	default:
		/* TODO: FINDINPUT/FINDOUTPUT/FINDUPDATE (s->handles++),
		   READ, WRITE, SCREEN_MODE, WAIT_CHAR, ... */
		ReplyPkt(dp, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
		break;
	}
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
	s->handles++;
	return MKBADDR(fh);
}

/* S:Shell-Startup (prompt, aliases) as the first input line: works on
   every OS version, while System() runs a startup stream before the
   interactive input only from V47 on (SYS_CmdStream). The Shell does
   not echo, so the line is not seen; con_input must append to inbuf */
static void shell_startup(struct msession *s)
{
	static const char cmd[] = "Execute S:Shell-Startup\n";
	BPTR lock;

	lock = Lock((STRPTR)"S:Shell-Startup", ACCESS_READ);
	if (!lock)
		return;
	UnLock(lock);
	memcpy(s->inbuf, cmd, sizeof cmd - 1);
	s->inlen = sizeof cmd - 1;
	s->inpos = 0;
}

/* start the Shell with our handler as console */
int shell_start(struct msession *s)
{
	BPTR in, out;

	s->handles = 0;
	s->shell_gone = FALSE;
	shell_startup(s);
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
	s->handles = 0;
	s->inlen = s->inpos = 0;
	return -1;
}

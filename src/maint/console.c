/*
 * console.c - DOS console handler between the Shell and the session.
 *
 * The Shell gets our port as its console: ACTION_READ is served from
 * CTERM reads (line reads in cooked mode, char reads in raw mode),
 * ACTION_WRITE goes out through ctermh_write. Translations: LF -> CR LF
 * out, CR -> LF in (cooked mode), CSI 0x9B <-> ESC [.
 *
 * The session waits on conport's signal and hands every message to
 * con_packet() (the DosPacket is in mn_Node.ln_Name); the line typed
 * comes in through con_input().
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

#define CSI 0x9b
#define ESC 0x1b
#ifndef ID_CON
#define ID_CON (('C' << 24) | ('O' << 16) | ('N' << 8))
#endif

/* create the handler port */
int con_start(struct msession *s)
{
	s->conport = CreateMsgPort();
	if (!s->conport)
		return -1;
	s->readpkt = NULL;
	s->sigtask = NULL;
	s->raw = FALSE;
	s->eof = FALSE;
	s->inlen = s->inpos = 0;
	s->handles = 0;
	s->shell_gone = FALSE;
	return 0;
}

/* the task a packet came from (its reply port is the process port) */
static struct Task *sender(struct DosPacket *dp)
{
	return dp->dp_Port ? (struct Task *)dp->dp_Port->mp_SigTask : NULL;
}

/* answer the waiting read from inbuf, or ask the terminal for more */
static void con_serve(struct msession *s)
{
	struct DosPacket *dp = s->readpkt;
	long n;

	if (!dp)
		return;
	if (s->inpos < s->inlen) {
		n = s->inlen - s->inpos;
		if (n > dp->dp_Arg3)
			n = dp->dp_Arg3;
		memcpy((char *)dp->dp_Arg2, s->inbuf + s->inpos, n);
		s->inpos += n;
		if (s->inpos == s->inlen)
			s->inpos = s->inlen = 0;
		s->readpkt = NULL;
		ReplyPkt(dp, n, 0);
		return;
	}
	if (s->eof || s->state == MS_DONE) {
		s->eof = FALSE;
		s->readpkt = NULL;
		ReplyPkt(dp, 0, 0);
		return;
	}
	if (!s->cth.read_pending)
		ctermh_read(&s->cth, s->raw ? CTH_CHAR : CTH_LINE, NULL, 0, NULL, 0);
}

/* output from the Shell: CSI becomes ESC [, LF -> CR LF in ctermh */
static void con_write(struct msession *s, const UBYTE *buf, long n)
{
	long i, from = 0;

	if (s->state == MS_DONE)
		return;
	for (i = 0; i < n; i++) {
		if (buf[i] != CSI)
			continue;
		if (i > from)
			ctermh_write(&s->cth, (const char *)buf + from, i - from);
		ctermh_write(&s->cth, "\033[", 2);
		from = i + 1;
	}
	if (n > from)
		ctermh_write(&s->cth, (const char *)buf + from, n - from);
}

/* one packet from the Shell (FINDINPUT/OUTPUT, READ, WRITE,
   SCREEN_MODE, WAIT_CHAR, END, ...) */
void con_packet(struct msession *s, struct DosPacket *dp)
{
	struct FileHandle *fh;
	struct InfoData *id;
	struct MsgPort *old;

	switch (dp->dp_Type) {
	case ACTION_FINDINPUT:
	case ACTION_FINDOUTPUT:
	case ACTION_FINDUPDATE:
		/* Open("*") or "CONSOLE:" from the Shell or a command */
		fh = BADDR(dp->dp_Arg1);
		fh->fh_Args = (LONG)s;
		fh->fh_Port = (struct MsgPort *)DOSTRUE;   /* interactive */
		s->handles++;
		if (!s->sigtask)
			s->sigtask = sender(dp);
		ReplyPkt(dp, DOSTRUE, 0);
		break;
	case ACTION_END:
		/* one handle on us closed; the Shell is gone when the last
		   one is (its own two, and any "*" opened from it) */
		ReplyPkt(dp, DOSTRUE, 0);
		if (s->handles > 0 && --s->handles == 0)
			s->shell_gone = TRUE;
		break;
	case ACTION_READ:
		if (s->readpkt) {
			/* one reader at a time, as on a real console */
			ReplyPkt(dp, -1, ERROR_OBJECT_IN_USE);
			break;
		}
		s->sigtask = sender(dp);
		s->readpkt = dp;
		con_serve(s);
		break;
	case ACTION_WRITE:
		con_write(s, (const UBYTE *)dp->dp_Arg2, dp->dp_Arg3);
		ReplyPkt(dp, dp->dp_Arg3, 0);
		break;
	case ACTION_SCREEN_MODE:
		/* DOSTRUE: raw (every key at once), 0: cooked (lines) */
		if ((dp->dp_Arg1 != 0) != s->raw) {
			s->raw = dp->dp_Arg1 != 0;
			/* a read of the old kind gives back what was typed so
			   far (con_input, term 0); the next one is of the new */
			if (s->cth.read_pending)
				ctermh_unread(&s->cth);
		}
		ReplyPkt(dp, DOSTRUE, 0);
		break;
	case ACTION_WAIT_CHAR:
		/* no timer: answers at once. Make sure keys keep coming. */
		if (s->inpos < s->inlen) {
			ReplyPkt(dp, DOSTRUE, 0);
			break;
		}
		if (s->state != MS_DONE && !s->cth.read_pending)
			ctermh_read(&s->cth, s->raw ? CTH_CHAR : CTH_LINE, NULL, 0, NULL, 0);
		ReplyPkt(dp, DOSFALSE, 0);
		break;
	case ACTION_CHANGE_SIGNAL:
		/* the Shell names the command that gets ^C */
		old = s->sigtask ? &((struct Process *)s->sigtask)->pr_MsgPort : NULL;
		if (dp->dp_Arg2)
			s->sigtask = (struct Task *)((struct MsgPort *)dp->dp_Arg2)->mp_SigTask;
		ReplyPkt(dp, DOSTRUE, (LONG)old);
		break;
	case ACTION_DISK_INFO:
		/* a console without a window (id_VolumeNode 0) */
		id = BADDR(dp->dp_Arg1);
		memset(id, 0, sizeof *id);
		id->id_DiskType = ID_CON;
		ReplyPkt(dp, DOSTRUE, 0);
		break;
	case ACTION_SEEK:
		ReplyPkt(dp, -1, ERROR_OBJECT_WRONG_TYPE);
		break;
	case ACTION_IS_FILESYSTEM:
		ReplyPkt(dp, DOSFALSE, 0);
		break;
	default:
		ReplyPkt(dp, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
		break;
	}
}

/* input from the terminal for the Shell: the keys or the line without
   its terminator, and the terminator (0: none, after char reads and
   ctermh_unread). Appends to inbuf; <0: no room, the rest is lost. */
int con_input(struct msession *s, const char *buf, long n, int term)
{
	long room, i;
	int rc = 0;
	char c;

	/* make room at the end */
	if (s->inpos > 0) {
		memmove(s->inbuf, s->inbuf + s->inpos, s->inlen - s->inpos);
		s->inlen -= s->inpos;
		s->inpos = 0;
	}
	room = sizeof s->inbuf - s->inlen;
	for (i = 0; i < n; i++) {
		c = buf[i];
		if (s->raw) {
			/* the Amiga console sends CSI where VT100 sends ESC [ */
			if (c == '[' && s->inlen > 0 && s->inbuf[s->inlen - 1] == ESC) {
				s->inbuf[s->inlen - 1] = (char)CSI;
				continue;
			}
		} else if (c == '\r')
			c = '\n';
		if (room == 0) {
			rc = -1;
			break;
		}
		s->inbuf[s->inlen++] = c;
		room--;
	}
	if (term == 0x1c)               /* Ctrl-\: end of file, as on CON: */
		s->eof = TRUE;
	else if (term) {
		if (room > 0)
			s->inbuf[s->inlen++] = s->raw ? '\r' : '\n';
		else
			rc = -1;
	}
	con_serve(s);
	return rc;
}

/* ^C / ^Y: break signal to the Shell or the command it runs.
   ^Y (VMS: stop everything) also sends Ctrl-D, which ends scripts. */
void con_interrupt(struct msession *s, int ch)
{
	ULONG sig = SIGBREAKF_CTRL_C;

	if (ch == 0x19)
		sig |= SIGBREAKF_CTRL_D;
	if (s->sigtask && s->handles > 0)   /* no Shell: the task may be gone */
		Signal(s->sigtask, sig);
}

/* the terminal is gone: reads end with EOF, the Shell and its command
   get a break. Waits until every handle on the port is closed (the
   port must not go away while a handle can still reach it), then
   removes it. */
void con_stop(struct msession *s)
{
	struct Message *m;

	if (!s->conport)
		return;
	s->state = MS_DONE;
	con_serve(s);                   /* waiting read: EOF */
	if (s->handles > 0 && s->sigtask)
		Signal(s->sigtask, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D);
	while (s->handles > 0) {
		WaitPort(s->conport);
		while ((m = GetMsg(s->conport)) != NULL)
			con_packet(s, (struct DosPacket *)m->mn_Node.ln_Name);
	}
	while ((m = GetMsg(s->conport)) != NULL)
		con_packet(s, (struct DosPacket *)m->mn_Node.ln_Name);
	DeleteMsgPort(s->conport);
	s->conport = NULL;
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

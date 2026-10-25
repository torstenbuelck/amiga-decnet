/*
 * console.c - DOS console handler between the Shell and the session.
 *
 * The Shell gets our port as its console: ACTION_READ is served from
 * CTERM character reads; in cooked mode the keys go through our own
 * line editor with history (edit.c), as the Amiga console has one and
 * the terminal side's editing differs from node to node. ACTION_WRITE
 * goes out through ctermh_write. Translations: LF -> CR LF out,
 * CSI 0x9B <-> ESC [, form feed -> ESC [H ESC [2J out.
 * ACTION_DISK_INFO hands out a ConUnit with the terminal size and the
 * cursor position, which we follow from our output (More 44.3 pages
 * by cu_YCP).
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

#include <devices/conunit.h>

#include "dnmaint.h"

static struct ConUnit fake_cu;
static struct IOStdReq fake_io;

#define STOP_TICKS (20 * 50)    /* con_stop waits this long for the Shell */
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
	s->cx = s->cy = 0;
	s->tstate = 0;
	ed_init(s);
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
		ctermh_read(&s->cth, CTH_CHAR, NULL, 0, NULL, 0);
}

int con_width(struct msession *s)
{
	return s->cth.width > 0 ? s->cth.width : 80;
}

static int con_height(struct msession *s)
{
	return s->cth.height > 0 ? s->cth.height : 24;
}

/* size and cursor into our ConUnit */
static void cu_sync(struct msession *s)
{
	int w = con_width(s);

	fake_cu.cu_XMax = w - 1;
	fake_cu.cu_YMax = con_height(s) - 1;
	fake_cu.cu_XCP = s->cx >= w ? w - 1 : s->cx;
	fake_cu.cu_YCP = s->cy;
}

/* a cursor control sequence ESC [ p0;p1 c on the cursor */
static void track_csi(struct msession *s, int c)
{
	int w = con_width(s), h = con_height(s);
	int n = s->tpar[0] > 0 ? s->tpar[0] : 1;

	if (s->cx >= w)
		s->cx = w - 1;
	switch (c) {
	case 'A':
		s->cy = s->cy > n ? s->cy - n : 0;
		break;
	case 'B':
		s->cy = s->cy + n < h ? s->cy + n : h - 1;
		break;
	case 'C':
		s->cx = s->cx + n < w ? s->cx + n : w - 1;
		break;
	case 'D':
		s->cx = s->cx > n ? s->cx - n : 0;
		break;
	case 'H':
	case 'f':
		s->cy = n <= h ? n - 1 : h - 1;
		n = s->tpar[1] > 0 ? s->tpar[1] : 1;
		s->cx = n <= w ? n - 1 : w - 1;
		break;
	}
}

/* follow the terminal's cursor through what we send it, by VT100 rules:
   a character in the last column leaves the cursor there until the
   next one wraps; at the bottom, line feeds scroll */
static void track(struct msession *s, const char *buf, long n)
{
	int w = con_width(s), h = con_height(s);
	unsigned char c;
	long i;

	for (i = 0; i < n; i++) {
		c = (unsigned char)buf[i];
		if (s->tstate == 1) {           /* after ESC */
			s->tstate = c == '[' ? 2 : 0;
			s->tpar[0] = s->tpar[1] = 0;
			s->tnpar = 0;
			continue;
		}
		if (s->tstate == 2) {           /* in ESC [ */
			if (c >= '0' && c <= '9') {
				if (s->tnpar < 2)
					s->tpar[s->tnpar] = s->tpar[s->tnpar] * 10 + c - '0';
			} else if (c == ';') {
				s->tnpar++;
			} else if (c < 0x20 || c > 0x3f) {
				s->tstate = 0;
				track_csi(s, c);
			}
			continue;
		}
		if (c == ESC) {
			s->tstate = 1;
		} else if (c == '\r') {
			s->cx = 0;
		} else if (c == '\n') {        /* goes out as CR LF */
			s->cx = 0;
			if (s->cy < h - 1)
				s->cy++;
		} else if (c == '\b') {
			if (s->cx >= w)
				s->cx = w - 1;
			if (s->cx > 0)
				s->cx--;
		} else if (c == '\t') {
			s->cx = (s->cx / 8 + 1) * 8;
			if (s->cx > w - 1)
				s->cx = w - 1;
		} else if (c >= 0x20 && c != 0x7f && (c < 0x80 || c >= 0xa0)) {
			if (s->cx >= w) {
				s->cx = 0;
				if (s->cy < h - 1)
					s->cy++;
			}
			s->cx++;
		}
	}
}

/* everything for the terminal goes out here */
void con_out(struct msession *s, const char *buf, long n)
{
	if (s->state == MS_DONE || n <= 0)
		return;
	track(s, buf, n);
	cu_sync(s);
	ctermh_write(&s->cth, buf, n);
}

/* append a decimal number */
static int put_num(char *p, int v)
{
	char tmp[8];
	int n = 0, i = 0;

	do
		tmp[n++] = (char)('0' + v % 10);
	while ((v /= 10) > 0 && n < (int)sizeof tmp);
	while (n > 0)
		p[i++] = tmp[--n];
	return i;
}

/* Window Status Request (CSI 0 q, used by More and others): answer
   like the Amiga console, CSI 1;1;rows;columns r, with the size the
   terminal reported; straight to the Shell, not through the editor */
static void con_report(struct msession *s)
{
	char r[32];
	int n = 0;

	r[n++] = (char)CSI;
	r[n++] = '1';
	r[n++] = ';';
	r[n++] = '1';
	r[n++] = ';';
	n += put_num(r + n, con_height(s));
	r[n++] = ';';
	n += put_num(r + n, con_width(s));
	r[n++] = ' ';
	r[n++] = 'r';
	if (s->inpos > 0) {
		memmove(s->inbuf, s->inbuf + s->inpos, s->inlen - s->inpos);
		s->inlen -= s->inpos;
		s->inpos = 0;
	}
	if (s->inlen + n <= (long)sizeof s->inbuf) {
		memcpy(s->inbuf + s->inlen, r, n);
		s->inlen += n;
	}
	con_serve(s);
}

/* output from the Shell: CSI becomes ESC [, form feed home + erase; a
   Window Status Request is answered here, not passed on. Collected,
   so that one Write() goes out in as few WRITE messages as fit */
static void con_write(struct msession *s, const UBYTE *buf, long n)
{
	char out[256];
	long i, o = 0, k;

	for (i = 0; i < n; i++) {
		if (o > (long)sizeof out - 8) {
			con_out(s, out, o);
			o = 0;
		}
		if (buf[i] == '\f') {
			/* clears the Amiga console; on a VT: home, erase */
			memcpy(out + o, "\033[H\033[2J", 7);
			o += 7;
			continue;
		}
		if (buf[i] == CSI)
			k = i + 1;
		else if (buf[i] == ESC && i + 1 < n && buf[i + 1] == '[')
			k = i + 2;
		else {
			out[o++] = (char)buf[i];
			continue;
		}
		if (k + 2 < n && buf[k] == '0' && buf[k + 1] == ' ' && buf[k + 2] == 'q') {
			con_report(s);
			i = k + 2;
			continue;
		}
		out[o++] = ESC;
		out[o++] = '[';
		i = k - 1;
	}
	con_out(s, out, o);
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
		if (s->cth.verbose)
			mlog("dnmaint: console opened (%ld open)\n", (long)s->handles);
		break;
	case ACTION_END:
		/* one handle on us closed; the Shell is gone when the last
		   one is (its own two, and any "*" opened from it) */
		ReplyPkt(dp, DOSTRUE, 0);
		if (s->handles > 0 && --s->handles == 0)
			s->shell_gone = TRUE;
		if (s->cth.verbose)
			mlog("dnmaint: console closed (%ld open)\n", (long)s->handles);
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
		/* DOSTRUE: raw (every key at once), 0: cooked (lines).
		   Both read single keys, so a pending read stays */
		s->raw = dp->dp_Arg1 != 0;
		ReplyPkt(dp, DOSTRUE, 0);
		break;
	case ACTION_WAIT_CHAR:
		/* no timer: answers at once. Make sure keys keep coming. */
		if (s->inpos < s->inlen) {
			ReplyPkt(dp, DOSTRUE, 0);
			break;
		}
		if (s->state != MS_DONE && !s->cth.read_pending)
			ctermh_read(&s->cth, CTH_CHAR, NULL, 0, NULL, 0);
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
		/* a console without a window (id_VolumeNode 0); programs
		   that size their output (More) find the terminal size in a
		   ConUnit of our own (no console.device behind it) */
		id = BADDR(dp->dp_Arg1);
		memset(id, 0, sizeof *id);
		id->id_DiskType = ID_CON;
		memset(&fake_cu, 0, sizeof fake_cu);
		memset(&fake_io, 0, sizeof fake_io);
		cu_sync(s);
		fake_cu.cu_XRSize = fake_cu.cu_YRSize = 8;
		fake_cu.cu_XRExtant = (fake_cu.cu_XMax + 1) * 8 - 1;
		fake_cu.cu_YRExtant = (fake_cu.cu_YMax + 1) * 8 - 1;
		fake_io.io_Unit = (struct Unit *)&fake_cu;
		id->id_InUse = (LONG)&fake_io;
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

/* keys from the terminal (character reads, term 0). Raw mode: to the
   Shell as they are, ESC [ as CSI like the Amiga console sends it;
   cooked mode: to the line editor, which hands over whole lines.
   Appends to inbuf; <0: no room, the rest is lost. */
int con_input(struct msession *s, const char *buf, long n, int term)
{
	long room, i;
	int rc = 0;
	char c;

	(void)term;
	if (s->inpos > 0) {             /* make room at the end */
		memmove(s->inbuf, s->inbuf + s->inpos, s->inlen - s->inpos);
		s->inlen -= s->inpos;
		s->inpos = 0;
	}
	if (!s->raw) {
		ed_keys(s, buf, n);
		con_serve(s);
		return 0;
	}
	room = sizeof s->inbuf - s->inlen;
	for (i = 0; i < n; i++) {
		c = buf[i];
		if (c == '[' && s->inlen > 0 && s->inbuf[s->inlen - 1] == ESC) {
			s->inbuf[s->inlen - 1] = (char)CSI;
			continue;
		}
		if (room == 0) {
			rc = -1;
			break;
		}
		s->inbuf[s->inlen++] = c;
		room--;
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
	int i;

	if (!s->conport)
		return;
	s->state = MS_DONE;
	con_serve(s);                   /* waiting read: EOF */
	if (s->handles > 0 && s->sigtask)
		Signal(s->sigtask, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D);
	for (i = 0; (s->handles > 0 || s->launcher) && i < STOP_TICKS / 5; i++) {
		while ((m = GetMsg(s->conport)) != NULL)
			con_packet(s, (struct DosPacket *)m->mn_Node.ln_Name);
		if (s->handles > 0 || s->launcher)
			Delay(5);
	}
	while ((m = GetMsg(s->conport)) != NULL)
		con_packet(s, (struct DosPacket *)m->mn_Node.ln_Name);
	if (s->handles > 0 || s->launcher) {
		/* a handle is still open (a command that neither reads nor
		   stops): deleting the port would crash whoever uses it later.
		   Leave it behind, ignoring messages: that one hangs instead */
		mlog("dnmaint: %ld console handle(s) still open, port left behind\n",
		     (long)s->handles);
		s->conport->mp_Flags = PA_IGNORE;
		s->conport = NULL;
		return;
	}
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

/* S:Shell-Startup (prompt, aliases) as the command Execute() runs
   before it reads the input, if the file exists. (Not as a first input
   line: the Shell would show its default prompt before reading it.) */
static const char *startup_cmd(void)
{
	BPTR lock;

	lock = Lock((STRPTR)"S:Shell-Startup", ACCESS_READ);
	if (!lock)
		return "";
	UnLock(lock);
	return "Execute S:Shell-Startup";
}

/*
 * The Shell is started the way Execute() documents for every OS
 * version: a command (S:Shell-Startup, or none) and an interactive
 * input handle give an interactive Shell like NewShell, which runs the
 * command first (System("") with SYS_Asynch does so only from V47 on;
 * before, it runs the empty command and ends). The Shell opens
 * its output itself ("*" on our port, as pr_ConsoleTask comes from the
 * input handle) and closes it when it ends.
 *
 * Execute() does not return before the Shell has ended, and the Shell
 * talks to the console all the time, so it must not run in this
 * process, which is the only one answering: a small process of its own
 * calls it and closes the input afterwards. One session at a time, so
 * the handover is static.
 */
static struct msession *launch_s;
static BPTR launch_in;
static struct Task *launch_parent;

static void launcher(void)
{
	struct msession *s = launch_s;
	BPTR dir, old = 0;
	LONG ok;

	/* the Shell starts in SYS:, not wherever dnmaint was started */
	dir = Lock((STRPTR)"SYS:", ACCESS_READ);
	if (dir)
		old = CurrentDir(dir);
	ok = Execute((STRPTR)startup_cmd(), launch_in, 0);
	if (dir)
		UnLock(CurrentDir(old));
	/* Execute() returns when the Shell has ended (or could not start).
	   The Shell has closed the output it opened; the input is ours.
	   This process may close it: the session answers ACTION_END */
	Close(launch_in);
	/* last: under Forbid() until this process is gone, so dnmaint
	   (whose code this is) cannot end before */
	Forbid();
	if (!ok)
		s->shell_failed = TRUE;
	s->launcher = FALSE;
	if (s->conport)                 /* wake the session up */
		Signal(launch_parent, 1UL << s->conport->mp_SigBit);
}

/* start the Shell with our handler as console */
int shell_start(struct msession *s)
{
	BPTR in;

	s->handles = 0;
	s->shell_gone = FALSE;
	s->shell_failed = FALSE;
	in = con_fh(s);
	if (!in)
		goto fail;
	launch_s = s;
	launch_in = in;
	launch_parent = FindTask(NULL);
	s->launcher = TRUE;
	/* NP_Cli: the launcher gets a copy of our CLI's command path
	   (Run from DECnet-Start: the one of the boot scripts), which
	   Execute() hands on to the Shell; without, it has only C: */
	if (!CreateNewProcTags(NP_Entry, (ULONG)launcher,
	                       NP_Name, (ULONG)"dnmaint launcher",
	                       NP_Cli, TRUE,
	                       NP_StackSize, 8192,
	                       NP_WindowPtr, -1,
	                       TAG_DONE))
		goto fail;
	return 0;
fail:
	/* not Close(): that would send ACTION_END to ourselves */
	if (in)
		FreeDosObject(DOS_FILEHANDLE, BADDR(in));
	s->launcher = FALSE;
	s->handles = 0;
	s->inlen = s->inpos = 0;
	return -1;
}

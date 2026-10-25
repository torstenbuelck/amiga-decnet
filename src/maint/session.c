/*
 * session.c - one maintenance session: CTERM host over the DECnet link.
 *
 * Pumps messages between the link (DnRead) and ctermh, and between
 * ctermh and the console handler the Shell uses (console.c). A LAT
 * session comes through latd instead and carries the terminal stream
 * as it is: the t_ functions below do what ctermh does for CTERM (no
 * echo for the password, LF -> CR LF, ^C/^Y from the stream).
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

#include "../amiga/dnmsg.h"
#include "../lat/latmsg.h"
#include "dnmaint.h"

#define NETBUF       2048       /* one NSP message from the terminal */
#define BIND_SECS    30         /* BIND to INITIATE */
#define LOGIN_SECS   60         /* for the password dialogue */

/*
 * The link is read asynchronously with dnetd's private protocol, as
 * dnterm.device does: DnRead() blocks, but the session must serve the
 * Shell's packets while it waits for keys. A read with a 1 s timeout
 * is always outstanding at dnetd; its timeouts are the session clock.
 * Writes go through DnWrite(). One session at a time, so this state is
 * static.
 */
static struct MsgPort *netport, *wport;
static struct DnMsg rmsg, wmsg;
static UBYTE netbuf[NETBUF];
static BOOL read_out;           /* rmsg is at dnetd */
static BOOL link_down;          /* the link is gone */

static void issue_read(struct msession *s)
{
	struct MsgPort *dn;

	if (read_out || link_down)
		return;
	rmsg.dm_Msg.mn_Node.ln_Type = NT_MESSAGE;
	rmsg.dm_Msg.mn_ReplyPort = netport;
	rmsg.dm_Msg.mn_Length = sizeof rmsg;
	rmsg.dm_Cmd = DNC_READ;
	rmsg.dm_Link = s->lat ? s->latid : s->link->dl_Id;
	rmsg.dm_Buf = netbuf;
	rmsg.dm_Len = NETBUF;
	rmsg.dm_Timeout = 1000;
	Forbid();
	dn = FindPort((STRPTR)(s->lat ? LAT_PORTNAME : DN_PORTNAME));
	if (dn)
		PutMsg(dn, &rmsg.dm_Msg);
	Permit();
	if (dn)
		read_out = TRUE;
	else
		link_down = TRUE;               /* dnetd is gone */
}

static void lat_keys(struct msession *s, const char *buf, long n);

/* the read came back: 1 = a second went by, 0 = data, -1 = link gone */
static int net_reply(struct msession *s)
{
	read_out = FALSE;
	if (rmsg.dm_Result == DNR_TIMEOUT)
		return 1;
	if (rmsg.dm_Result < 0 ||
	    (!s->lat && ctermh_net(&s->cth, netbuf, rmsg.dm_Result) < 0)) {
		link_down = TRUE;
		return -1;
	}
	if (s->lat)
		lat_keys(s, (const char *)netbuf, rmsg.dm_Result);
	return 0;
}

static void say(struct msession *s, const char *text)
{
	t_write(s, text, (long)strlen(text));
}

/* ---- the terminal: CTERM (ctermh) or LAT (raw stream) -------------------- */

/* keys to latd; waits for its reply (latd holds it while the session's
   buffer is full) */
static void lat_send(struct msession *s, const char *buf, long n)
{
	struct MsgPort *p;

	if (link_down || n <= 0)
		return;
	wmsg.dm_Msg.mn_Node.ln_Type = NT_MESSAGE;
	wmsg.dm_Msg.mn_ReplyPort = wport;
	wmsg.dm_Msg.mn_Length = sizeof wmsg;
	wmsg.dm_Cmd = DNC_WRITE;
	wmsg.dm_Link = s->latid;
	wmsg.dm_Buf = (APTR)buf;
	wmsg.dm_Len = n;
	Forbid();
	p = FindPort((STRPTR)LAT_PORTNAME);
	if (p)
		PutMsg(p, &wmsg.dm_Msg);
	Permit();
	if (!p) {
		link_down = TRUE;
		return;
	}
	while (!GetMsg(wport))
		WaitPort(wport);
	if (wmsg.dm_Result < 0)
		link_down = TRUE;
}

void t_write(struct msession *s, const char *buf, long n)
{
	char out[512];
	long i, o = 0;

	if (!s->lat) {
		ctermh_write(&s->cth, buf, n);
		return;
	}
	for (i = 0; i < n; i++) {
		if (o >= (long)sizeof out - 2) {
			lat_send(s, out, o);
			o = 0;
		}
		if (buf[i] == '\n' && !s->lout_cr)
			out[o++] = '\r';
		out[o++] = buf[i];
		s->lout_cr = buf[i] == '\r';
	}
	lat_send(s, out, o);
}

int t_read(struct msession *s, int kind, const char *prompt, long plen)
{
	char keys[sizeof s->ahead];
	int n;

	if (!s->lat)
		return ctermh_read(&s->cth, kind, prompt, plen, NULL, 0);
	if (prompt && plen > 0)
		t_write(s, prompt, plen);
	s->lread = kind + 1;
	s->llen = 0;
	if (s->aheadlen) {              /* typed before we asked */
		n = s->aheadlen;
		memcpy(keys, s->ahead, n);
		s->aheadlen = 0;
		lat_keys(s, keys, n);
	}
	return 0;
}

BOOL t_read_pending(struct msession *s)
{
	return s->lat ? s->lread != 0 : s->cth.read_pending != 0;
}

/* LAT tells no terminal size */
int t_width(struct msession *s)
{
	return !s->lat && s->cth.width > 0 ? s->cth.width : 80;
}

int t_height(struct msession *s)
{
	return !s->lat && s->cth.height > 0 ? s->cth.height : 24;
}

/* keys from the LAT terminal: to the pending read (key reads at once,
   the password up to CR, without echo), ^C/^Y in the Shell as a break,
   the rest kept until a read asks */
static void lat_keys(struct msession *s, const char *buf, long n)
{
	char got[sizeof s->ahead];
	long i, k = 0;
	char c;

	for (i = 0; i < n; i++) {
		c = buf[i];
		if (s->state == MS_SHELL && (c == 0x03 || c == 0x19)) {
			session_interrupt(s, c);
			continue;
		}
		if (s->lread == CTH_CHAR + 1) {
			if (k < (long)sizeof got)
				got[k++] = c;
			continue;
		}
		if (s->lread == CTH_SECRET + 1) {
			if (c == '\n' && s->lcr) {     /* CR LF: one end of line */
				s->lcr = FALSE;
				continue;
			}
			s->lcr = c == '\r';
			if (c == '\r' || c == '\n') {
				s->lread = 0;
				t_write(s, "\n", 1);       /* the end of the line shows */
				session_input(s, s->lline, s->llen, '\r');
				s->llen = 0;
			} else if (c == '\b' || c == 0x7f) {
				if (s->llen > 0)
					s->llen--;
			} else if (c == 0x15 || c == 0x18) {
				s->llen = 0;            /* ^U / ^X */
			} else if ((unsigned char)c >= 0x20 && s->llen < MAINT_PWMAX) {
				s->lline[s->llen++] = c;
			}
			continue;
		}
		if (s->aheadlen < (int)sizeof s->ahead)
			s->ahead[s->aheadlen++] = c;
	}
	if (k) {
		s->lread = 0;
		session_input(s, got, k, 0);
	}
}

/* set up the session and ctermh callbacks, send BIND; call
   session_end afterwards in any case, also when this fails */
int session_init(struct msession *s, struct mcfg *cfg, struct DnLink *l)
{
	memset(s, 0, sizeof *s);
	s->cfg = cfg;
	s->link = l;
	s->state = MS_BIND;
	read_out = FALSE;
	link_down = FALSE;
	netport = CreateMsgPort();
	wport = CreateMsgPort();
	if (!netport || !wport || con_start(s)) {
		if (netport)
			DeleteMsgPort(netport);
		if (wport)
			DeleteMsgPort(wport);
		netport = wport = NULL;
		return -1;
	}
	s->cth.net_write = session_net_write;
	s->cth.input = session_input;
	s->cth.interrupt = session_interrupt;
	s->cth.log = session_log;
	s->cth.ctx = s;
	ctermh_init(&s->cth);
	return ctermh_start(&s->cth);
}

/* a LAT session latd handed over (already accepted): straight to the
   password, no BIND; call session_end afterwards in any case */
int session_init_lat(struct msession *s, struct mcfg *cfg, ULONG latid)
{
	memset(s, 0, sizeof *s);
	s->cfg = cfg;
	s->lat = TRUE;
	s->latid = latid;
	s->state = MS_BIND;
	read_out = FALSE;
	link_down = FALSE;
	netport = CreateMsgPort();
	wport = CreateMsgPort();
	if (!netport || !wport || con_start(s)) {
		if (netport)
			DeleteMsgPort(netport);
		if (wport)
			DeleteMsgPort(wport);
		netport = wport = NULL;
		return -1;
	}
	s->state = MS_LOGIN;
	return login_start(s);
}

/* main loop until the session is over; 0 = ended normally */
int session_run(struct msession *s)
{
	ULONG nsig = 1UL << netport->mp_SigBit;
	ULONG csig = 1UL << s->conport->mp_SigBit;
	ULONG got;
	struct Message *m;
	long secs = 0;
	int rc = 0;

	issue_read(s);
	while (s->state != MS_DONE) {
		if (link_down) {
			rc = -1;
			s->state = MS_DONE;
			break;
		}
		got = Wait(nsig | csig | SIGBREAKF_CTRL_C);
		if (got & SIGBREAKF_CTRL_C) {
			if (s->state == MS_SHELL || s->state == MS_LOGIN)
				say(s, "\n*** dnmaint stopped on the Amiga\n");
			/* Wait() took the signal; main must see it as well */
			SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
			s->state = MS_DONE;
			break;
		}
		if (got & nsig) {
			while ((m = GetMsg(netport)) != NULL) {
				if (m != &rmsg.dm_Msg)
					continue;
				if (net_reply(s) > 0)
					secs++;
			}
			issue_read(s);
		}
		if (got & csig)
			while ((m = GetMsg(s->conport)) != NULL)
				con_packet(s, (struct DosPacket *)m->mn_Node.ln_Name);

		switch (s->state) {
		case MS_BIND:
			if (s->cth.unbound) {
				s->state = MS_DONE;
			} else if (s->cth.ready) {
				s->state = MS_LOGIN;
				secs = 0;
				if (login_start(s))
					s->state = MS_DONE;
			} else if (secs >= BIND_SECS) {
				rc = -1;
				s->state = MS_DONE;
			}
			break;
		case MS_LOGIN:
			if (s->cth.unbound) {
				s->state = MS_DONE;
			} else if (secs >= LOGIN_SECS) {
				say(s, "\nTimeout.\n");
				s->state = MS_DONE;
			}
			break;
		case MS_SHELL:
			/* the terminal may unbind at once; the Shell goes on
			   until it has seen EOF (con_stop in session_end) */
			if (s->shell_failed) {
				say(s, "Cannot start a Shell.\n");
				s->state = MS_DONE;
			} else if (s->shell_gone || s->cth.unbound)
				s->state = MS_DONE;
			break;
		}
	}
	return rc;
}

/* stop the Shell and the console, unbind, close the link */
void session_end(struct msession *s)
{
	struct Message *m;

	/* tell the terminal first: con_stop can wait for a busy command */
	if (!s->lat && !link_down && s->cth.bound && !s->cth.unbound)
		ctermh_unbind(&s->cth);
	con_stop(s);
	if (s->link) {
		/* dnetd replies the outstanding read before the close */
		DnClose(s->link);
		s->link = NULL;
	}
	if (s->lat && s->latid) {
		/* so does latd */
		lt_close(s->latid);
		s->latid = 0;
	}
	while (read_out) {
		WaitPort(netport);
		while ((m = GetMsg(netport)) != NULL)
			if (m == &rmsg.dm_Msg)
				read_out = FALSE;
	}
	if (netport) {
		DeleteMsgPort(netport);
		netport = NULL;
	}
	if (wport) {
		DeleteMsgPort(wport);
		wport = NULL;
	}
}

/* ctermh: one message to the terminal */
int session_net_write(void *ctx, const unsigned char *buf, long n)
{
	struct msession *s = ctx;

	if (link_down || !s->link)
		return -1;
	if (DnWrite(s->link, buf, n)) {
		link_down = TRUE;
		return -1;
	}
	return 0;
}

/* ctermh: a read is done; to login.c or the console by state */
void session_input(void *ctx, const char *buf, long n, int term)
{
	struct msession *s = ctx;
	int r;

	switch (s->state) {
	case MS_LOGIN:
		r = login_input(s, buf, n, term);
		if (r < 0) {
			s->state = MS_DONE;
		} else if (r > 0) {
			s->state = MS_SHELL;
			if (shell_start(s)) {
				say(s, "Cannot start a Shell.\n");
				s->state = MS_DONE;
			}
		}
		break;
	case MS_SHELL:
		con_input(s, buf, n, term);
		break;
	}
}

/* ctermh: ^C / ^Y; only the Shell gets them */
void session_interrupt(void *ctx, int ch)
{
	struct msession *s = ctx;

	if (s->state == MS_SHELL)
		con_interrupt(s, ch);
}

/* ctermh: debug log (ctermh calls it only with verbose set) */
void session_log(void *ctx, const char *msg)
{
	(void)ctx;
	mlog("dnmaint: %s\n", msg);
}

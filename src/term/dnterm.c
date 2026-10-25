/*
 * dnterm.c - dnterm.device: a serial.device look-alike that logs in to
 * DECnet hosts over CTERM.
 *
 * Terminal programs (Term, NComm, ...) open dnterm.device instead of
 * serial.device and see a Hayes modem: "ATD HOWIE" or "ATD 1.10" opens a
 * CTERM session to that node, "CONNECT" and the carrier bit in io_Status
 * tell that it is up, "NO CARRIER" that it is gone. "+++" (with a second
 * of silence before and after) returns to command mode, ATH hangs up,
 * ATO goes back online.
 *
 * Units 4 to 7 do the same with LAT services instead: "ATD HOWIE" there
 * opens a LAT session to the service HOWIE through latd (src/lat),
 * which carries the terminal stream as it is; SDCMD_BREAK sends a
 * BREAK.
 *
 * Every unit has a process of its own: it serves the IORequests, talks
 * to dnetd (or latd) and runs the CTERM client (src/cterm). It speaks
 * dnetd's private message protocol (dnmsg.h) directly, because
 * decnet.library only offers blocking calls and a terminal must read
 * and write at the same time; decnet.library is only used to look node
 * names up. latd takes the same messages on its port "LAT".
 *
 * Built without C startup code; AmigaOS 2.04, 68000.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <exec/resident.h>
#include <exec/devices.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/serial.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/decnet.h>

#include <string.h>

#include "../amiga/dnmsg.h"
#include "../cterm/cterm.h"
#include "../lat/latmsg.h"

#define DEVNAME   "dnterm.device"
#define VERSION   0
#define REVISION  3
#define IDSTRING  "dnterm.device 0.3 (25.10.2026)\r\n"

#define NUNITS    8
#define LATUNIT   4             /* units from here on are LAT */
#define RBUFSIZE  8192          /* data for the terminal program */
#define NETBUF    2048          /* one NSP message from the host */
#define TICK_MS   100
#define GUARD     10            /* "+++" guard time in ticks */

#define REG(r, decl) register decl __asm(#r)

/* device private io_Flags bits: where a request is linked */
#define IOF_INPORT  0x40        /* still in the unit's message port */
#define IOF_INLIST  0x20        /* in the list of waiting reads */

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
/* libnix's 32 bit multiply and divide (used by gcc) call utility.library */
extern struct Library *UtilityBase;

static const char devname[] = DEVNAME;
static const char idstring[] = IDSTRING;

struct DtUnit {
	struct Unit u;                  /* unit_MsgPort gets the IORequests */
	int num;
	int lat;                        /* a LAT unit: talks to latd */
	int shared;                     /* opened with SERF_SHARED */
	struct Process *proc;
	struct Message *quit;           /* from the last CloseDevice */
	char procname[24];

	/* serial side */
	struct List reads;              /* waiting CMD_READs */
	UBYTE *rbuf;
	ULONG rhead, rtail, rcount;
	UWORD status;                   /* io_Status bits besides carrier */
	UBYTE serflags;
	UBYTE term[8];                  /* EOF mode terminators */

	/* modem emulation */
	int online;                     /* CTERM link up */
	int cmdmode;                    /* AT commands, not data */
	int dialing;                    /* waiting for CONNECT */
	int connect_out;                /* cmsg is at dnetd */
	int abandon;                    /* a key stopped dialing */
	int echo, quiet, verbose;
	char cmd[80], last[80];
	unsigned cmdlen;
	int plus;                       /* "+++" typed so far */
	ULONG ticks, last_data, plus_tick;

	/* dnetd */
	struct MsgPort *netport;        /* connect and read replies */
	struct MsgPort *wport;          /* write and close replies */
	struct DnMsg cmsg, rmsg, wmsg;
	struct DnConnectReq creq;
	struct LatConnect lconn;
	ULONG link;
	int read_out;                   /* rmsg is at dnetd */
	struct Library *decnet;
	UBYTE net[NETBUF];

	struct cterm ct;

	struct MsgPort *tport;
	struct timerequest *treq;
	int timer_out;
};

struct DtBase {
	struct Device dev;
	BPTR seglist;
	struct DtUnit *units[NUNITS];
};

struct DtStart {
	struct Message msg;
	struct DtUnit *u;
	LONG ok;
};

/* ---- read buffer -------------------------------------------------------- */

static int is_term(struct DtUnit *u, UBYTE c)
{
	int i;

	for (i = 0; i < 8; i++)
		if (u->term[i] == c)
			return 1;
	return 0;
}

/* io_Status with the carrier bit (low active); only in IOExtSer sized
   requests */
static void set_status(struct DtUnit *u, struct IOExtSer *io)
{
	if (io->IOSer.io_Message.mn_Length >= sizeof(struct IOExtSer))
		io->io_Status = u->status | (u->online ? 0 : 1 << 5);
}

static void reply_io(struct IOStdReq *io)
{
	if (!(io->io_Flags & IOF_QUICK))
		ReplyMsg(&io->io_Message);
}

/* Move buffered bytes into a read; call under Forbid. The read is done
   when it has io_Length bytes, a NUL with io_Length -1, or an EOF
   character. */
static int fill(struct DtUnit *u, struct IOStdReq *r)
{
	LONG want = (LONG)r->io_Length;
	int done = want == 0;

	while (!done && u->rcount) {
		UBYTE c = u->rbuf[u->rtail];
		u->rtail = (u->rtail + 1) % RBUFSIZE;
		u->rcount--;
		((UBYTE *)r->io_Data)[r->io_Actual++] = c;
		if ((want > 0 && (LONG)r->io_Actual >= want) || (want < 0 && c == 0) ||
		    ((u->serflags & SERF_EOFMODE) && is_term(u, c)))
			done = 1;
	}
	return done;
}

/* Would a read be done with what is buffered now? Under Forbid. */
static int enough(struct DtUnit *u, struct IOStdReq *r)
{
	LONG want = (LONG)r->io_Length;
	ULONG i, p;

	if (want == 0 || (want > 0 && u->rcount >= (ULONG)want))
		return 1;
	for (i = 0, p = u->rtail; i < u->rcount; i++, p = (p + 1) % RBUFSIZE) {
		UBYTE c = u->rbuf[p];
		if ((want < 0 && c == 0) || ((u->serflags & SERF_EOFMODE) && is_term(u, c)))
			return 1;
	}
	return 0;
}

/* Fill the waiting reads from the buffer. */
static void deliver(struct DtUnit *u)
{
	struct IOExtSer *io;

	Forbid();
	while ((io = (struct IOExtSer *)u->reads.lh_Head)->IOSer.io_Message.mn_Node.ln_Succ) {
		struct IOStdReq *r = &io->IOSer;

		if (!fill(u, r))
			break;
		Remove(&r->io_Message.mn_Node);
		r->io_Flags &= ~IOF_INLIST;
		set_status(u, io);
		reply_io(r);
	}
	Permit();
}

static void put(struct DtUnit *u, const char *buf, long n)
{
	Forbid();                       /* BeginIO reads the buffer too */
	while (n-- > 0) {
		if (u->rcount >= RBUFSIZE) {
			u->status |= IO_STATF_OVERRUN;
			break;
		}
		u->rbuf[u->rhead] = *buf++;
		u->rhead = (u->rhead + 1) % RBUFSIZE;
		u->rcount++;
	}
	Permit();
	deliver(u);
}

static void put_str(struct DtUnit *u, const char *s)
{
	put(u, s, strlen(s));
}

static void abort_reads(struct DtUnit *u)
{
	struct IOStdReq *r;

	Forbid();
	while ((r = (struct IOStdReq *)RemHead(&u->reads)) != NULL) {
		r->io_Flags &= ~IOF_INLIST;
		r->io_Error = IOERR_ABORTED;
		reply_io(r);
	}
	Permit();
}

/* modem result codes */
enum { R_OK, R_CONNECT, R_RING, R_NOCARRIER, R_ERROR, R_NODIALTONE = 6,
       R_BUSY, R_NOANSWER };

static void result(struct DtUnit *u, int r);

/* ---- dnetd -------------------------------------------------------------- */

/* to dnetd, or to latd on a LAT unit */
static int send_dnetd(struct DtUnit *u, struct DnMsg *m, struct MsgPort *reply)
{
	struct MsgPort *dn;

	m->dm_Msg.mn_Node.ln_Type = NT_MESSAGE;
	m->dm_Msg.mn_ReplyPort = reply;
	m->dm_Msg.mn_Length = sizeof(struct DnMsg);
	Forbid();
	dn = FindPort((STRPTR)(u->lat ? LAT_PORTNAME : DN_PORTNAME));
	if (dn)
		PutMsg(dn, &m->dm_Msg);
	Permit();
	return dn != NULL;
}

/* write and close wait for their reply; the read stays at dnetd */
static int transact(struct DtUnit *u, struct DnMsg *m)
{
	if (!send_dnetd(u, m, u->wport))
		return 0;
	while (!GetMsg(u->wport))
		WaitPort(u->wport);
	return 1;
}

static void issue_read(struct DtUnit *u)
{
	if (u->read_out || !u->online || u->rcount > RBUFSIZE - 2 * NETBUF)
		return;
	u->rmsg.dm_Cmd = DNC_READ;
	u->rmsg.dm_Link = u->link;
	u->rmsg.dm_Buf = u->net;
	u->rmsg.dm_Len = NETBUF;
	u->rmsg.dm_Timeout = 0;
	u->read_out = send_dnetd(u, &u->rmsg, u->netport);
	if (!u->read_out) {
		u->online = 0;                  /* dnetd is gone */
		u->cmdmode = 1;
		result(u, R_NOCARRIER);
	}
}

static void close_link(struct DtUnit *u, ULONG id)
{
	u->wmsg.dm_Cmd = DNC_CLOSE;
	u->wmsg.dm_Link = id;
	transact(u, &u->wmsg);
}

static int net_write(void *ctx, const unsigned char *buf, long n)
{
	struct DtUnit *u = ctx;

	if (!u->online)
		return -1;
	u->wmsg.dm_Cmd = DNC_WRITE;
	u->wmsg.dm_Link = u->link;
	u->wmsg.dm_Buf = (APTR)buf;
	u->wmsg.dm_Len = n;
	if (!transact(u, &u->wmsg) || u->wmsg.dm_Result < 0)
		return -1;
	return 0;
}

static void term_write(void *ctx, const char *buf, long n)
{
	put(ctx, buf, n);
}

/* ---- modem emulation ----------------------------------------------------- */

static void result(struct DtUnit *u, int r)
{
	static const char *const text[] = {
		"OK", "CONNECT", "RING", "NO CARRIER", "ERROR", "", "NO DIALTONE",
		"BUSY", "NO ANSWER"
	};
	char num[4];

	if (u->quiet)
		return;
	if (u->verbose) {
		put_str(u, "\r\n");
		put_str(u, text[r]);
		put_str(u, "\r\n");
	} else {
		num[0] = '0' + r;
		num[1] = '\r';
		put(u, num, 2);
	}
}

/* a line of text before a result code, only with verbose results */
static void say(struct DtUnit *u, const char *s)
{
	if (!u->quiet && u->verbose) {
		put_str(u, "\r\n");
		put_str(u, s);
	}
}

static int upper(int c)
{
	return c >= 'a' && c <= 'z' ? c - 32 : c;
}

static void hangup(struct DtUnit *u)
{
	if (u->online) {
		u->online = 0;
		close_link(u, u->link);
		/* dnetd has replied the waiting read before the close */
		while (u->read_out) {
			struct Message *m;
			WaitPort(u->netport);
			while ((m = GetMsg(u->netport)) != NULL) {
				if (m == &u->rmsg.dm_Msg)
					u->read_out = 0;
			}
		}
	}
	u->cmdmode = 1;
	u->plus = 0;
}

static void reset_modem(struct DtUnit *u)
{
	u->echo = 1;
	u->quiet = 0;
	u->verbose = 1;
}

static void dial(struct DtUnit *u, const char *name)
{
	struct Library *DECnetBase;
	LONG addr;

	if (u->online || u->dialing) {
		result(u, R_ERROR);
		return;
	}
	if (!*name) {
		say(u, u->lat ? "No service given (ATD SERVICE)" : "No node given (ATD NODE)");
		result(u, R_ERROR);
		return;
	}
	if (u->lat) {
		/* latd skips a T or P of ATDT/ATDP itself if needed */
		memset(&u->lconn, 0, sizeof u->lconn);
		strncpy(u->lconn.lc_Service, name, sizeof u->lconn.lc_Service - 1);
		strcpy(u->lconn.lc_Port, "LAT0");
		u->lconn.lc_Port[3] = '0' + u->num;
		u->cmsg.dm_Cmd = DNC_CONNECT;
		u->cmsg.dm_Buf = &u->lconn;
		u->cmsg.dm_Req = NULL;
		u->cmsg.dm_Timeout = 30000;
		if (!send_dnetd(u, &u->cmsg, u->netport)) {
			say(u, "LAT is not running (latd)");
			result(u, R_NODIALTONE);
			return;
		}
		u->dialing = 1;
		u->connect_out = 1;
		u->abandon = 0;
		return;
	}
	if (!u->decnet)
		u->decnet = OpenLibrary((STRPTR)DECNETNAME, 1);
	DECnetBase = u->decnet;
	if (!DECnetBase || !FindPort((STRPTR)DN_PORTNAME)) {
		say(u, DECnetBase ? "DECnet is not running" : "decnet.library not found");
		result(u, R_NODIALTONE);
		return;
	}
	addr = DnNodeAddr((CONST_STRPTR)name);
	if (addr < 0 && (upper(*name) == 'T' || upper(*name) == 'P')) {
		/* ATDT / ATDP: tone or pulse dialing, as a phone book sends it */
		const char *n = name + 1;
		while (*n == ' ')
			n++;
		if (*n)
			addr = DnNodeAddr((CONST_STRPTR)n);
	}
	if (addr < 0) {
		say(u, "Unknown node");
		result(u, R_ERROR);
		return;
	}
	memset(&u->creq, 0, sizeof u->creq);
	u->creq.dcr_Node = (UWORD)addr;
	u->creq.dcr_ObjNum = CTERM_OBJECT;
	u->creq.dcr_SrcName = (CONST_STRPTR)"DNTERM";
	u->creq.dcr_Timeout = 30000;
	u->cmsg.dm_Cmd = DNC_CONNECT;
	u->cmsg.dm_Req = &u->creq;
	u->cmsg.dm_Buf = NULL;
	if (!send_dnetd(u, &u->cmsg, u->netport)) {
		say(u, "DECnet is not running");
		result(u, R_NODIALTONE);
		return;
	}
	u->dialing = 1;
	u->connect_out = 1;
	u->abandon = 0;
}

static void connect_done(struct DtUnit *u)
{
	LONG reason = u->cmsg.dm_Reason;
	const char *why = NULL;
	int abandon = u->abandon;

	u->dialing = u->connect_out = u->abandon = 0;
	if (u->cmsg.dm_Result == 0) {
		if (abandon) {
			close_link(u, u->cmsg.dm_Link);
			return;
		}
		u->link = u->cmsg.dm_Link;
		u->online = 1;
		u->cmdmode = 0;
		u->plus = 0;
		u->last_data = u->ticks;
		if (!u->lat)
			cterm_init(&u->ct);
		result(u, R_CONNECT);
		issue_read(u);
		return;
	}
	if (abandon)
		return;
	if (u->lat) {
		/* LAT reasons, see src/lat/lat.h */
		switch (reason) {
		case 6:  result(u, R_BUSY); return;         /* service in use */
		case 32: result(u, R_NOANSWER); return;     /* node does not answer */
		case 7:  why = "Unknown LAT service"; break;
		case 8:  why = "Service is disabled"; break;
		case 5:  why = "No resources"; break;
		case 11:
		case 14: why = "Access refused"; break;
		}
		if (why)
			say(u, why);
		result(u, R_NOCARRIER);
		return;
	}
	switch (reason) {
	case DNREASON_BUSY:        result(u, R_BUSY); return;
	case DNREASON_TIMEOUT:     result(u, R_NOANSWER); return;
	case DNREASON_NODNETD:
	case DNREASON_SHUTDOWN:    result(u, R_NODIALTONE); return;
	case DNREASON_UNKNOWNNODE: why = "Node does not know us"; break;
	case DNREASON_NOOBJECT:    why = "Node has no CTERM server"; break;
	case DNREASON_RESOURCES:   why = "No resources"; break;
	case DNREASON_ACCESS:      why = "Access refused"; break;
	case DNREASON_UNREACHABLE: why = "Node unreachable"; break;
	}
	if (why)
		say(u, why);
	result(u, R_NOCARRIER);
}

static int number(const char **p)
{
	int v = 0;

	while (**p >= '0' && **p <= '9')
		v = v * 10 + *(*p)++ - '0';
	return v;
}

static void at_command(struct DtUnit *u, const char *s)
{
	const char *p;
	int r = R_OK, v;

	while (*s == ' ')
		s++;
	if (upper(s[0]) != 'A' || upper(s[1]) != 'T')
		return;                         /* modems ignore other lines */
	strcpy(u->last, s);
	for (p = s + 2; *p && r == R_OK; ) {
		int c = upper(*p++);
		switch (c) {
		case ' ':
			break;
		case 'D': {
			char name[40];
			unsigned n = 0;
			while (*p == ' ')
				p++;
			while (*p && *p != ';' && n < sizeof name - 1)
				name[n++] = *p++;
			while (n && name[n - 1] == ' ')
				n--;
			name[n] = 0;
			dial(u, name);
			return;                 /* the result comes with the link */
		}
		case 'E': u->echo = number(&p) != 0; break;
		case 'Q': u->quiet = number(&p) != 0; break;
		case 'V': u->verbose = number(&p) != 0; break;
		case 'H':
			number(&p);
			hangup(u);
			break;
		case 'Z':
			number(&p);
			hangup(u);
			reset_modem(u);
			break;
		case 'I':
			number(&p);
			say(u, u->lat ? "dnterm.device 0.3, LAT" : "dnterm.device 0.3, DECnet CTERM");
			break;
		case 'O':
			number(&p);
			if (!u->online) {
				r = R_ERROR;
				break;
			}
			u->cmdmode = 0;
			u->last_data = u->ticks;
			result(u, R_CONNECT);
			return;
		case 'S':
			number(&p);
			if (*p == '=') {
				p++;
				number(&p);
			} else if (*p == '?') {
				p++;
				say(u, "000");
			}
			break;
		case '&':
			if (*p)
				p++;
			number(&p);
			break;
		case '+': case '\\': case '%': case '#': case '*':
			p += strlen(p);         /* extended commands: accepted */
			break;
		default:
			if (c >= 'A' && c <= 'Z') {
				v = number(&p);     /* B C L M N X ...: accepted */
				(void)v;
			} else {
				r = R_ERROR;
			}
		}
	}
	result(u, r);
}

static void cmd_char(struct DtUnit *u, char c)
{
	if (c == '\n')
		return;
	if (u->dialing) {               /* any key stops dialing */
		u->dialing = 0;
		u->abandon = 1;
		result(u, R_NOCARRIER);
		return;
	}
	if (c == '\b' || c == 0x7f) {
		if (u->cmdlen) {
			u->cmdlen--;
			if (u->echo)
				put(u, "\b \b", 3);
		}
		return;
	}
	if (u->echo)
		put(u, &c, 1);
	if (c == '\r') {
		u->cmd[u->cmdlen] = 0;
		u->cmdlen = 0;
		at_command(u, u->cmd);
		return;
	}
	if (u->cmdlen < sizeof u->cmd - 1)
		u->cmd[u->cmdlen++] = c;
	if (u->cmdlen == 2 && upper(u->cmd[0]) == 'A' && u->cmd[1] == '/') {
		u->cmdlen = 0;                  /* A/ repeats without CR */
		if (u->echo)
			put_str(u, "\r");
		at_command(u, u->last);
	}
}

/* keys for the host: through CTERM, or as they are to latd */
static void keys(struct DtUnit *u, const char *buf, long n)
{
	if (u->lat)
		net_write(u, (const unsigned char *)buf, n);
	else
		cterm_keys(&u->ct, buf, n);
}

static void flush_plus(struct DtUnit *u)
{
	static const char plus[] = "+++";

	if (u->plus) {
		keys(u, plus, u->plus);
		u->plus = 0;
		u->last_data = u->ticks;
	}
}

static void data_chars(struct DtUnit *u, const char *buf, long n)
{
	long i;

	for (i = 0; i < n; i++) {
		if (buf[i] == '+' && u->plus < 3 &&
		    (u->plus || u->ticks - u->last_data >= GUARD)) {
			u->plus++;              /* held back until the guard time */
			u->plus_tick = u->ticks;
			continue;
		}
		flush_plus(u);
		keys(u, buf + i, 1);
		u->last_data = u->ticks;
	}
}

static void tick(struct DtUnit *u)
{
	u->ticks++;
	if (u->plus && u->ticks - u->plus_tick >= GUARD) {
		if (u->plus == 3) {
			u->plus = 0;
			u->cmdmode = 1;
			result(u, R_OK);
		} else {
			flush_plus(u);
		}
	}
	if (u->online && !u->lat)
		cterm_tick(&u->ct, TICK_MS);
}

/* ---- IORequests in the unit process --------------------------------------- */

static void do_write(struct DtUnit *u, struct IOStdReq *r)
{
	const char *p = r->io_Data;
	LONG n = (LONG)r->io_Length;

	if (n < 0)
		n = strlen(p);
	if (u->cmdmode || !u->online) {
		LONG i;
		for (i = 0; i < n; i++)
			cmd_char(u, p[i]);
	} else {
		data_chars(u, p, n);
	}
	r->io_Actual = n;
}

static void do_io(struct DtUnit *u, struct IOExtSer *io)
{
	struct IOStdReq *r = &io->IOSer;

	switch (r->io_Command) {
	case CMD_READ:
		r->io_Actual = 0;
		Forbid();
		AddTail(&u->reads, &r->io_Message.mn_Node);
		r->io_Flags |= IOF_INLIST;
		Permit();
		deliver(u);
		return;
	case CMD_WRITE:
		do_write(u, r);
		break;
	case CMD_CLEAR:
		Forbid();
		u->rhead = u->rtail = u->rcount = 0;
		Permit();
		u->status &= ~IO_STATF_OVERRUN;
		break;
	case CMD_RESET:
		abort_reads(u);
		Forbid();
		u->rhead = u->rtail = u->rcount = 0;
		Permit();
		u->status = 0;
		u->serflags = 0;
		memset(u->term, 0, sizeof u->term);
		break;
	case CMD_FLUSH:
		abort_reads(u);
		break;
	case SDCMD_QUERY:
		r->io_Actual = u->rcount;
		set_status(u, io);
		break;
	case SDCMD_SETPARAMS:
		if (r->io_Message.mn_Length >= sizeof(struct IOExtSer)) {
			u->serflags = io->io_SerFlags;
			memcpy(u->term, &io->io_TermArray, 8);
		}
		break;
	case SDCMD_BREAK:
		if (u->lat && u->online) {
			u->wmsg.dm_Cmd = LTC_BREAK;
			u->wmsg.dm_Link = u->link;
			transact(u, &u->wmsg);
		}
		break;
	default:                        /* CMD_START, CMD_STOP */
		break;
	}
	reply_io(r);
}

static void handle_port(struct DtUnit *u)
{
	struct Message *m;

	for (;;) {
		Forbid();
		m = GetMsg(&u->u.unit_MsgPort);
		if (m && m != u->quit)
			((struct IOStdReq *)m)->io_Flags &= ~IOF_INPORT;
		Permit();
		if (!m)
			break;
		if (m == u->quit)
			continue;               /* seen by the main loop */
		do_io(u, (struct IOExtSer *)m);
	}
}

static void handle_net(struct DtUnit *u)
{
	struct Message *m;

	while ((m = GetMsg(u->netport)) != NULL) {
		if (m == &u->cmsg.dm_Msg) {
			connect_done(u);
		} else if (m == &u->rmsg.dm_Msg) {
			LONG n = u->rmsg.dm_Result;
			u->read_out = 0;
			if (!u->online)
				continue;
			if (n > 0 && u->lat) {
				put(u, (const char *)u->net, n);
				issue_read(u);
			} else if (n > 0 && cterm_net(&u->ct, u->net, n) >= 0 && !u->ct.unbound) {
				issue_read(u);
			} else if (n == DNR_TIMEOUT) {
				issue_read(u);
			} else {
				/* the host logged out or the link is gone */
				hangup(u);
				result(u, R_NOCARRIER);
			}
		}
	}
}

static void start_timer(struct DtUnit *u)
{
	u->treq->tr_node.io_Command = TR_ADDREQUEST;
	u->treq->tr_time.tv_secs = 0;
	u->treq->tr_time.tv_micro = TICK_MS * 1000;
	SendIO(&u->treq->tr_node);
	u->timer_out = 1;
}

static void cleanup(struct DtUnit *u)
{
	if (u->timer_out) {
		AbortIO(&u->treq->tr_node);
		WaitIO(&u->treq->tr_node);
	}
	if (u->treq) {
		if (u->treq->tr_node.io_Device)
			CloseDevice(&u->treq->tr_node);
		DeleteIORequest(&u->treq->tr_node);
	}
	if (u->tport)
		DeleteMsgPort(u->tport);
	if (u->netport)
		DeleteMsgPort(u->netport);
	if (u->wport)
		DeleteMsgPort(u->wport);
	if (u->decnet)
		CloseLibrary(u->decnet);
	if (u->rbuf)
		FreeVec(u->rbuf);
	if (u->u.unit_MsgPort.mp_SigBit != (UBYTE)-1)
		FreeSignal(u->u.unit_MsgPort.mp_SigBit);
}

static void new_list(struct List *l)
{
	l->lh_Head = (struct Node *)&l->lh_Tail;
	l->lh_Tail = NULL;
	l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static int setup(struct DtUnit *u)
{
	struct MsgPort *p = &u->u.unit_MsgPort;
	BYTE sig = AllocSignal(-1);

	p->mp_SigBit = (UBYTE)sig;
	if (sig < 0)
		return 0;
	p->mp_Node.ln_Type = NT_MSGPORT;
	p->mp_Flags = PA_SIGNAL;
	p->mp_SigTask = FindTask(NULL);
	new_list(&p->mp_MsgList);
	new_list(&u->reads);

	u->rbuf = AllocVec(RBUFSIZE, MEMF_ANY);
	u->netport = CreateMsgPort();
	u->wport = CreateMsgPort();
	u->tport = CreateMsgPort();
	if (!u->rbuf || !u->netport || !u->wport || !u->tport)
		return 0;
	u->treq = (struct timerequest *)CreateIORequest(u->tport, sizeof(struct timerequest));
	if (!u->treq)
		return 0;
	if (OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, &u->treq->tr_node, 0)) {
		u->treq->tr_node.io_Device = NULL;
		return 0;
	}

	u->ct.net_write = net_write;
	u->ct.term_write = term_write;
	u->ct.ctx = u;
	strcpy(u->ct.revision, "dnterm");
	cterm_init(&u->ct);
	u->cmdmode = 1;
	reset_modem(u);
	return 1;
}

static void unit_main(void)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	struct DtStart *sm;
	struct DtUnit *u;
	ULONG psig, nsig, tsig;

	WaitPort(&me->pr_MsgPort);
	sm = (struct DtStart *)GetMsg(&me->pr_MsgPort);
	u = sm->u;
	u->u.unit_MsgPort.mp_SigBit = (UBYTE)-1;
	if (!setup(u)) {
		cleanup(u);
		Forbid();
		ReplyMsg(&sm->msg);         /* ok = 0 */
		return;
	}
	sm->ok = 1;
	ReplyMsg(&sm->msg);

	psig = 1UL << u->u.unit_MsgPort.mp_SigBit;
	nsig = 1UL << u->netport->mp_SigBit;
	tsig = 1UL << u->tport->mp_SigBit;
	start_timer(u);
	while (!u->quit) {
		ULONG s = Wait(psig | nsig | tsig);
		if (s & tsig) {
			if (GetMsg(u->tport)) {
				u->timer_out = 0;
				tick(u);
				start_timer(u);
			}
		}
		if (s & nsig)
			handle_net(u);
		if (s & psig)
			handle_port(u);
		issue_read(u);                  /* if it waited for buffer space */
	}

	/* last CloseDevice: no requests are left */
	if (u->connect_out) {
		/* the connect is still at dnetd; it has a timeout */
		u->abandon = 1;
		while (u->connect_out) {
			WaitPort(u->netport);
			handle_net(u);
		}
	}
	hangup(u);
	abort_reads(u);
	for (;;) {                              /* the quit message, if still there */
		struct Message *m;
		Forbid();
		m = GetMsg(&u->u.unit_MsgPort);
		Permit();
		if (!m)
			break;
		if (m != u->quit) {
			((struct IOStdReq *)m)->io_Error = IOERR_ABORTED;
			ReplyMsg(m);
		}
	}
	cleanup(u);
	Forbid();                               /* CloseDevice waits for this */
	ReplyMsg(u->quit);
}

/* ---- device interface ----------------------------------------------------- */

static struct DtUnit *start_unit(int num)
{
	struct DtUnit *u = AllocVec(sizeof *u, MEMF_PUBLIC | MEMF_CLEAR);
	struct DtStart sm;
	struct MsgPort *port;
	char *n;

	if (!u)
		return NULL;
	u->num = num;
	u->lat = num >= LATUNIT;
	strcpy(u->procname, "dnterm.device unit 0");
	n = u->procname + strlen(u->procname) - 1;
	*n = '0' + num;
	port = CreateMsgPort();
	u->proc = port ? CreateNewProcTags(NP_Entry, (ULONG)unit_main,
	                                   NP_Name, (ULONG)u->procname,
	                                   NP_Priority, 1,  /* before busy-polling callers */
	                                   NP_StackSize, 8192,
	                                   TAG_DONE) : NULL;
	if (!u->proc) {
		if (port)
			DeleteMsgPort(port);
		FreeVec(u);
		return NULL;
	}
	memset(&sm, 0, sizeof sm);
	sm.msg.mn_Node.ln_Type = NT_MESSAGE;
	sm.msg.mn_ReplyPort = port;
	sm.msg.mn_Length = sizeof sm;
	sm.u = u;
	PutMsg(&u->proc->pr_MsgPort, &sm.msg);
	WaitPort(port);
	GetMsg(port);
	DeleteMsgPort(port);
	if (!sm.ok) {
		FreeVec(u);
		return NULL;
	}
	return u;
}

static void stop_unit(struct DtUnit *u)
{
	struct Message qm;
	struct MsgPort *port = CreateMsgPort();

	if (!port)
		return;                         /* leaves the unit running */
	memset(&qm, 0, sizeof qm);
	qm.mn_Node.ln_Type = NT_MESSAGE;
	qm.mn_ReplyPort = port;
	qm.mn_Length = sizeof qm;
	Forbid();
	u->quit = &qm;
	PutMsg(&u->u.unit_MsgPort, &qm);
	Permit();
	WaitPort(port);
	GetMsg(port);
	DeleteMsgPort(port);
	FreeVec(u);
}

static void set_defaults(struct IOExtSer *io)
{
	io->io_CtlChar = SER_DEFAULT_CTLCHAR;
	io->io_RBufLen = RBUFSIZE;
	io->io_ExtFlags = 0;
	io->io_Baud = 9600;
	io->io_BrkTime = 250000;
	io->io_TermArray.TermArray0 = 0;
	io->io_TermArray.TermArray1 = 0;
	io->io_ReadLen = 8;
	io->io_WriteLen = 8;
	io->io_StopBits = 1;
	io->io_Status = 0;
}

static BPTR DevExpunge(REG(a6, struct DtBase *base))
{
	BPTR seg;

	if (base->dev.dd_Library.lib_OpenCnt) {
		base->dev.dd_Library.lib_Flags |= LIBF_DELEXP;
		return 0;
	}
	seg = base->seglist;
	Remove(&base->dev.dd_Library.lib_Node);
	CloseLibrary(UtilityBase);
	CloseLibrary((struct Library *)DOSBase);
	FreeMem((UBYTE *)base - base->dev.dd_Library.lib_NegSize,
	        base->dev.dd_Library.lib_NegSize + base->dev.dd_Library.lib_PosSize);
	return seg;
}

static void DevOpen(REG(a1, struct IOExtSer *io), REG(d0, ULONG unit), REG(d1, ULONG flags),
                    REG(a6, struct DtBase *base))
{
	struct DtUnit *u;
	/* short requests only from tools like Version (2.04 opens the device) */
	int ext = io->IOSer.io_Message.mn_Length >= sizeof(struct IOExtSer);
	int shared = ext && (io->io_SerFlags & SERF_SHARED) != 0;

	(void)flags;
	base->dev.dd_Library.lib_OpenCnt++;     /* no expunge while we wait */
	io->IOSer.io_Error = IOERR_OPENFAIL;
	if (unit >= NUNITS) {
		base->dev.dd_Library.lib_OpenCnt--;
		return;
	}
	u = base->units[unit];
	if (u && (!shared || !u->shared)) {
		io->IOSer.io_Error = SerErr_DevBusy;
		base->dev.dd_Library.lib_OpenCnt--;
		return;
	}
	if (!u) {
		u = start_unit((int)unit);
		if (!u) {
			base->dev.dd_Library.lib_OpenCnt--;
			return;
		}
		u->shared = shared;
		base->units[unit] = u;
	}
	u->u.unit_OpenCnt++;
	if (ext)
		set_defaults(io);
	io->IOSer.io_Unit = &u->u;
	io->IOSer.io_Device = &base->dev;
	io->IOSer.io_Error = 0;
	base->dev.dd_Library.lib_Flags &= ~LIBF_DELEXP;
}

static BPTR DevClose(REG(a1, struct IOExtSer *io), REG(a6, struct DtBase *base))
{
	struct DtUnit *u = (struct DtUnit *)io->IOSer.io_Unit;

	io->IOSer.io_Unit = (struct Unit *)-1;
	io->IOSer.io_Device = (struct Device *)-1;
	if (u && --u->u.unit_OpenCnt == 0) {
		base->units[u->num] = NULL;
		stop_unit(u);
	}
	if (--base->dev.dd_Library.lib_OpenCnt == 0 &&
	    (base->dev.dd_Library.lib_Flags & LIBF_DELEXP))
		return DevExpunge(base);
	return 0;
}

static void DevBeginIO(REG(a1, struct IOExtSer *io), REG(a6, struct DtBase *base))
{
	struct IOStdReq *r = &io->IOSer;
	struct DtUnit *u = (struct DtUnit *)r->io_Unit;

	(void)base;
	r->io_Error = 0;
	r->io_Message.mn_Node.ln_Type = NT_MESSAGE;

	/* Like serial.device: a quick read that the buffer satisfies, or a
	   quick query, is done at once in the caller's task. Programs that
	   poll in a loop (VAXterm) depend on it. Only when nothing else of
	   this unit waits, so the order stays. */
	if ((r->io_Flags & IOF_QUICK) &&
	    (r->io_Command == CMD_READ || r->io_Command == SDCMD_QUERY)) {
		int done = 0, wake = 0;
		Forbid();
		if (IsListEmpty(&u->reads) && IsListEmpty(&u->u.unit_MsgPort.mp_MsgList)) {
			if (r->io_Command == SDCMD_QUERY) {
				r->io_Actual = u->rcount;
				done = 1;
			} else if (enough(u, r)) {
				wake = u->rcount > RBUFSIZE - 2 * NETBUF;
				r->io_Actual = 0;
				fill(u, r);
				/* the unit holds back its network read while the
				   buffer is full; let it look again */
				wake = wake && u->rcount <= RBUFSIZE - 2 * NETBUF;
				done = 1;
			}
			if (done)
				set_status(u, io);
		}
		if (wake)
			Signal(u->u.unit_MsgPort.mp_SigTask, 1UL << u->u.unit_MsgPort.mp_SigBit);
		Permit();
		if (done)
			return;                 /* IOF_QUICK stays set: no reply */
	}

	switch (r->io_Command) {
	case CMD_READ: case CMD_WRITE: case CMD_CLEAR: case CMD_RESET: case CMD_FLUSH:
	case CMD_START: case CMD_STOP:
	case SDCMD_QUERY: case SDCMD_BREAK: case SDCMD_SETPARAMS:
		Forbid();
		r->io_Flags = (r->io_Flags & ~IOF_QUICK) | IOF_INPORT;
		PutMsg(&u->u.unit_MsgPort, &r->io_Message);
		Permit();
		break;
	default:
		r->io_Error = IOERR_NOCMD;
		reply_io(r);
	}
}

static LONG DevAbortIO(REG(a1, struct IOExtSer *io), REG(a6, struct DtBase *base))
{
	struct IOStdReq *r = &io->IOSer;
	LONG err = 0;

	(void)base;
	Forbid();
	if (r->io_Flags & (IOF_INPORT | IOF_INLIST)) {
		Remove(&r->io_Message.mn_Node);
		r->io_Flags &= ~(IOF_INPORT | IOF_INLIST);
		r->io_Error = IOERR_ABORTED;
		ReplyMsg(&r->io_Message);
	} else {
		err = -1;                       /* already done */
	}
	Permit();
	return err;
}

static LONG DevReserved(void)
{
	return 0;
}

static struct DtBase *DevInit(REG(d0, struct DtBase *base), REG(a0, BPTR seg),
                              REG(a6, struct ExecBase *sysbase))
{
	SysBase = sysbase;
	base->seglist = seg;
	base->dev.dd_Library.lib_Node.ln_Type = NT_DEVICE;
	base->dev.dd_Library.lib_Node.ln_Name = (char *)devname;
	base->dev.dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
	base->dev.dd_Library.lib_Version = VERSION;
	base->dev.dd_Library.lib_Revision = REVISION;
	base->dev.dd_Library.lib_IdString = (APTR)idstring;
	DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);
	UtilityBase = OpenLibrary((STRPTR)"utility.library", 37);
	if (!DOSBase || !UtilityBase) {
		CloseLibrary(UtilityBase);
		CloseLibrary((struct Library *)DOSBase);
		FreeMem((UBYTE *)base - base->dev.dd_Library.lib_NegSize,
		        base->dev.dd_Library.lib_NegSize + base->dev.dd_Library.lib_PosSize);
		return NULL;
	}
	return base;
}

static const APTR functable[] = {
	(APTR)DevOpen,
	(APTR)DevClose,
	(APTR)DevExpunge,
	(APTR)DevReserved,
	(APTR)DevBeginIO,
	(APTR)DevAbortIO,
	(APTR)-1
};

static const ULONG inittable[] = {
	sizeof(struct DtBase),
	(ULONG)functable,
	0,
	(ULONG)DevInit
};

/* ramlib only finds a longword aligned romtag */
const struct Resident romtag __attribute__((aligned(4))) = {
	RTC_MATCHWORD,
	(struct Resident *)&romtag,
	(APTR)(&romtag + 1),
	RTF_AUTOINIT,
	VERSION,
	NT_DEVICE,
	0,
	(char *)devname,
	(char *)idstring,
	(APTR)inittable
};

/*
 * dnmaint.h - maintenance login service: shared declarations.
 *
 * A CTERM host (ctermh.c) on object 42 that lets exactly one DECnet node
 * (the maintenance machine) log in to an Amiga Shell. Every connect from
 * another node is rejected before CTERM starts; without a configured node
 * and password the service refuses every connect.
 *
 * The same login is offered over LAT (latd, service named after the
 * node) to the terminal servers named in maint-latnodes.
 *
 * Configuration (read from ENV: only, see the installer):
 *   ENV:DECnet/maint-node      the only node allowed (area.node, or a
 *                              name from ENV:DECnet/nodes)
 *   ENV:DECnet/maint-latnodes  LAT node names allowed (blank or comma
 *                              separated); none: no LAT
 *   ENV:DECnet/maint-password  password asked after connect
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DNMAINT_H
#define DNMAINT_H

#include <exec/types.h>
#include <exec/ports.h>
#include <dos/dosextens.h>
#include <libraries/decnet.h>

#include "../cterm/ctermh.h"

#define MAINT_OBJECT     42     /* CTERM */
#define MAINT_TRIES      3      /* password attempts per connect */
#define MAINT_FAILDELAY  (3 * 50)   /* ticks to wait after a wrong password */
#define MAINT_PWMAX      40

/* configuration, from ENV:DECnet/maint-* */
struct mcfg {
	UWORD node;                     /* allowed node, 0 = none configured */
	char  latnodes[120];            /* allowed LAT nodes, empty = no LAT */
	char  password[MAINT_PWMAX];    /* empty = none configured */
};

/* session states */
#define MS_BIND     0           /* waiting for BIND-ACCEPT / INITIATE */
#define MS_LOGIN    1           /* password prompt */
#define MS_SHELL    2           /* shell running */
#define MS_DONE     3           /* over: unbind, close */

/* one login session (the service serves one at a time) */
struct msession {
	struct mcfg    *cfg;
	struct DnLink  *link;
	struct ctermh   cth;
	int             verbose;

	/* LAT instead of CTERM: a raw terminal stream through latd */
	BOOL            lat;
	ULONG           latid;          /* latd's session id */
	int             lread;          /* read pending: CTH_CHAR/CTH_SECRET + 1 */
	char            lline[MAINT_PWMAX + 2];
	int             llen;
	BOOL            lcr;            /* last key was CR (CR LF: one end) */
	BOOL            lout_cr;        /* last output byte was CR */
	char            ahead[256];     /* keys before a read was asked for */
	int             aheadlen;
	int             state;
	int             tries;          /* wrong passwords so far */

	/* console side: the Shell's I/O through our handler */
	struct MsgPort *conport;        /* handler port the Shell talks to */
	struct DosPacket *readpkt;      /* ACTION_READ waiting for input */
	struct Task    *sigtask;        /* gets ^C (ACTION_CHANGE_SIGNAL) */
	BOOL            raw;            /* ACTION_SCREEN_MODE: char reads */
	BOOL            eof;            /* Ctrl-\ typed: next read ends 0 */
	char            inbuf[CTERMH_MAXLINE + 2];
	long            inlen, inpos;   /* input not yet given to the Shell */
	int             handles;        /* file handles open on conport */
	BOOL            shell_gone;     /* the Shell has ended (handles 0) */
	BOOL            shell_failed;   /* Execute() could not start it */
	BOOL            launcher;       /* the process calling Execute() runs */

	/* the terminal's cursor, followed from our output (console.c) */
	int             cx, cy;         /* cx == width: wrap pending */
	int             tstate;         /* escape sequence state */
	int             tpar[2], tnpar;

	/* line editor for cooked reads (edit.c) */
	char            ed_buf[CTERMH_MAXLINE + 1];
	char            ed_save[CTERMH_MAXLINE + 1]; /* line before history */
	int             ed_len, ed_pos;
	int             ed_startx;      /* column where the line starts */
	BOOL            ed_active;      /* a line is being typed */
	int             ed_hpos;        /* history entry shown, hist_n: none */
	int             ed_esc;         /* key escape sequence state */
	char            ed_seq[8];
	int             ed_seqlen;
};

/* dnmaint.c */
void mlog(const char *fmt, ...);
int  cfg_load(struct mcfg *cfg);
BOOL node_allowed(const struct mcfg *cfg, UWORD node);
BOOL latnode_allowed(const struct mcfg *cfg, const char *node);

/* session.c: CTERM over the link, or LAT through latd */
int  session_init(struct msession *s, struct mcfg *cfg, struct DnLink *l);
int  session_init_lat(struct msession *s, struct mcfg *cfg, ULONG latid);
int  session_run(struct msession *s);
void session_end(struct msession *s);
int  session_net_write(void *ctx, const unsigned char *buf, long n);
void session_input(void *ctx, const char *buf, long n, int term);
void session_interrupt(void *ctx, int ch);
void session_log(void *ctx, const char *msg);
/* the terminal, whichever way it came: output (LF -> CR LF), a read of
   kind CTH_CHAR or CTH_SECRET with an optional prompt, its size */
void t_write(struct msession *s, const char *buf, long n);
int  t_read(struct msession *s, int kind, const char *prompt, long plen);
BOOL t_read_pending(struct msession *s);
int  t_width(struct msession *s);
int  t_height(struct msession *s);

/* latlink.c: latd's offer of our service */
ULONG lt_offer(void);
LONG  lt_accept(ULONG offer, char *node, char *port, LONG timeout);
int   lt_answer(ULONG id, LONG reason);
void  lt_close(ULONG id);
void  lt_unoffer(ULONG offer);

/* login.c: password dialogue */
int  login_start(struct msession *s);
int  login_input(struct msession *s, const char *buf, long n, int term);
BOOL login_check(const struct mcfg *cfg, const char *buf, long n);

/* console.c: DOS console handler for the Shell */
int  con_start(struct msession *s);
void con_packet(struct msession *s, struct DosPacket *dp);
int  con_input(struct msession *s, const char *buf, long n, int term);
void con_interrupt(struct msession *s, int ch);
void con_stop(struct msession *s);
int  shell_start(struct msession *s);
void con_out(struct msession *s, const char *buf, long n);
int  con_width(struct msession *s);

/* edit.c: line editing and history for cooked reads */
void ed_init(struct msession *s);
void ed_keys(struct msession *s, const char *buf, long n);

#endif

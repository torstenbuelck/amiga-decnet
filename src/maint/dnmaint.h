/*
 * dnmaint.h - maintenance login service: shared declarations.
 *
 * A CTERM host (ctermh.c) on object 42 that lets exactly one DECnet node
 * (the maintenance machine) log in to an Amiga Shell. Every connect from
 * another node is rejected before CTERM starts; without a configured node
 * and password the service refuses every connect.
 *
 * Configuration (read from ENV: only, see the installer):
 *   ENV:DECnet/maint-node      address of the only node allowed (area.node)
 *   ENV:DECnet/maint-password  password asked after connect
 *
 * Framework only: the bodies are still empty.
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
	int             state;
	int             tries;          /* wrong passwords so far */

	/* console side: the Shell's I/O through our handler */
	struct MsgPort *conport;        /* handler port the Shell talks to */
	struct DosPacket *readpkt;      /* ACTION_READ waiting for input */
	BOOL            raw;            /* ACTION_SCREEN_MODE: char reads */
	char            inbuf[CTERMH_MAXLINE + 2];
	long            inlen, inpos;   /* input not yet given to the Shell */
	BOOL            shell_gone;     /* the Shell has ended */
};

/* dnmaint.c */
int  cfg_load(struct mcfg *cfg);
BOOL node_allowed(const struct mcfg *cfg, UWORD node);

/* session.c: CTERM over the link */
int  session_init(struct msession *s, struct mcfg *cfg, struct DnLink *l);
int  session_run(struct msession *s);
void session_end(struct msession *s);
int  session_net_write(void *ctx, const unsigned char *buf, long n);
void session_input(void *ctx, const char *buf, long n, int term);
void session_interrupt(void *ctx, int ch);
void session_log(void *ctx, const char *msg);

/* login.c: password dialogue */
int  login_start(struct msession *s);
int  login_input(struct msession *s, const char *buf, long n, int term);
BOOL login_check(const struct mcfg *cfg, const char *buf, long n);

/* console.c: DOS console handler for the Shell */
int  con_start(struct msession *s);
void con_packet(struct msession *s, struct DosPacket *dp);
int  con_input(struct msession *s, const char *buf, long n);
void con_interrupt(struct msession *s, int ch);
void con_stop(struct msession *s);
int  shell_start(struct msession *s);

#endif

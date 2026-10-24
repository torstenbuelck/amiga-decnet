/*
 * ctermh.h - portable CTERM host side (the end that runs the programs).
 *
 * A remote terminal (VMS SET HOST, our own CTERM client) connects to
 * object 42 here. The host binds the session, asks for input with
 * START-READ and sends output with WRITE; the terminal side does echo
 * and line editing. Like cterm.c it only talks through callbacks, so a
 * Linux test (dntool serve) and the Amiga shell service share it.
 *
 * Message layouts follow what OpenVMS 7.2 sends as a host (captured
 * 24.10.2026) and dnprogs ctermd.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef CTERMH_H
#define CTERMH_H

#define CTERMH_MAXLINE 255      /* longest line read */
#define CTERMH_MAXMSG  480      /* longest CTERM message we send */

/* kinds of read */
#define CTH_LINE    0           /* edited line with echo */
#define CTH_SECRET  1           /* edited line without echo (passwords) */
#define CTH_CHAR    2           /* every key at once, no echo */

struct ctermh {
	/* one complete NSP message to the terminal; 0 = ok */
	int  (*net_write)(void *ctx, const unsigned char *buf, long n);
	/* a read is done: the line without its terminator (CTH_LINE,
	   CTH_SECRET) or the keys typed (CTH_CHAR); term is the terminator
	   (CR, ^Z, ...) or 0 (CTH_CHAR, taken back by ctermh_unread) */
	void (*input)(void *ctx, const char *buf, long n, int term);
	/* ^C or ^Y typed (out of band, also outside a read) */
	void (*interrupt)(void *ctx, int ch);
	void (*log)(void *ctx, const char *msg);                    /* optional */
	void *ctx;
	int verbose;

	/* state, cleared by ctermh_init */
	int bound;                  /* BIND-ACCEPT seen */
	int ready;                  /* INITIATE exchanged: reads may start */
	int unbound;                /* the terminal side ended the session */
	int read_pending;
	int read_kind;
	int width, height;          /* from the terminal, 0 = unknown */
	char termtype[16];
	int last_cr;                /* last output byte was CR (LF -> CR LF) */
};

void ctermh_init(struct ctermh *h);
/* send BIND; call once the link is up */
int  ctermh_start(struct ctermh *h);
/* one NSP message from the terminal; <0: the session is over */
int  ctermh_net(struct ctermh *h, const unsigned char *msg, long n);
/* output; LF becomes CR LF */
int  ctermh_write(struct ctermh *h, const char *buf, long n);
/* ask for input, optionally with a prompt and text already in the line;
   only once ready is set, one read at a time */
int  ctermh_read(struct ctermh *h, int kind, const char *prompt, long plen,
                 const char *preload, long llen);
/* take back a pending read (its input arrives with term 0) */
int  ctermh_unread(struct ctermh *h);
/* end the session */
int  ctermh_unbind(struct ctermh *h);

#endif

/*
 * cterm.h - portable CTERM client (DECnet command terminal, object 42).
 *
 * CTERM is not a plain character stream: the host asks for input with a
 * START-READ (prompt, terminator set, echo, maximum length), and the
 * client edits the line locally and sends it back when a terminator is
 * typed. This module does both protocol layers (Foundation and CTERM)
 * and the local line editing. It only talks to the outside through two
 * callbacks, so dntool (Linux) and dnterm.device (Amiga) share it.
 *
 * Message layouts follow dnprogs dnlogin (C. Caulfield) and the CTERM
 * implementation memos of TOPS-20.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef CTERM_H
#define CTERM_H

#define CTERM_OBJECT 42

#define CTERM_INBUF  512            /* longest input line */
#define CTERM_AHEAD  128            /* typed while no read is active */

struct cterm {
	/* one complete NSP message to the host; 0 = ok */
	int  (*net_write)(void *ctx, const unsigned char *buf, long n);
	/* output for the terminal (VT100 byte stream) */
	void (*term_write)(void *ctx, const char *buf, long n);
	void (*log)(void *ctx, const char *msg);                     /* optional */
	void *ctx;
	int verbose;
	char revision[9];               /* our name in INITIATE, max 8 chars */
	int width, height;              /* reported terminal size */

	/* state, cleared by cterm_init */
	int bound;                      /* Foundation BIND done */
	int unbound;                    /* host sent UNBIND */
	char host[24];                  /* host system from BIND */

	unsigned char char_attr[256];   /* per-character attributes (OOB etc.) */
	unsigned char terminators[32];  /* bit set of terminator characters */

	int read_pending;               /* a START-READ is active */
	int echo, echo_terminator, allow_edit, uppercase, escape_proc;
	int discard;                    /* ^O: output off */
	unsigned max_len;
	long timeout_ms;                /* <0: none */
	long idle_ms;                   /* time since the last key in a read */
	int line_start;                 /* column after the prompt */
	int skip_lf;                    /* last write ended with our own LF */
	char last_char;

	char input[CTERM_INBUF];
	unsigned input_len, input_pos;
	char ahead[CTERM_AHEAD];
	unsigned ahead_len;
	char esc[32];
	unsigned esc_len;
	int esc_state;
	int input_count_state;          /* host wants INPUT-STATE messages */
};

void cterm_init(struct cterm *c);
/* one NSP message from the host; <0: the session is over (UNBIND) */
int  cterm_net(struct cterm *c, const unsigned char *msg, long n);
/* bytes typed on the keyboard */
void cterm_keys(struct cterm *c, const char *buf, long n);
/* time passes (for the START-READ timeout) */
void cterm_tick(struct cterm *c, long ms);

#endif

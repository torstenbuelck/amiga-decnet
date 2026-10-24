/*
 * ctermh.c - portable CTERM host side, see ctermh.h.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ctermh.h"

/* Foundation messages */
#define FND_BIND        1
#define FND_UNBIND      2
#define FND_BINDACCEPT  4
#define FND_ENTERMODE   5
#define FND_NOMODE      8
#define FND_COMMONDATA  9

/* CTERM messages */
#define CT_INITIATE      1
#define CT_START_READ    2
#define CT_READ_DATA     3
#define CT_OOB           4
#define CT_UNREAD        5
#define CT_WRITE         7
#define CT_WRITE_DONE    8
#define CT_READ_CHARS   10
#define CT_CHARS        11
#define CT_INPUT_COUNT  13
#define CT_INPUT_STATE  14

/* READ-DATA termination reasons */
#define RD_TERMINATOR    0
#define RD_VALID_ESCAPE  1
#define RD_OOB           3
#define RD_BUFFER_FULL   4
#define RD_UNREAD        6

/* START-READ flags */
#define SR_NOECHO        0x000800
#define SR_NOEDIT        0x000200   /* bits 8-10 = 2: no line editing */
#define SR_NORAISE       0x000040   /* bits 6-7 = 1: keep lower case */
#define SR_TERMSET       0x004000   /* bits 14-15 = 1: terminators follow */
#define SR_DEFTERM       0x008000   /* bits 14-15 = 2: default terminators */
#define SR_ESCAPE        0x020000   /* bits 16-17 = 2: escape recognition */

static void logf_(struct ctermh *h, const char *fmt, ...)
{
	char buf[160];
	va_list ap;

	if (!h->log || !h->verbose)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	h->log(h->ctx, buf);
}

static unsigned get16(const unsigned char *p)
{
	return p[0] | (p[1] << 8);
}

static void put16(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

/* one CTERM message inside a Foundation COMMON-DATA message */
static int send_common(struct ctermh *h, const unsigned char *msg, long n)
{
	unsigned char buf[CTERMH_MAXMSG + 8];

	if (n > CTERMH_MAXMSG)
		n = CTERMH_MAXMSG;
	buf[0] = FND_COMMONDATA;
	buf[1] = 0;
	put16(buf + 2, (unsigned)n);
	memcpy(buf + 4, msg, n);
	return h->net_write(h->ctx, buf, n + 4);
}

/* ---- output ------------------------------------------------------------ */

static int send_write(struct ctermh *h, const char *data, long n)
{
	unsigned char m[CTERMH_MAXMSG];

	m[0] = CT_WRITE;
	put16(m + 1, 0x0030);           /* plain, as ctermd */
	m[3] = m[4] = 0;                /* no prefix, no postfix */
	memcpy(m + 5, data, n);
	return send_common(h, m, n + 5);
}

int ctermh_write(struct ctermh *h, const char *buf, long n)
{
	char out[CTERMH_MAXMSG - 8];
	long i, o = 0;

	if (!h->ready)
		return -1;
	for (i = 0; i < n; i++) {
		if (o >= (long)sizeof out - 2) {
			if (send_write(h, out, o))
				return -1;
			o = 0;
		}
		if (buf[i] == '\n' && !h->last_cr)
			out[o++] = '\r';
		out[o++] = buf[i];
		h->last_cr = buf[i] == '\r';
	}
	if (o && send_write(h, out, o))
		return -1;
	return 0;
}

/* ---- input ------------------------------------------------------------- */

static int send_read(struct ctermh *h, int kind, const char *prompt, long plen,
                     const char *preload, long llen, int redisplay)
{
	unsigned char m[CTERMH_MAXMSG];
	unsigned long flags;
	long n = 17;

	if (plen < 0) plen = 0;
	if (llen < 0) llen = 0;
	if (plen > CTERMH_MAXMSG - 64)
		plen = CTERMH_MAXMSG - 64;
	if (llen > CTERMH_MAXLINE)
		llen = CTERMH_MAXLINE;
	switch (kind) {
	case CTH_CHAR:
		/* as ctermd: every character ends the read */
		flags = SR_ESCAPE | SR_TERMSET | SR_NOECHO | SR_NOEDIT | SR_NORAISE;
		break;
	case CTH_SECRET:
		flags = SR_DEFTERM | SR_NOECHO | SR_NORAISE;
		break;
	default:
		flags = SR_ESCAPE | SR_DEFTERM | SR_NORAISE;
		break;
	}
	m[0] = CT_START_READ;
	m[1] = (unsigned char)flags;
	m[2] = (unsigned char)(flags >> 8);
	m[3] = (unsigned char)(flags >> 16);
	put16(m + 4, kind == CTH_CHAR ? 0xa0 : CTERMH_MAXLINE);
	put16(m + 6, (unsigned)(plen + llen));          /* end of data */
	put16(m + 8, 0);                                /* timeout */
	put16(m + 10, (unsigned)plen);                  /* end of prompt */
	put16(m + 12, redisplay ? 0 : (unsigned)(plen + llen)); /* start of display */
	put16(m + 14, 0);                               /* low water */
	if (kind == CTH_CHAR) {
		m[16] = 32;
		memset(m + 17, 0xff, 32);
		n += 32;
	} else
		m[16] = 0;
	memcpy(m + n, prompt, plen);
	n += plen;
	memcpy(m + n, preload, llen);
	n += llen;
	h->read_pending = 1;
	h->read_kind = kind;
	return send_common(h, m, n);
}

int ctermh_read(struct ctermh *h, int kind, const char *prompt, long plen,
                const char *preload, long llen)
{
	if (!h->ready || h->read_pending)
		return -1;
	return send_read(h, kind, prompt, plen, preload, llen, 1);
}

int ctermh_unread(struct ctermh *h)
{
	static const unsigned char m[2] = { CT_UNREAD, 0 };

	if (!h->read_pending)
		return 0;
	return send_common(h, m, 2);
}

static void do_read_data(struct ctermh *h, const unsigned char *b, long n)
{
	const char *data;
	unsigned reason, termpos;
	long len;
	int term = 0;

	if (n < 8 || !h->read_pending)
		return;
	reason = b[1];
	termpos = get16(b + 6);
	data = (const char *)b + 8;
	len = n - 8;
	h->read_pending = 0;
	logf_(h, "read-data reason %u termpos %u len %ld", reason, termpos, len);

	if (termpos > len)
		termpos = (unsigned)len;
	if (reason == RD_OOB) {
		/* ^C or ^Y (already passed on as interrupt) ended the read;
		   like the Amiga console, go on with the line typed so far */
		send_read(h, h->read_kind, NULL, 0, data, h->read_kind == CTH_CHAR ? 0 : len, 0);
		return;
	}
	if (h->read_kind == CTH_CHAR) {
		h->input(h->ctx, data, len, 0);
		return;
	}
	if (reason == RD_VALID_ESCAPE) {
		/* a cursor key: no line history here, go on with the same line
		   and do not show it again */
		send_read(h, h->read_kind, NULL, 0, data, termpos, 0);
		return;
	}
	if (reason == RD_TERMINATOR && termpos < len)
		term = (unsigned char)data[termpos];
	/* the terminal does not echo the terminator: end the line here, as
	   the Amiga console does */
	if (reason != RD_UNREAD) {
		h->last_cr = 0;
		ctermh_write(h, "\n", 1);
	}
	h->input(h->ctx, data, termpos, term);
}

/* CHARACTERISTICS from the terminal: answer to our READ-CHARACTERISTICS */
static void do_chars(struct ctermh *h, const unsigned char *b, long n)
{
	long p = 2;

	while (p + 1 < n) {
		unsigned sel = get16(b + p), id = sel & 0xff;
		p += 2;
		switch (sel & 0x300) {
		case 0x000:
			p += (id == 1 || id == 2 || id == 3 || id == 5) ? 2 : (id == 9 || id == 10) ? 0 : 1;
			break;
		case 0x100:
			if (id == 3) {
				unsigned l = p < n ? b[p] : 0;
				if (p + 1 + (long)l <= n) {
					if (l >= sizeof h->termtype)
						l = sizeof h->termtype - 1;
					memcpy(h->termtype, b + p + 1, l);
					h->termtype[l] = 0;
				}
				p += (p < n ? b[p] : 0) + 1;
			} else if (id == 9 || id == 0xa) {
				if (p + 1 < n) {
					if (id == 9)
						h->width = get16(b + p);
					else
						h->height = get16(b + p);
				}
				p += 2;
			} else if (id == 2 || (id >= 0xb && id <= 0x11))
				p += 2;
			else
				p += 1;
			break;
		case 0x200:
			p += id == 2 ? 3 : id == 8 ? 2 : 1;
			break;
		default:
			return;
		}
	}
	logf_(h, "terminal %s %dx%d", h->termtype, h->width, h->height);
}

static void after_initiate(struct ctermh *h)
{
	/* ^C and ^Y out of band with echo; unlike VMS (0x29) without the
	   discard bit, which would swallow the program's output after it */
	static const unsigned char chars[] = {
		CT_CHARS, 0,
		0x02, 0x02, 0x03, 0x3b, 0x21,
		0x02, 0x02, 0x19, 0x3b, 0x21,
	};
	/* terminal type and size */
	static const unsigned char query[] = {
		CT_READ_CHARS, 0,
		0x03, 0x01, 0x09, 0x01, 0x0a, 0x01,
	};

	send_common(h, chars, sizeof chars);
	send_common(h, query, sizeof query);
	h->ready = 1;
}

static void do_cterm(struct ctermh *h, const unsigned char *b, long n)
{
	if (n < 1)
		return;
	logf_(h, "cterm msg %d len %ld", b[0], n);
	switch (b[0]) {
	case CT_INITIATE:
		if (!h->ready)
			after_initiate(h);
		break;
	case CT_READ_DATA:
		do_read_data(h, b, n);
		break;
	case CT_OOB:
		if (n > 2 && h->interrupt)
			h->interrupt(h->ctx, b[2]);
		break;
	case CT_CHARS:
		do_chars(h, b, n);
		break;
	case CT_WRITE_DONE:
	case CT_INPUT_COUNT:
	case CT_INPUT_STATE:
		break;
	default:
		logf_(h, "cterm: ignored message %d", b[0]);
		break;
	}
}

void ctermh_init(struct ctermh *h)
{
	/* keep the callbacks and settings, clear the session state */
	memset(&h->bound, 0, sizeof *h - ((char *)&h->bound - (char *)h));
}

int ctermh_start(struct ctermh *h)
{
	/* version 2.4.0, OS 7 (VMS; there is no number for AmigaOS) */
	static const unsigned char bind[] = { FND_BIND, 2, 4, 0, 7, 0, 0x10, 0 };

	return h->net_write(h->ctx, bind, sizeof bind);
}

int ctermh_net(struct ctermh *h, const unsigned char *b, long n)
{
	long p;

	if (n < 1)
		return 0;
	switch (b[0]) {
	case FND_BINDACCEPT: {
		static const unsigned char init[] = {
			CT_INITIATE, 0,
			1, 4, 0,                                /* version 1.4.0 */
			'A', 'm', 'i', 'g', 'a', ' ', ' ', ' ',
			1, 2, 0x00, 0x02,                       /* max message 512 */
			2, 2, CTERMH_MAXLINE, 0,                /* max input buffer */
			3, 4, 0xfe, 0x7f, 0, 0                  /* messages 1..14 */
		};
		logf_(h, "bind accepted");
		h->bound = 1;
		return send_common(h, init, sizeof init) ? -1 : 0;
	}
	case FND_UNBIND:
		logf_(h, "unbind, reason %d", n > 1 ? b[1] : 0);
		h->unbound = 1;
		return -1;
	case FND_ENTERMODE: {
		unsigned char nomode = FND_NOMODE;
		return h->net_write(h->ctx, &nomode, 1) ? -1 : 0;
	}
	case FND_COMMONDATA:
		p = 2;
		while (p + 2 <= n) {
			unsigned len = get16(b + p);
			p += 2;
			if (p + (long)len > n)
				len = (unsigned)(n - p);
			do_cterm(h, b + p, len);
			p += len;
		}
		return 0;
	default:
		logf_(h, "foundation: ignored message %d", b[0]);
		return 0;
	}
}

int ctermh_unbind(struct ctermh *h)
{
	static const unsigned char m[3] = { FND_UNBIND, 3, 0 };

	h->ready = 0;
	return h->net_write(h->ctx, m, sizeof m);
}

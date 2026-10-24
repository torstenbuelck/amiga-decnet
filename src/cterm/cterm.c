/*
 * cterm.c - portable CTERM client, see cterm.h.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cterm.h"

/* Foundation messages */
#define FND_BIND        1
#define FND_UNBIND      2
#define FND_BINDACCEPT  4
#define FND_ENTERMODE   5
#define FND_NOMODE      8
#define FND_COMMONDATA  9
#define FND_MODEDATA   10

/* CTERM messages */
#define CT_INITIATE      1
#define CT_START_READ    2
#define CT_READ_DATA     3
#define CT_OOB           4
#define CT_UNREAD        5
#define CT_CLEAR_INPUT   6
#define CT_WRITE         7
#define CT_WRITE_DONE    8
#define CT_DISCARD_STATE 9
#define CT_READ_CHARS   10
#define CT_CHARS        11
#define CT_CHECK_INPUT  12
#define CT_INPUT_COUNT  13
#define CT_INPUT_STATE  14

/* READ-DATA termination reasons */
#define RD_TERMINATOR    0
#define RD_VALID_ESCAPE  1
#define RD_BUFFER_FULL   4
#define RD_TIMEOUT       5
#define RD_UNREAD        6

/* character attribute bits (CHARACTERISTICS 0x202) */
#define CA_OOB_MASK      0x03   /* 1 clear, 2 deferred, 3 immediate */
#define CA_OOB_DISCARD   0x04
#define CA_ECHO_MASK     0x30   /* echo the OOB character */

#define ESC   0x1b
#define DEL   0x7f
#define CTRL(c) ((c) & 0x1f)

static const char *const os_names[] = {
	"RT-11", "RSTS/E", "RSX-11S", "RSX-11M", "RSX-11D", "IAS", "VMS",
	"TOPS-20", "TOPS-10", "OS8", "RTS-8", "RSX-11M+"
};

static void logf_(struct cterm *c, const char *fmt, ...)
{
	char buf[160];
	va_list ap;

	if (!c->log || !c->verbose)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	c->log(c->ctx, buf);
}

/* ---- output to the host ---------------------------------------------- */

static int send_raw(struct cterm *c, const unsigned char *buf, long n)
{
	return c->net_write(c->ctx, buf, n);
}

/* one CTERM message inside a Foundation COMMON-DATA message */
static int send_common(struct cterm *c, const unsigned char *msg, long n)
{
	unsigned char buf[CTERM_INBUF + 32];

	if (n > (long)sizeof buf - 4)
		n = sizeof buf - 4;
	buf[0] = FND_COMMONDATA;
	buf[1] = 0;
	buf[2] = (unsigned char)n;
	buf[3] = (unsigned char)(n >> 8);
	memcpy(buf + 4, msg, n);
	return send_raw(c, buf, n + 4);
}

/* ---- output to the terminal ------------------------------------------ */

/* escape sequence parser (appendix B of the CTERM specification): 1 while
   inside a sequence, 0 when it ended; the cases fall through on purpose */
static int parse_escape(char ch, int *state)
{
	switch (*state) {
	case 0:
		if (ch == '?' || ch == ';') { *state = 10; return 1; }
		if (ch == 'O') { *state = 20; return 1; }
		if (ch == 'Y') { *state = 30; return 1; }
		if (ch == '[') { *state = 15; return 1; }
		/* fall through */
	case 10:
		if (ch >= 32 && ch <= 47) { *state = 10; return 1; }
		if (ch >= 48 && ch <= 126) { *state = 0; return 0; }
		/* fall through */
	case 15:
		if (ch >= 48 && ch <= 63) { *state = 15; return 1; }
		/* fall through */
	case 20:
		if (ch >= 32 && ch <= 47) { *state = 20; return 1; }
		if (ch >= 64 && ch <= 126) { *state = 0; return 0; }
		/* fall through */
	case 30:
		if (ch >= 32 && ch <= 126) { *state = 40; return 1; }
		/* fall through */
	case 40:
		if (ch >= 32 && ch <= 126) { *state = 0; return 0; }
		break;
	}
	*state = 0;
	return 0;
}

static void term_out(struct cterm *c, const char *buf, long n)
{
	int in_esc = 0, st = 0;
	long i, j;

	if (c->discard || n <= 0)
		return;
	/* VMS pads with NULs (fill characters); a terminal shows nothing
	   for them, but some emulators print a symbol */
	for (i = 0; i < n; i = j + 1) {
		for (j = i; j < n && buf[j]; j++)
			;
		if (j > i)
			c->term_write(c->ctx, buf + i, j - i);
	}
	/* remember the last printing character, skipping escape sequences */
	for (i = 0; i < n; i++) {
		if (buf[i] == ESC)
			in_esc = 1;
		else if (in_esc) {
			if (!parse_escape(buf[i], &st))
				in_esc = 0;
		} else if (buf[i])
			c->last_char = buf[i];
	}
}

static void term_str(struct cterm *c, const char *s)
{
	term_out(c, s, (long)strlen(s));
}

static void move_to(struct cterm *c, int col)
{
	char buf[16];

	if (col > 0)
		snprintf(buf, sizeof buf, "\r\033[%dC", col);
	else
		strcpy(buf, "\r");
	term_str(c, buf);
}

static void redraw(struct cterm *c)
{
	if (!c->echo)
		return;
	move_to(c, c->line_start);
	term_str(c, "\033[K");
	term_out(c, c->input, c->input_len);
	move_to(c, c->line_start + (int)c->input_pos);
}

/* ---- input ------------------------------------------------------------ */

static void send_read_data(struct cterm *c, int reason, const char *data, unsigned len,
                           unsigned termpos)
{
	unsigned char buf[CTERM_INBUF + 16];

	if (len > CTERM_INBUF)
		len = CTERM_INBUF;
	buf[0] = CT_READ_DATA;
	buf[1] = (unsigned char)reason;
	buf[2] = buf[3] = 0;            /* low water */
	buf[4] = 0;                     /* vertical position */
	buf[5] = 0;                     /* horizontal position */
	buf[6] = (unsigned char)termpos;
	buf[7] = (unsigned char)(termpos >> 8);
	memcpy(buf + 8, data, len);
	c->read_pending = 0;
	send_common(c, buf, 8 + len);
}

static void finish_read(struct cterm *c, int reason)
{
	unsigned termpos = c->input_len;

	/* the terminator is the last character, except after a timeout */
	if (reason != RD_TIMEOUT && reason != RD_UNREAD && termpos > 0)
		termpos--;
	send_read_data(c, reason, c->input, c->input_len, termpos);
	c->input_len = c->input_pos = 0;
	c->echo = 1;
	c->escape_proc = 0;
}

static void input_state(struct cterm *c, int typed)
{
	unsigned char m[2];

	if (!c->input_count_state)
		return;
	m[0] = CT_INPUT_STATE;
	m[1] = (unsigned char)typed;
	send_common(c, m, 2);
}

static void send_oob(struct cterm *c, char ch, int discard)
{
	unsigned char m[3];

	m[0] = CT_OOB;
	m[1] = (unsigned char)discard;
	m[2] = (unsigned char)ch;
	send_common(c, m, 3);
	if (c->char_attr[(unsigned char)ch] & CA_ECHO_MASK) {
		if (ch == CTRL('C'))
			term_str(c, "\033[7m Cancel \033[0m\r\n");
		else if (ch == CTRL('Y'))
			term_str(c, "\033[7m Interrupt \033[0m\r\n");
		else if (ch == CTRL('O')) {
			c->discard = !c->discard;
			if (!c->discard)
				term_str(c, "\r\n\033[7m Output On \033[0m\r\n");
		}
	}
}

static int is_terminator(struct cterm *c, unsigned char ch)
{
	return (c->terminators[ch >> 3] >> (ch & 7)) & 1;
}

static void default_terminators(struct cterm *c)
{
	/* all control characters except ^A ^E ^H ^I ^R ^U ^W ^X (editing) */
	memset(c->terminators, 0, sizeof c->terminators);
	c->terminators[0] = 0xdd;
	c->terminators[1] = 0xfc;
	c->terminators[2] = 0x5b;
	c->terminators[3] = 0xfe;
}

static void key(struct cterm *c, char ch)
{
	unsigned char attr;

	if (ch == '\n')
		ch = '\r';
	attr = c->char_attr[(unsigned char)ch];
	if ((attr & CA_OOB_MASK) == 0 && ch == CTRL('C'))
		ch = CTRL('Y');         /* like dnlogin: ^C acts as ^Y at DCL */
	attr = c->char_attr[(unsigned char)ch];
	if (attr & CA_OOB_MASK) {
		int d = (attr & CA_OOB_DISCARD) != 0;
		send_oob(c, ch, d);
		if (d)
			c->input_len = c->input_pos = 0;
		return;
	}
	if (!c->read_pending) {
		if (c->ahead_len == 0)
			input_state(c, 1);
		if (c->ahead_len < sizeof c->ahead)
			c->ahead[c->ahead_len++] = ch;
		return;
	}
	c->idle_ms = 0;

	/* escape sequences (cursor keys) */
	if (ch == ESC && c->esc_len == 0 && c->escape_proc) {
		c->esc[c->esc_len++] = ch;
		c->esc_state = 0;
		return;
	}
	if (c->esc_len) {
		if (c->esc_len < sizeof c->esc)
			c->esc[c->esc_len++] = ch;
		if (!parse_escape(ch, &c->esc_state)) {
			int done = 0;
			if (c->allow_edit && c->esc_len == 3 && c->esc[1] == '[') {
				if (c->esc[2] == 'C') {
					if (c->input_pos < c->input_len) {
						c->input_pos++;
						if (c->echo)
							term_out(c, c->esc, 3);
					}
					done = 1;
				} else if (c->esc[2] == 'D') {
					if (c->input_pos > 0) {
						c->input_pos--;
						if (c->echo)
							term_out(c, c->esc, 3);
					}
					done = 1;
				}
			}
			if (!done && c->input_len + c->esc_len <= CTERM_INBUF) {
				/* hand the sequence to the host (e.g. DCL recall) */
				unsigned termpos = c->input_len;
				memcpy(c->input + c->input_len, c->esc, c->esc_len);
				c->input_len += c->esc_len;
				send_read_data(c, RD_VALID_ESCAPE, c->input, c->input_len, termpos);
				c->input_len = c->input_pos = 0;
				c->echo = 1;
			}
			c->esc_len = 0;
		}
		return;
	}

	if (is_terminator(c, (unsigned char)ch)) {
		if (c->echo && c->echo_terminator)
			term_out(c, &ch, 1);
		if (c->input_len < CTERM_INBUF)
			c->input[c->input_len++] = ch;
		finish_read(c, RD_TERMINATOR);
		return;
	}

	if ((unsigned char)ch < ' ' || ch == DEL) {
		if (!c->allow_edit)
			return;
		switch (ch) {
		case CTRL('U'):
		case CTRL('X'):
			move_to(c, c->line_start);
			if (c->echo)
				term_str(c, "\033[K");
			c->input_len = c->input_pos = 0;
			break;
		case CTRL('H'):
			if (c->echo)
				move_to(c, c->line_start);
			c->input_pos = 0;
			break;
		case CTRL('E'):
			if (c->echo)
				move_to(c, c->line_start + (int)c->input_len);
			c->input_pos = c->input_len;
			break;
		case CTRL('R'):
		case CTRL('W'):
			redraw(c);
			break;
		case CTRL('M'):
			if (c->input_len < CTERM_INBUF)
				c->input[c->input_len++] = ch;
			finish_read(c, RD_TERMINATOR);
			break;
		case DEL:
			if (c->input_pos > 0) {
				c->input_pos--;
				memmove(c->input + c->input_pos, c->input + c->input_pos + 1,
				        c->input_len - c->input_pos - 1);
				c->input_len--;
				if (c->input_pos == c->input_len) {
					if (c->echo)
						term_str(c, "\b \b");
				} else
					redraw(c);
			}
			break;
		}
		return;
	}

	if (c->uppercase && ch >= 'a' && ch <= 'z')
		ch -= 'a' - 'A';
	if (c->input_pos == c->input_len) {
		if (c->input_len >= CTERM_INBUF)
			return;
		c->input[c->input_len++] = ch;
		c->input_pos++;
		if (c->echo)
			term_out(c, &ch, 1);
	} else {
		c->input[c->input_pos++] = ch;     /* overstrike */
		if (c->echo)
			term_out(c, &ch, 1);
	}
	if (c->input_len >= c->max_len)
		finish_read(c, RD_BUFFER_FULL);
}

void cterm_keys(struct cterm *c, const char *buf, long n)
{
	long i;

	for (i = 0; i < n; i++)
		key(c, buf[i]);
}

void cterm_tick(struct cterm *c, long ms)
{
	if (!c->read_pending || c->timeout_ms < 0)
		return;
	c->idle_ms += ms;
	if (c->idle_ms >= c->timeout_ms)
		finish_read(c, RD_TIMEOUT);
}

/* ---- messages from the host ------------------------------------------ */

static unsigned get16(const unsigned char *p)
{
	return p[0] | (p[1] << 8);
}

static void do_initiate(struct cterm *c)
{
	unsigned char m[32];
	unsigned n = 0, maxin = CTERM_INBUF - 12;

	m[n++] = CT_INITIATE;
	m[n++] = 0;                     /* flags */
	m[n++] = 1; m[n++] = 4; m[n++] = 0;     /* version 1.4.0 */
	memset(m + n, ' ', 8);
	memcpy(m + n, c->revision, strlen(c->revision) > 8 ? 8 : strlen(c->revision));
	n += 8;
	m[n++] = 1; m[n++] = 2;         /* maximum message size */
	m[n++] = 0x00; m[n++] = 0x02;
	m[n++] = 2; m[n++] = 2;         /* maximum input buffer */
	m[n++] = (unsigned char)maxin; m[n++] = (unsigned char)(maxin >> 8);
	m[n++] = 3; m[n++] = 4;         /* supported messages 1..14 */
	m[n++] = 0xfe; m[n++] = 0x7f; m[n++] = 0; m[n++] = 0;
	send_common(c, m, n);
}

static void do_start_read(struct cterm *c, const unsigned char *b, long n)
{
	unsigned long flags;
	unsigned maxlen, eodata, timeout, eoprompt, termlen, p;
	const char *data;
	long datalen;
	unsigned i;

	if (n < 17)
		return;
	flags = b[1] | (b[2] << 8) | ((unsigned long)b[3] << 16);
	maxlen = get16(b + 4);
	eodata = get16(b + 6);
	timeout = get16(b + 8);
	eoprompt = get16(b + 10);
	/* b+12 start of display, b+14 low water: not used */
	termlen = b[16];
	p = 17;
	if (p + termlen > (unsigned long)n)
		return;
	data = (const char *)b + p + termlen;
	datalen = n - p - termlen;
	(void)eodata;
	if (eoprompt > datalen)
		eoprompt = datalen;
	logf_(c, "start-read flags %06lx max %u prompt %u", flags, maxlen, eoprompt);

	if (flags & 0x4)
		c->ahead_len = 0;               /* clear type-ahead */
	c->echo = !(flags & 0x800);
	/* format: start a new line if the prompt does not do so itself */
	if ((flags & 0x8) && eoprompt && data[0] != '\n' && c->last_char == '\r')
		term_str(c, "\n");
	switch ((flags >> 14) & 3) {
	case 1:
		memset(c->terminators, 0, sizeof c->terminators);
		memcpy(c->terminators, b + p, termlen > 32 ? 32 : termlen);
		break;
	case 2:
		default_terminators(c);
		break;
	}
	switch ((flags >> 16) & 3) {
	case 1: c->escape_proc = 0; break;
	case 2: c->escape_proc = 1; break;
	}
	c->allow_edit = ((flags >> 8) & 7) != 2;
	c->uppercase = ((flags >> 6) & 3) == 2;
	c->max_len = maxlen && maxlen < CTERM_INBUF ? maxlen : CTERM_INBUF;
	c->timeout_ms = (flags & 0x2000) ? (long)timeout * 1000 : -1;
	c->idle_ms = 0;
	c->echo_terminator = (flags >> 12) & 1;

	/* prompt, then data already in the line (e.g. a recalled command) */
	if (c->skip_lf && eoprompt && data[0] == '\n') {
		term_out(c, data + 1, eoprompt - 1);
	} else if (eoprompt)
		term_out(c, data, eoprompt);
	c->skip_lf = 0;
	c->line_start = 0;
	for (i = 0; i < eoprompt; i++) {
		if (data[i] == '\r' || data[i] == '\n')
			c->line_start = 0;
		else if ((unsigned char)data[i] >= ' ')
			c->line_start++;
	}
	c->input_len = (unsigned)(datalen - eoprompt);
	if (c->input_len > CTERM_INBUF)
		c->input_len = CTERM_INBUF;
	memcpy(c->input, data + eoprompt, c->input_len);
	c->input_pos = c->input_len;
	if (c->echo)
		term_out(c, c->input, c->input_len);
	c->read_pending = 1;

	/* type-ahead goes into the new read */
	if (c->ahead_len) {
		char tmp[CTERM_AHEAD];
		unsigned k = c->ahead_len;
		memcpy(tmp, c->ahead, k);
		c->ahead_len = 0;
		for (i = 0; i < k && c->read_pending; i++)
			key(c, tmp[i]);
		if (i < k) {            /* the read ended: keep the rest */
			memcpy(c->ahead, tmp + i, k - i);
			c->ahead_len = k - i;
		}
	}
}

/* prefix/postfix of a WRITE: 1 = newlines, 2 = one character */
static void write_fix(struct cterm *c, int mode, unsigned char v)
{
	int i;

	if (mode == 1 && v) {
		term_str(c, "\r");
		for (i = 0; i < v - c->skip_lf; i++)
			term_str(c, "\n");
		c->skip_lf = 0;
	} else if (mode == 2) {
		char ch = (char)v;
		term_out(c, &ch, 1);
	}
}

static void do_write(struct cterm *c, const unsigned char *b, long n)
{
	unsigned flags;
	const char *data;
	long len;

	if (n < 5)
		return;
	flags = get16(b + 1);
	data = (const char *)b + 5;
	len = n - 5;
	c->discard = !(flags >> 3);     /* as dnlogin: only bits 0-2 keep it */
	write_fix(c, (flags >> 6) & 3, b[3]);
	if (c->skip_lf && len > 0 && data[0] == '\n') {
		data++;
		len--;
		c->skip_lf = 0;
	}
	term_out(c, data, len);
	if (flags & 0x4) {              /* L: end with a new line */
		if (!c->skip_lf) {
			term_str(c, "\n");
			c->skip_lf = 1;
		} else
			c->skip_lf = 0;
	}
	write_fix(c, (flags >> 8) & 3, b[4]);
	if (flags & 0x400) {            /* S: send WRITE-COMPLETE */
		unsigned char m[6] = { CT_WRITE_DONE, 0, 0, 0, 0, 0 };
		send_common(c, m, 6);
	}
	/* a pending read: show the prompt line again below the output */
	if (c->read_pending && len > 0 && c->echo && c->input_len)
		redraw(c);
}

static void put16(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

/* READ-CHARACTERISTICS: answer with the values we keep */
static void do_read_chars(struct cterm *c, const unsigned char *b, long n)
{
	unsigned char out[256];
	long p = 2, o = 0;

	out[o++] = CT_CHARS;
	out[o++] = 0;
	while (p + 1 < n && o < (long)sizeof out - 16) {
		unsigned sel = get16(b + p), id = sel & 0xff;
		p += 2;
		out[o++] = (unsigned char)id;
		out[o++] = (unsigned char)(sel >> 8);
		switch (sel & 0x300) {
		case 0x000:             /* physical */
			switch (id) {
			case 1: case 2: put16(out + o, 9600); o += 2; break;  /* speeds */
			case 3: put16(out + o, 8); o += 2; break;             /* char size */
			case 5: put16(out + o, 1); o += 2; break;             /* parity type */
			case 4: case 6: case 7: case 8: case 0xb: case 0xc:
				out[o++] = id == 0xb;   /* only "eight bit" is true */
				break;
			default: break;
			}
			break;
		case 0x100:             /* logical */
			switch (id) {
			case 2: put16(out + o, 3); o += 2; break;     /* attributes */
			case 3: {                               /* terminal type */
				unsigned l = (unsigned)strlen(c->termtype);
				out[o++] = (unsigned char)l;
				memcpy(out + o, c->termtype, l);
				o += l;
				break;
			}
			case 9: put16(out + o, c->width); o += 2; break;
			case 0xa: put16(out + o, c->height); o += 2; break;
			case 0xb: case 0xc: case 0xd: put16(out + o, 0); o += 2; break;
			case 0xe: case 0xf: case 0x10: case 0x11: put16(out + o, 1); o += 2; break;
			default: out[o++] = id >= 4 && id <= 8; break;
			}
			break;
		case 0x200:             /* handler */
			switch (id) {
			case 2:
				if (p < n) {
					unsigned char ch = b[p++];
					out[o++] = ch;
					out[o++] = 0xff;
					out[o++] = c->char_attr[ch];
				}
				break;
			case 8: put16(out + o, c->input_count_state); o += 2; break;
			case 5: case 6: case 7: out[o++] = 1; break;
			default: out[o++] = 0; break;
			}
			break;
		}
	}
	send_common(c, out, o);
}

/* CHARACTERISTICS: the host sets values; only handler ones matter here */
static void do_chars(struct cterm *c, const unsigned char *b, long n)
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
			if (id == 3)
				p += (p < n ? b[p] : 0) + 1;
			else if (id == 2 || (id >= 9 && id <= 0x11))
				p += 2;
			else
				p += 1;
			break;
		case 0x200:
			switch (id) {
			case 2:
				if (p + 2 < n) {
					unsigned char ch = b[p], mask = b[p + 1], val = b[p + 2];
					c->char_attr[ch] = (c->char_attr[ch] & ~mask) | (val & mask);
				}
				p += 3;
				break;
			case 6:
				if (p < n)
					c->escape_proc = b[p] != 0;
				p += 1;
				break;
			case 8:
				if (p + 1 < n)
					c->input_count_state = get16(b + p);
				p += 2;
				break;
			default:
				p += 1;
				break;
			}
			break;
		default:
			return;         /* unknown class: cannot know its length */
		}
	}
}

static void do_cterm(struct cterm *c, const unsigned char *b, long n)
{
	if (n < 1)
		return;
	logf_(c, "cterm msg %d len %ld", b[0], n);
	if (c->verbose > 1) {
		char hex[160];
		long i, o = 0;
		for (i = 0; i < n && o < (long)sizeof hex - 4; i++)
			o += snprintf(hex + o, sizeof hex - o, "%02x ", b[i]);
		logf_(c, "  %s", hex);
	}
	switch (b[0]) {
	case CT_INITIATE:
		do_initiate(c);
		break;
	case CT_START_READ:
		do_start_read(c, b, n);
		break;
	case CT_UNREAD:
		if (c->read_pending && (n < 2 || !(b[1] & 1) || c->input_len == 0))
			finish_read(c, RD_UNREAD);
		break;
	case CT_CLEAR_INPUT:
		c->ahead_len = 0;
		break;
	case CT_WRITE:
		do_write(c, b, n);
		break;
	case CT_DISCARD_STATE:
		if (n > 1)
			c->discard = !(b[1] & 1);
		break;
	case CT_READ_CHARS:
		do_read_chars(c, b, n);
		break;
	case CT_CHARS:
		do_chars(c, b, n);
		break;
	case CT_CHECK_INPUT: {
		unsigned char m[4];
		unsigned cnt = c->input_len + c->ahead_len;
		m[0] = CT_INPUT_COUNT;
		m[1] = 0;
		put16(m + 2, cnt);
		send_common(c, m, 4);
		break;
	}
	default:
		logf_(c, "cterm: ignored message %d", b[0]);
		break;
	}
}

void cterm_init(struct cterm *c)
{
	/* keep the callbacks and settings, clear the session state */
	memset(&c->bound, 0, sizeof *c - ((char *)&c->bound - (char *)c));
	if (!c->revision[0])
		strcpy(c->revision, "DECnet");
	if (!c->termtype[0])
		strcpy(c->termtype, "VT200");
	if (!c->width)
		c->width = 80;
	if (!c->height)
		c->height = 24;
	c->echo = 1;
	c->allow_edit = 1;
	c->max_len = CTERM_INBUF;
	c->timeout_ms = -1;
	default_terminators(c);
}

int cterm_net(struct cterm *c, const unsigned char *b, long n)
{
	long p;

	if (n < 1)
		return 0;
	switch (b[0]) {
	case FND_BIND: {
		static const unsigned char accept[] = {
			FND_BINDACCEPT,
			2, 4, 0,                /* version 2.4.0 */
			7, 0,                   /* OS: VMS, as dnlogin says */
			0, 0, 0, 0, 0, 0, 0, 0, /* no revision */
			0, 0,                   /* no logical terminal id */
			0                       /* no options */
		};
		unsigned os = n > 4 ? b[4] : 0;
		snprintf(c->host, sizeof c->host, "%s",
		         os >= 1 && os <= sizeof os_names / sizeof os_names[0] ? os_names[os - 1] : "?");
		logf_(c, "bind from a %s host", c->host);
		c->bound = 1;
		return send_raw(c, accept, sizeof accept) ? -1 : 0;
	}
	case FND_UNBIND:
		logf_(c, "unbind, reason %d", n > 1 ? b[1] : 0);
		c->unbound = 1;
		return -1;
	case FND_ENTERMODE: {
		unsigned char nomode = FND_NOMODE;
		return send_raw(c, &nomode, 1) ? -1 : 0;
	}
	case FND_COMMONDATA:
		p = 2;
		while (p + 2 <= n) {
			unsigned len = get16(b + p);
			p += 2;
			if (p + (long)len > n)
				len = (unsigned)(n - p);
			do_cterm(c, b + p, len);
			p += len;
		}
		return 0;
	case FND_MODEDATA:
	case 3:
		return 0;
	default:
		logf_(c, "foundation: ignored message %d", b[0]);
		return 0;
	}
}

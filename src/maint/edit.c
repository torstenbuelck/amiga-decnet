/*
 * edit.c - line editing and history for the Shell's cooked reads.
 *
 * On the Amiga the console handler edits the line and keeps the
 * history (cursor up/down), not the Shell. Our handler stands in for
 * it: keys come one at a time from CTERM character reads without echo,
 * so the terminal side's own line editing (VMS: one recalled line,
 * overstrike, Delete ends the read) plays no part. The line is drawn
 * with plain VT100 controls and may wrap over several rows.
 *
 * Keys: cursor left/right, Home/End (also ^A/^E), Backspace (BS and
 * DEL: a VT's Backspace key sends DEL), Delete (ESC [3~), cursor
 * up/down through the history, ^X/^U erase the line, ^K erases to its
 * end, ^\ ends the input (EOF), as on CON:.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>

#include <string.h>

#include "dnmaint.h"

#define HIST_LINES 20
#define ESC 0x1b
#define CSI 0x9b

/* one session at a time: the history is static, cleared per session */
static char hist[HIST_LINES][CTERMH_MAXLINE + 1];
static int hist_n;

void ed_init(struct msession *s)
{
	hist_n = 0;
	s->ed_len = s->ed_pos = 0;
	s->ed_active = FALSE;
	s->ed_hpos = 0;
	s->ed_esc = 0;
	s->ed_seqlen = 0;
}

static void out(struct msession *s, const char *buf, long n)
{
	con_out(s, buf, n);
}

/* ESC [ n c */
static void out_csi(struct msession *s, int n, char c)
{
	char b[8];
	int i = 0;

	b[i++] = ESC;
	b[i++] = '[';
	if (n >= 100)
		b[i++] = (char)('0' + n / 100 % 10);
	if (n >= 10)
		b[i++] = (char)('0' + n / 10 % 10);
	b[i++] = (char)('0' + n % 10);
	b[i++] = c;
	out(s, b, i);
}

/* move the cursor from line position a to b */
static void move(struct msession *s, int a, int b)
{
	int w = con_width(s);
	int ra = (s->ed_startx + a) / w, rb = (s->ed_startx + b) / w;
	int cb = (s->ed_startx + b) % w;

	if (a == b)
		return;
	if (ra == rb && b < a && a - b <= 4) {
		out(s, "\b\b\b\b", a - b);
		return;
	}
	if (rb < ra)
		out_csi(s, ra - rb, 'A');
	else if (rb > ra)
		out_csi(s, rb - ra, 'B');
	out(s, "\r", 1);
	if (cb > 0)
		out_csi(s, cb, 'C');
}

/* draw the line from position i to its end, erase what was behind it,
   put the cursor at position to */
static void redraw(struct msession *s, int i, int to)
{
	move(s, s->ed_pos, i);
	out(s, s->ed_buf + i, s->ed_len - i);
	/* text up to the last column: the VT cursor waits there; make it
	   wrap now, so that it stands where we count it */
	if (s->ed_len > i && (s->ed_startx + s->ed_len) % con_width(s) == 0)
		out(s, " \r", 2);
	out(s, "\033[J", 3);
	s->ed_pos = s->ed_len;
	move(s, s->ed_pos, to);
	s->ed_pos = to;
}

/* replace the whole line (history) */
static void set_line(struct msession *s, const char *text)
{
	s->ed_len = (int)strlen(text);
	memcpy(s->ed_buf, text, s->ed_len);
	redraw(s, 0, s->ed_len);
}

static void insert(struct msession *s, char c)
{
	if (s->ed_len >= CTERMH_MAXLINE)
		return;
	memmove(s->ed_buf + s->ed_pos + 1, s->ed_buf + s->ed_pos, s->ed_len - s->ed_pos);
	s->ed_buf[s->ed_pos] = c;
	s->ed_len++;
	if (s->ed_pos == s->ed_len - 1 && (s->ed_startx + s->ed_len) % con_width(s) != 0) {
		out(s, &c, 1);                  /* the usual case: typing at the end */
		s->ed_pos++;
		return;
	}
	redraw(s, s->ed_pos, s->ed_pos + 1);
}

/* remove the character at position i, cursor to i */
static void delete_at(struct msession *s, int i)
{
	if (i < 0 || i >= s->ed_len)
		return;
	memmove(s->ed_buf + i, s->ed_buf + i + 1, s->ed_len - i - 1);
	s->ed_len--;
	redraw(s, i, i);
}

static void hist_add(struct msession *s)
{
	if (s->ed_len == 0)
		return;
	s->ed_buf[s->ed_len] = 0;
	if (hist_n > 0 && strcmp(hist[hist_n - 1], s->ed_buf) == 0)
		return;
	if (hist_n == HIST_LINES) {
		memmove(hist[0], hist[1], sizeof hist[0] * (HIST_LINES - 1));
		hist_n--;
	}
	strcpy(hist[hist_n++], s->ed_buf);
}

static void hist_up(struct msession *s)
{
	if (s->ed_hpos <= 0)
		return;
	if (s->ed_hpos == hist_n) {
		memcpy(s->ed_save, s->ed_buf, s->ed_len);
		s->ed_save[s->ed_len] = 0;
	}
	set_line(s, hist[--s->ed_hpos]);
}

static void hist_down(struct msession *s)
{
	if (s->ed_hpos >= hist_n)
		return;
	s->ed_hpos++;
	set_line(s, s->ed_hpos == hist_n ? s->ed_save : hist[s->ed_hpos]);
}

/* hand the line (and newline, unless at EOF) to the Shell */
static void finish(struct msession *s, BOOL eof)
{
	long room;

	move(s, s->ed_pos, s->ed_len);
	out(s, "\n", 1);
	if (!eof)
		hist_add(s);
	room = (long)sizeof s->inbuf - s->inlen;
	if (s->ed_len + 1 <= room) {
		memcpy(s->inbuf + s->inlen, s->ed_buf, s->ed_len);
		s->inlen += s->ed_len;
		if (!eof)
			s->inbuf[s->inlen++] = '\n';
	}
	if (eof)
		s->eof = TRUE;
	s->ed_len = s->ed_pos = 0;
	s->ed_active = FALSE;
	s->ed_hpos = hist_n;
}

/* the end of an escape sequence: ESC [ seq c, ESC O c or CSI seq c */
static void do_seq(struct msession *s, char c)
{
	const char *q = s->ed_seq;

	switch (c) {
	case 'A':
		hist_up(s);
		break;
	case 'B':
		hist_down(s);
		break;
	case 'C':
		if (s->ed_pos < s->ed_len) {
			move(s, s->ed_pos, s->ed_pos + 1);
			s->ed_pos++;
		}
		break;
	case 'D':
		if (s->ed_pos > 0) {
			move(s, s->ed_pos, s->ed_pos - 1);
			s->ed_pos--;
		}
		break;
	case 'H':
		move(s, s->ed_pos, 0);
		s->ed_pos = 0;
		break;
	case 'F':
		move(s, s->ed_pos, s->ed_len);
		s->ed_pos = s->ed_len;
		break;
	case '~':                       /* VT220 editing keys */
		if (!strcmp(q, "1") || !strcmp(q, "7"))
			do_seq(s, 'H');
		else if (!strcmp(q, "4") || !strcmp(q, "8"))
			do_seq(s, 'F');
		else if (!strcmp(q, "3"))
			delete_at(s, s->ed_pos);
		break;
	}
}

/* keys typed in cooked mode; complete lines go into s->inbuf */
void ed_keys(struct msession *s, const char *buf, long n)
{
	unsigned char c;
	long i;

	for (i = 0; i < n; i++) {
		c = (unsigned char)buf[i];
		if (!s->ed_active) {
			/* a new line starts where the prompt left the cursor */
			s->ed_active = TRUE;
			s->ed_startx = s->cx % con_width(s);
			s->ed_len = s->ed_pos = 0;
			s->ed_hpos = hist_n;
		}
		if (s->ed_esc == 1) {           /* after ESC */
			s->ed_seqlen = 0;
			s->ed_seq[0] = 0;
			s->ed_esc = c == '[' ? 2 : c == 'O' ? 3 : 0;
			continue;
		}
		if (s->ed_esc == 2) {           /* parameters, then the final */
			if (c >= 0x20 && c <= 0x3f) {
				if (s->ed_seqlen < (int)sizeof s->ed_seq - 1) {
					s->ed_seq[s->ed_seqlen++] = (char)c;
					s->ed_seq[s->ed_seqlen] = 0;
				}
				continue;
			}
			s->ed_esc = 0;
			do_seq(s, (char)c);
			continue;
		}
		if (s->ed_esc == 3) {           /* SS3: application cursor keys */
			s->ed_esc = 0;
			do_seq(s, (char)c);
			continue;
		}
		switch (c) {
		case ESC:
			s->ed_esc = 1;
			break;
		case CSI:
			s->ed_esc = 2;
			s->ed_seqlen = 0;
			s->ed_seq[0] = 0;
			break;
		case '\r':
		case '\n':
			finish(s, FALSE);
			break;
		case 0x1c:                      /* ^\: end of file */
			finish(s, TRUE);
			break;
		case '\b':
		case 0x7f:
			if (s->ed_pos > 0)
				delete_at(s, s->ed_pos - 1);
			break;
		case 0x18:                      /* ^X */
		case 0x15:                      /* ^U */
			move(s, s->ed_pos, 0);
			s->ed_pos = 0;
			s->ed_len = 0;
			out(s, "\033[J", 3);
			break;
		case 0x0b:                      /* ^K */
			s->ed_len = s->ed_pos;
			out(s, "\033[J", 3);
			break;
		case 0x01:                      /* ^A */
			do_seq(s, 'H');
			break;
		case 0x05:                      /* ^E */
			do_seq(s, 'F');
			break;
		default:
			if ((c >= 0x20 && c < 0x7f) || c >= 0xa0)
				insert(s, (char)c);
			break;
		}
	}
}

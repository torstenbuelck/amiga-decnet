/*
 * dncopy - copy a file from a DECnet node (DAP / FAL).
 *
 * Usage: dncopy NODE/A,SPEC/A,TO/A,USER/K,PASSWORD/K,NOBLOCK/S,VERBOSE/S,PROGRESS/K
 *   Text records get a linefeed; everything else is copied byte for byte.
 *   NOBLOCK for servers like FOTOFX. VERBOSE logs the DAP messages.
 *   Without USER, USER/PASSWORD/NOBLOCK come from ENV:DECnet/nodes.
 *   PROGRESS=WINDOW shows node, file, step and a progress bar in a window
 *   on the Workbench screen (like DAPGet on the Mac); PROGRESS=TEXT shows
 *   a progress line in the Shell. The size comes from the server's
 *   ATTRIB; without it only the bytes are counted. Ctrl-C or the close
 *   gadget cancel.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/decnet.h>

#include <stdio.h>
#include <string.h>

#include "dapamiga.h"

static const char version[] = "$VER: dncopy 0.2 (23.10.2026)";
unsigned long __stack = 16384;

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;

#define OUTBUF 8192

struct out {
	BPTR fh;
	UBYTE *buf;
	LONG used;
	LONG bytes, records;
	BOOL failed;
};

static BOOL flush(struct out *o)
{
	if (o->used && Write(o->fh, o->buf, o->used) != o->used)
		o->failed = TRUE;
	o->used = 0;
	return !o->failed;
}

static BOOL put(struct out *o, const unsigned char *p, LONG n)
{
	while (n > 0) {
		LONG c = OUTBUF - o->used;
		if (c > n)
			c = n;
		CopyMem((APTR)p, o->buf + o->used, c);
		o->used += c;
		p += c;
		n -= c;
		if (o->used == OUTBUF && !flush(o))
			return FALSE;
	}
	return TRUE;
}

/* ---------------------------------------------------------------- progress */

enum { P_NONE, P_TEXT, P_WINDOW };

static struct {
	int mode;
	struct Window *win;
	struct Screen *scr;
	struct DrawInfo *dri;
	WORD fh, base, rowh, labw, x0, w;
	WORD ynode, yfile, ystep, ybar, ybarh, ycount;
	char node[64];
	const char *spec;
	char step[64];
	LONG done, total;
	LONG waited;            /* seconds without an answer, 0 = none */
	struct DateStamp t0;    /* start of the data transfer */
	LONG lastdraw;
	WORD sweep;
	BOOL started, aborted;
	BOOL textline;          /* TEXT: a progress line is on screen */
} ui;

static LONG ticks_since(const struct DateStamp *t)
{
	struct DateStamp n;

	DateStamp(&n);
	return (n.ds_Days - t->ds_Days) * 24 * 60 * 60 * TICKS_PER_SECOND +
	       (n.ds_Minute - t->ds_Minute) * 60 * TICKS_PER_SECOND +
	       (n.ds_Tick - t->ds_Tick);
}

/* done/total in 1/1000, without overflow for large files */
static LONG permille(void)
{
	LONG d = ui.done > ui.total ? ui.total : ui.done;

	if (ui.total <= 0)
		return 0;
	return ui.total > 0x1fffff ? d / (ui.total / 1000) : d * 1000 / ui.total;
}

/* 1234567 -> "1,234,567" */
static char *num(char *buf, LONG v)
{
	char tmp[16];
	int n = 0, i = 0, j;

	do {
		if (n && n % 3 == 0)
			tmp[i++] = ',';
		tmp[i++] = '0' + v % 10;
		v /= 10;
		n++;
	} while (v > 0);
	for (j = 0; j < i; j++)
		buf[j] = tmp[i - 1 - j];
	buf[i] = 0;
	return buf;
}

/* transfer rate as "41.0 KB/s", empty during the first second */
static char *rate(char *buf)
{
	LONG t = ticks_since(&ui.t0);
	ULONG r;

	buf[0] = 0;
	if (t >= TICKS_PER_SECOND) {
		r = (ULONG)ui.done * 10 / 1024 * TICKS_PER_SECOND / t;
		sprintf(buf, "%lu.%lu KB/s", (unsigned long)(r / 10), (unsigned long)(r % 10));
	}
	return buf;
}

static void counter_text(char *buf)
{
	char a[16], b[16], r[16];

	if (ui.total > 0)
		sprintf(buf, "%s of %s bytes", num(a, ui.done), num(b, ui.total));
	else
		sprintf(buf, "%s bytes", num(a, ui.done));
	if (*rate(r)) {
		strcat(buf, "   ");
		strcat(buf, r);
	}
}

/* window: one line of text at y, cut to fit, rest of the line cleared */
static void wtext(WORD x, WORD y, WORD w, const char *s)
{
	struct RastPort *rp = ui.win->RPort;
	struct TextExtent te;
	ULONG n = strlen(s);

	SetAPen(rp, ui.dri->dri_Pens[BACKGROUNDPEN]);
	RectFill(rp, x, y, x + w - 1, y + ui.fh - 1);
	if (n) {
		n = TextFit(rp, (STRPTR)s, n, &te, NULL, 1, w, ui.fh + 1);
		SetAPen(rp, ui.dri->dri_Pens[TEXTPEN]);
		SetBPen(rp, ui.dri->dri_Pens[BACKGROUNDPEN]);
		SetDrMd(rp, JAM2);
		Move(rp, x, y + ui.base);
		Text(rp, (STRPTR)s, n);
	}
}

static void wlabel(WORD y, const char *label, const char *s)
{
	wtext(ui.x0, y, ui.labw, label);
	wtext(ui.x0 + ui.labw, y, ui.w - ui.labw, s);
}

static void wbar(void)
{
	struct RastPort *rp = ui.win->RPort;
	UWORD *pen = ui.dri->dri_Pens;
	WORD x0 = ui.x0, y0 = ui.ybar, x1 = ui.x0 + ui.w - 1, y1 = ui.ybar + ui.ybarh - 1;
	WORD iw = ui.w - 4, fx0 = 0, fx1 = 0;

	SetAPen(rp, pen[SHADOWPEN]);            /* recessed frame */
	Move(rp, x0, y1); Draw(rp, x0, y0); Draw(rp, x1, y0);
	SetAPen(rp, pen[SHINEPEN]);
	Move(rp, x1, y0 + 1); Draw(rp, x1, y1); Draw(rp, x0 + 1, y1);

	if (ui.total > 0) {
		fx1 = (WORD)(permille() * iw / 1000);
		if (fx1 > iw)
			fx1 = iw;
	} else if (ui.done > 0) {               /* size unknown: moving block */
		WORD bw = iw / 5, span = iw - bw, p = ui.sweep % (2 * span);
		fx0 = p < span ? p : 2 * span - p;
		fx1 = fx0 + bw;
	}
	SetAPen(rp, pen[BACKGROUNDPEN]);
	RectFill(rp, x0 + 2, y0 + 1, x1 - 2, y1 - 1);
	if (fx1 > fx0) {
		SetAPen(rp, pen[FILLPEN]);
		RectFill(rp, x0 + 2 + fx0, y0 + 1, x0 + 2 + fx1 - 1, y1 - 1);
	}
}

static void wstep(void)
{
	char buf[100];

	if (ui.waited)
		sprintf(buf, "%s  (waiting %ld s)", ui.step, (long)ui.waited);
	else
		strcpy(buf, ui.step);
	wtext(ui.x0, ui.ystep, ui.w, buf);
}

static void wcounter(void)
{
	char buf[100];

	buf[0] = 0;
	if (ui.started)
		counter_text(buf);
	wtext(ui.x0, ui.ycount, ui.w, buf);
}

static BOOL open_window(void)
{
	static const char sample[] = "Receiving  99,999,999 of 99,999,999 bytes   999.9 KB/s";
	struct Screen *scr;
	WORD iw, ih, ow, oh;

	IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 37);
	GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 37);
	if (!IntuitionBase || !GfxBase)
		return FALSE;
	scr = LockPubScreen(NULL);
	if (!scr)
		return FALSE;
	ui.scr = scr;
	ui.dri = GetScreenDrawInfo(scr);
	if (!ui.dri)
		return FALSE;
	ui.fh = scr->RastPort.TxHeight;
	ui.base = scr->RastPort.TxBaseline;
	ui.rowh = ui.fh + 3;
	ui.labw = TextLength(&scr->RastPort, (STRPTR)"Node: ", 6);
	ui.w = TextLength(&scr->RastPort, (STRPTR)sample, sizeof sample - 1);
	if (ui.w > scr->Width - 60)
		ui.w = scr->Width - 60;
	ui.ybarh = ui.fh + 4;
	iw = ui.w + 16;
	ih = 6 + 2 * ui.rowh + 6 + ui.rowh + ui.ybarh + 3 + ui.rowh + 3;
	ow = iw + scr->WBorLeft + scr->WBorRight;
	oh = ih + scr->WBorTop + scr->Font->ta_YSize + 1 + scr->WBorBottom;
	ui.win = OpenWindowTags(NULL,
		WA_Left, (scr->Width - ow) / 2,
		WA_Top, (scr->Height - oh) / 2,
		WA_InnerWidth, iw,
		WA_InnerHeight, ih,
		WA_Title, (ULONG)"dncopy - DECnet transfer",
		WA_PubScreen, (ULONG)scr,
		WA_IDCMP, IDCMP_CLOSEWINDOW,
		WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
		          WFLG_SMART_REFRESH | WFLG_NOCAREREFRESH | WFLG_RMBTRAP,
		TAG_DONE);
	if (!ui.win)
		return FALSE;
	ui.x0 = ui.win->BorderLeft + 8;
	ui.ynode = ui.win->BorderTop + 6;
	ui.yfile = ui.ynode + ui.rowh;
	ui.ystep = ui.yfile + ui.rowh + 6;
	ui.ybar = ui.ystep + ui.rowh;
	ui.ycount = ui.ybar + ui.ybarh + 3;
	wlabel(ui.ynode, "Node:", ui.node);
	wlabel(ui.yfile, "File:", ui.spec);
	wbar();
	return TRUE;
}

static void close_window(void)
{
	if (ui.win)
		CloseWindow(ui.win);
	if (ui.dri)
		FreeScreenDrawInfo(ui.scr, ui.dri);
	if (ui.scr)
		UnlockPubScreen(NULL, ui.scr);
	if (GfxBase)
		CloseLibrary((struct Library *)GfxBase);
	if (IntuitionBase)
		CloseLibrary((struct Library *)IntuitionBase);
	ui.win = NULL;
	ui.dri = NULL;
	ui.scr = NULL;
}

/* TEXT: the progress line, overwritten in place */
static void tline(void)
{
	char buf[180], c[80];
	int i, n;

	if (!ui.started) {
		sprintf(buf, "\r%s", ui.step);
		if (ui.waited)
			sprintf(buf + strlen(buf), "  (waiting %ld s)", (long)ui.waited);
	} else {
		counter_text(c);
		if (ui.total > 0) {
			LONG pc = permille() / 10;
			n = (int)(pc / 5);
			strcpy(buf, "\r[");
			for (i = 0; i < 20; i++)
				buf[2 + i] = i < n ? '#' : '.';
			sprintf(buf + 22, "] %3ld%%  %s", (long)pc, c);
		} else {
			sprintf(buf, "\r%s", c);
		}
		if (ui.waited)
			sprintf(buf + strlen(buf), "  (waiting %ld s)", (long)ui.waited);
	}
	strcat(buf, "    ");                    /* clear what a longer line left */
	ui.textline = TRUE;
	PutStr((STRPTR)buf);
	Flush(Output());
}

/* Ctrl-C or close gadget pressed? */
static BOOL ui_poll(void)
{
	struct IntuiMessage *m;

	if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
		ui.aborted = TRUE;
	if (ui.win)
		while ((m = (struct IntuiMessage *)GetMsg(ui.win->UserPort))) {
			if (m->Class == IDCMP_CLOSEWINDOW)
				ui.aborted = TRUE;
			ReplyMsg((struct Message *)m);
		}
	return ui.aborted;
}

static void ui_open(int mode, const char *node, const char *spec)
{
	ui.mode = mode;
	ui.spec = spec;
	strncpy(ui.node, node, sizeof ui.node - 1);
	strcpy(ui.step, "Connecting ...");
	if (mode == P_WINDOW && !open_window()) {
		close_window();
		PutStr((STRPTR)"dncopy: cannot open the progress window, showing text\n");
		ui.mode = P_TEXT;
	}
	if (ui.mode == P_WINDOW)
		wstep();
}

/* after the connect: show the address next to the name */
static void ui_node(const char *node)
{
	LONG a = DnNodeAddr((CONST_STRPTR)node);

	if (a >= 0 && !strchr(node, '.'))
		sprintf(ui.node, "%s (%ld.%ld)", node, (long)(a >> 10), (long)(a & 1023));
	if (ui.mode == P_WINDOW)
		wlabel(ui.ynode, "Node:", ui.node);
}

static void ui_step(const char *s)
{
	strcpy(ui.step, s);
	ui.waited = 0;
	if (ui.mode == P_WINDOW)
		wstep();
}

static void ui_wait(LONG s)
{
	if (s == ui.waited)
		return;
	ui.waited = s;
	if (ui.mode == P_WINDOW)
		wstep();
	else if (ui.mode == P_TEXT)
		tline();
}

static void ui_start(LONG total)
{
	ui.total = total;
	ui.started = TRUE;
	DateStamp(&ui.t0);
	ui.lastdraw = -100;
	ui_step("Receiving ...");
	if (ui.mode == P_WINDOW)
		wcounter();
}

static void ui_progress(LONG done, BOOL force)
{
	LONG t;

	ui.done = done;
	if (ui.mode == P_NONE)
		return;
	t = ticks_since(&ui.t0);
	if (!force && t - ui.lastdraw < TICKS_PER_SECOND / 5)
		return;
	ui.lastdraw = t;
	ui.sweep += 6;
	if (ui.mode == P_WINDOW) {
		wbar();
		wcounter();
	} else {
		tline();
	}
}

/* window: text over the step, bar and counter area, broken at spaces */
static void wwrap(const char *s)
{
	struct RastPort *rp = ui.win->RPort;
	struct TextExtent te;
	WORD y = ui.ystep, ymax = ui.ycount + ui.fh;
	ULONG n;

	SetAPen(rp, ui.dri->dri_Pens[BACKGROUNDPEN]);
	RectFill(rp, ui.x0, ui.ystep, ui.x0 + ui.w - 1, ymax - 1);
	while (*s && y + ui.fh <= ymax) {
		ULONG len = strlen(s);
		n = TextFit(rp, (STRPTR)s, len, &te, NULL, 1, ui.w, ui.fh + 1);
		if (n < len) {
			ULONG k = n;
			while (k > 0 && s[k] != ' ')
				k--;
			if (k > 0)
				n = k;
		}
		SetAPen(rp, ui.dri->dri_Pens[TEXTPEN]);
		SetDrMd(rp, JAM1);
		Move(rp, ui.x0, y + ui.base);
		Text(rp, (STRPTR)s, n);
		s += n;
		while (*s == ' ')
			s++;
		y += ui.rowh;
	}
}

/* final state; the window stays 1.5 s (4 s with an error) or until closed */
static void ui_finish(BOOL ok, const char *msg)
{
	LONG hold;
	struct DateStamp t;

	if (ui.mode == P_TEXT) {
		if (ui.textline)
			PutStr((STRPTR)"\n");
		Flush(Output());
		return;
	}
	if (ui.mode != P_WINDOW)
		return;
	ui.waited = 0;
	if (ok) {
		ui_step(msg);
		wbar();
		wcounter();
		hold = TICKS_PER_SECOND * 3 / 2;
	} else {
		wwrap(msg);
		hold = TICKS_PER_SECOND * 4;
	}
	ui.aborted = FALSE;
	DateStamp(&t);
	while (ticks_since(&t) < hold && !ui_poll())
		Delay(5);
}

/* ---------------------------------------------------------------- reading */

static long (*link_read)(void *, unsigned char *, long, long);

/* Waits in short slices, so the display can count the seconds without an
   answer and Ctrl-C / the close gadget work while the server is silent. */
static long slice_read(void *ctx, unsigned char *buf, long max, long timeout)
{
	long waited = 0, t, r;

	if (timeout <= 0)
		return link_read(ctx, buf, max, timeout);
	for (;;) {
		t = timeout - waited;
		if (t > 250)
			t = 250;
		r = link_read(ctx, buf, max, t);
		if (r != 0) {
			ui_wait(0);
			return r;
		}
		waited += t;
		if (ui_poll())
			return -1;
		if (waited >= 2000)
			ui_wait(waited / 1000);
		if (waited >= timeout)
			return 0;
	}
}

static int parse_mode(const char *s)
{
	char u[8];
	int i;

	if (!s)
		return P_NONE;
	for (i = 0; s[i] && i < 7; i++)
		u[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];
	u[i] = 0;
	if (!s[i] && !strcmp(u, "WINDOW"))
		return P_WINDOW;
	if (!s[i] && !strcmp(u, "TEXT"))
		return P_TEXT;
	return -1;
}

int main(void)
{
	static const char template[] =
		"NODE/A,SPEC/A,TO/A,USER/K,PASSWORD/K,NOBLOCK/S,VERBOSE/S,PROGRESS/K";
	LONG args[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	struct RDArgs *rda;
	struct dap *d;
	struct dap_file f;
	struct out o;
	struct DateStamp t0;
	const unsigned char *p;
	long n;
	char msg[200];
	int mode, r, rc = RETURN_FAIL;

	rda = ReadArgs((STRPTR)template, args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (STRPTR)"dncopy");
		return RETURN_FAIL;
	}
	mode = parse_mode((const char *)args[7]);
	if (mode < 0) {
		PutStr((STRPTR)"dncopy: PROGRESS must be WINDOW or TEXT\n");
		FreeArgs(rda);
		return RETURN_FAIL;
	}
	memset(&o, 0, sizeof o);
	o.buf = AllocVec(OUTBUF, MEMF_ANY);
	if (!o.buf)
		goto done;
	DateStamp(&t0);
	ui_open(mode, (const char *)args[0], (const char *)args[1]);
	d = dapa_open((const char *)args[0], (const char *)args[3], (const char *)args[4],
	              !args[5], args[6] != 0);
	if (!d) {
		ui_finish(FALSE, dapa_error);
		goto done;
	}
	ui_node((const char *)args[0]);
	link_read = d->read;
	d->read = slice_read;
	o.fh = Open((STRPTR)args[2], MODE_NEWFILE);
	if (!o.fh) {
		LONG err = IoErr();
		PrintFault(err, (STRPTR)args[2]);
		Fault(err, (STRPTR)args[2], (STRPTR)msg, sizeof msg);
		ui_finish(FALSE, msg);
	} else {
		ui_step("Opening the file ...");
		r = -1;
		if (dap_open_read(d, (const char *)args[1], &f) == 0) {
			ui_start(f.size > 0 ? f.size : 0);
			while ((r = dap_read_record(d, &p, &n)) > 0) {
				if (!put(&o, p, n) || (f.text && !put(&o, (const unsigned char *)"\n", 1)))
					break;
				o.bytes += n;
				o.records++;
				ui_progress(o.bytes, FALSE);
				if (ui_poll())
					break;
			}
			/* after a cancel just drop the link: a regular close would
			   first have to receive the rest of the file */
			if (r >= 0 && !ui.aborted)
				dap_close_file(d);
		}
		flush(&o);
		Close(o.fh);
		if (ui.aborted)
			strcpy(d->error, "cancelled");
		if (r == 0 && !o.failed) {
			LONG ticks = ticks_since(&t0);
			sprintf(msg, "%s -> %s: %ld bytes, %ld records in %ld.%ld s",
			        (char *)args[1], (char *)args[2], (long)o.bytes, (long)o.records,
			        (long)(ticks / TICKS_PER_SECOND),
			        (long)((ticks % TICKS_PER_SECOND) * 10 / TICKS_PER_SECOND));
			char done[40];
			/* the announced size is only an estimate (blocks, line ends) */
			ui.total = o.bytes;
			ui_progress(o.bytes, TRUE);
			sprintf(done, "Done in %ld.%ld s", (long)(ticks / TICKS_PER_SECOND),
			        (long)((ticks % TICKS_PER_SECOND) * 10 / TICKS_PER_SECOND));
			ui_finish(TRUE, done);
			Printf("%s\n", (STRPTR)msg);
			rc = RETURN_OK;
		} else {
			const char *e = o.failed ? "write error" : d->error;
			ui_finish(FALSE, e);
			Printf("%s\n", (STRPTR)e);
			DeleteFile((STRPTR)args[2]);
		}
	}
	dapa_close(d);
done:
	if (ui.mode == P_WINDOW)
		close_window();
	if (o.buf)
		FreeVec(o.buf);
	FreeArgs(rda);
	(void)version;
	return rc;
}

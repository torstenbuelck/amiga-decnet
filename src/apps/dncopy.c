/*
 * dncopy - copy a file from a DECnet node (DAP / FAL).
 *
 * Usage: dncopy NODE/A,SPEC/A,TO/A,USER/K,PASSWORD/K,NOBLOCK/S,VERBOSE/S
 *   Text records get a linefeed; everything else is copied byte for byte.
 *   NOBLOCK for servers like FOTOFX.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#include "dapamiga.h"

static const char version[] = "$VER: dncopy 0.1 (21.10.2026)";
unsigned long __stack = 16384;

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

static int record(void *ud, const unsigned char *p, long n, int text)
{
	struct out *o = ud;

	if (!put(o, p, n) || (text && !put(o, (const unsigned char *)"\n", 1)))
		return 1;
	o->bytes += n;
	o->records++;
	return (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) ? 1 : 0;
}

int main(void)
{
	static const char template[] =
		"NODE/A,SPEC/A,TO/A,USER/K,PASSWORD/K,NOBLOCK/S,VERBOSE/S";
	LONG args[7] = { 0, 0, 0, 0, 0, 0, 0 };
	struct RDArgs *rda;
	struct dap *d;
	struct out o;
	struct DateStamp t0, t1;
	long size;
	int rc = RETURN_FAIL;

	rda = ReadArgs((STRPTR)template, args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (STRPTR)"dncopy");
		return RETURN_FAIL;
	}
	memset(&o, 0, sizeof o);
	o.buf = AllocVec(OUTBUF, MEMF_ANY);
	if (!o.buf)
		goto done;
	DateStamp(&t0);
	d = dapa_open((const char *)args[0], (const char *)args[3], (const char *)args[4],
	              !args[5], args[6] != 0);
	if (!d)
		goto done;
	o.fh = Open((STRPTR)args[2], MODE_NEWFILE);
	if (!o.fh) {
		PrintFault(IoErr(), (STRPTR)args[2]);
	} else {
		int r = dap_get(d, (const char *)args[1], record, &o, &size);
		flush(&o);
		Close(o.fh);
		if (r == 0 && !o.failed) {
			LONG ticks;
			DateStamp(&t1);
			ticks = (t1.ds_Days - t0.ds_Days) * 24 * 60 * 60 * TICKS_PER_SECOND +
			        (t1.ds_Minute - t0.ds_Minute) * 60 * TICKS_PER_SECOND +
			        (t1.ds_Tick - t0.ds_Tick);
			Printf("%s -> %s: %ld bytes, %ld records in %ld.%ld s\n",
			       (STRPTR)args[1], (STRPTR)args[2], o.bytes, o.records,
			       ticks / TICKS_PER_SECOND, (ticks % TICKS_PER_SECOND) * 10 / TICKS_PER_SECOND);
			rc = RETURN_OK;
		} else {
			Printf("%s\n", (STRPTR)(o.failed ? "write error" : d->error));
			DeleteFile((STRPTR)args[2]);
		}
	}
	dapa_close(d);
done:
	if (o.buf)
		FreeVec(o.buf);
	FreeArgs(rda);
	(void)version;
	return rc;
}

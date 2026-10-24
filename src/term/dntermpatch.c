/*
 * dntermpatch - make a copy of a program that opens dnterm.device where
 * the original opens serial.device.
 *
 *   dntermpatch FROM/A TO/A
 *
 * For terminal programs that have the device name built in (VAXterm):
 * both names are 13 characters long, so the copy stays the same size.
 * Only the exact string "serial.device" with its terminating NUL is
 * replaced; the original is not touched.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

static const char version[] = "$VER: dntermpatch 0.1 (24.10.2026)";

#define OLD "serial.device"
#define NEW "dnterm.device"

int main(void)
{
	LONG args[2] = { 0, 0 };
	struct RDArgs *rda;
	BPTR in = 0, out = 0;
	UBYTE *buf = NULL;
	LONG size, n, i, count = 0;
	int rc = RETURN_FAIL;

	(void)version;
	rda = ReadArgs((STRPTR)"FROM/A,TO/A", args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (STRPTR)"dntermpatch");
		return RETURN_FAIL;
	}
	in = Open((STRPTR)args[0], MODE_OLDFILE);
	if (!in) {
		PrintFault(IoErr(), (STRPTR)args[0]);
		goto done;
	}
	Seek(in, 0, OFFSET_END);
	size = Seek(in, 0, OFFSET_BEGINNING);
	if (size <= 0 || !(buf = AllocVec(size, MEMF_ANY))) {
		Printf("%s: cannot read it\n", args[0]);
		goto done;
	}
	if (Read(in, buf, size) != size) {
		PrintFault(IoErr(), (STRPTR)args[0]);
		goto done;
	}
	n = sizeof OLD;                 /* with the NUL */
	for (i = 0; i + n <= size; i++) {
		if (buf[i] == 's' && !memcmp(buf + i, OLD, n)) {
			memcpy(buf + i, NEW, n);
			count++;
			i += n - 1;
		}
	}
	if (!count) {
		Printf("%s does not contain \"" OLD "\"; nothing written.\n", args[0]);
		rc = RETURN_WARN;
		goto done;
	}
	out = Open((STRPTR)args[1], MODE_NEWFILE);
	if (!out || Write(out, buf, size) != size) {
		PrintFault(IoErr(), (STRPTR)args[1]);
		goto done;
	}
	Printf("%s: %ld x \"" OLD "\" -> \"" NEW "\"\n", args[1], count);
	rc = RETURN_OK;
done:
	if (out)
		Close(out);
	if (in)
		Close(in);
	if (buf)
		FreeVec(buf);
	FreeArgs(rda);
	return rc;
}

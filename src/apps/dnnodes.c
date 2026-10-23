/*
 * dnnodes - copy node names from another node's NML (object 19).
 *
 * Usage: dnnodes FROM/A,UPDATE/S,REACHABLE/S,FILE/K,USER/K,PASSWORD/K,VERBOSE/S
 *
 *   dnnodes FROM=EUGENE           list the nodes EUGENE knows with their
 *                                 state, marking the ones missing in the
 *                                 nodes file as new
 *   dnnodes FROM=EUGENE UPDATE    append the new ones to the nodes file
 *   REACHABLE                     only nodes the source can reach now
 *                                 (the source itself always counts); the
 *                                 others are often long offline
 *
 * Only nodes with a name whose name and address are both unknown in the
 * file are added; existing lines (access data, options) are never
 * changed. The new lines follow a comment naming the source and date.
 * FILE defaults to ENV:DECnet/nodes; then UPDATE appends the same lines to
 * ENVARC:DECnet/nodes too, if it exists, so they survive a reboot. Like
 * NCP COPY KNOWN NODES this reads
 * the source's volatile database (nodes it currently knows about).
 *
 * Copyright (C) 2026  Torsten
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/datetime.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/decnet.h>

#include <string.h>

#include "../nml/nml.h"

static const char version[] = "$VER: dnnodes 0.2 (23.10.2026)";
unsigned long __stack = 16384;

#define MAXNODES 1024
#define DEFFILE  "ENV:DECnet/nodes"
#define ARCFILE  "ENVARC:DECnet/nodes"

struct Library *DECnetBase;
static struct DnLink *link;

struct node {
	UWORD addr;
	WORD state;                 /* NML_STATE_..., as the source sees it */
	char name[24];
};

static struct node *have, *got;
static LONG nhave, ngot;

static long link_read(void *ctx, unsigned char *buf, long max, long timeout)
{
	(void)ctx;
	return DnRead(link, buf, max, timeout);
}

static int link_write(void *ctx, const unsigned char *buf, long n)
{
	(void)ctx;
	return DnWrite(link, buf, n) == 0 ? 0 : -1;
}

static void nml_log(void *ctx, const char *msg)
{
	(void)ctx;
	Printf("nml: %s\n", (STRPTR)msg);
}

static int ieq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char x = *a, y = *b;
		if (x >= 'a' && x <= 'z') x -= 32;
		if (y >= 'a' && y <= 'z') y -= 32;
		if (x != y)
			return 0;
	}
	return *a == *b;
}

/* "area.node" -> address, 0 if not one */
static UWORD parse_addr(const char *s, int len)
{
	LONG a = 0, n = 0, *v = &a;
	int i, dot = 0;

	for (i = 0; i < len; i++) {
		if (s[i] == '.' && !dot) {
			dot = 1;
			v = &n;
		} else if (s[i] >= '0' && s[i] <= '9') {
			*v = *v * 10 + (s[i] - '0');
		} else {
			return 0;
		}
	}
	if (!dot || a < 1 || a > 63 || n < 1 || n > 1023)
		return 0;
	return (UWORD)(a << 10 | n);
}

/* Name and address of every node line in buf. */
static void parse_file(const char *buf, LONG len)
{
	const char *p = buf, *e = buf + len;

	while (p < e && nhave < MAXNODES) {
		const char *eol = p, *w1, *w2;
		int l1, l2;

		while (eol < e && *eol != '\n')
			eol++;
		while (p < eol && (*p == ' ' || *p == '\t'))
			p++;
		if (p < eol && *p != ';' && *p != '#') {
			w1 = p;
			while (p < eol && *p != ' ' && *p != '\t')
				p++;
			l1 = p - w1;
			while (p < eol && (*p == ' ' || *p == '\t'))
				p++;
			w2 = p;
			while (p < eol && *p != ' ' && *p != '\t' && *p != '\r')
				p++;
			l2 = p - w2;
			if (l1 > 0 && l1 < (int)sizeof have->name) {
				memcpy(have[nhave].name, w1, l1);
				have[nhave].name[l1] = 0;
				have[nhave].addr = parse_addr(w2, l2);
				nhave++;
			}
		}
		p = eol + 1;
	}
}

static BOOL known(const struct node *n)
{
	LONG i;

	for (i = 0; i < nhave; i++)
		if (ieq(have[i].name, n->name) || have[i].addr == n->addr)
			return TRUE;
	return FALSE;
}

/* RawDoFmt PutChProc: move.b d0,(a3)+ ; rts. A UWORD array, so it is on an
   even address (a string literal may be odd: guru 80000003). */
static const UWORD putchproc[] = { 0x16c0, 0x4e75 };

static BOOL only_reachable;
static LONG nskipped;

static int collect(void *ud, unsigned addr, const char *name, int exec, int state)
{
	(void)ud;
	if (exec)
		state = NML_STATE_REACHABLE;    /* the source, we are talking to it */
	if (name[0] && only_reachable && state != NML_STATE_REACHABLE) {
		nskipped++;
	} else if (name[0] && ngot < MAXNODES) {
		got[ngot].addr = (UWORD)addr;
		got[ngot].state = (WORD)state;
		strncpy(got[ngot].name, name, sizeof got->name - 1);
		got[ngot].name[sizeof got->name - 1] = 0;
		ngot++;
	}
	return (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) ? 1 : 0;
}

/* The whole file, NULL (and *len 0) if it does not exist. */
static char *read_file(const char *name, LONG *len)
{
	BPTR fh = Open((STRPTR)name, MODE_OLDFILE);
	char *buf;

	*len = 0;
	if (!fh)
		return NULL;
	Seek(fh, 0, OFFSET_END);
	*len = Seek(fh, 0, OFFSET_BEGINNING);
	buf = AllocVec(*len + 1, MEMF_ANY);
	if (buf && Read(fh, buf, *len) != *len) {
		FreeVec(buf);
		buf = NULL;
	}
	Close(fh);
	if (!buf)
		*len = 0;
	return buf;
}

static BOOL append(const char *file, const char *from, BOOL newline_first)
{
	struct DateTime dt;
	char day[LEN_DATSTRING], line[80];
	BPTR fh;
	LONG i;
	BOOL ok;

	fh = Open((STRPTR)file, MODE_READWRITE);
	if (!fh)
		return FALSE;
	Seek(fh, 0, OFFSET_END);
	memset(&dt, 0, sizeof dt);
	DateStamp(&dt.dat_Stamp);
	dt.dat_Format = FORMAT_DOS;
	dt.dat_StrDate = (STRPTR)day;
	DateToStr(&dt);
	for (i = strlen(day); i > 0 && day[i - 1] == ' '; i--)   /* DateToStr pads */
		day[i - 1] = 0;
	ok = FPrintf(fh, (STRPTR)"%s; added by dnnodes from %s on %s\n",
	             (STRPTR)(newline_first ? "\n" : ""), (STRPTR)from, (STRPTR)day) >= 0;
	for (i = 0; ok && i < ngot; i++)
		if (!known(&got[i])) {
			int n = strlen(got[i].name);
			strcpy(line, got[i].name);
			while (n < 8)
				line[n++] = ' ';
			line[n] = 0;
			ok = FPrintf(fh, (STRPTR)"%s%ld.%ld\n", (STRPTR)line,
			             (LONG)(got[i].addr >> 10), (LONG)(got[i].addr & 1023)) >= 0;
		}
	Close(fh);
	return ok;
}

int main(void)
{
	static const char template[] =
		"FROM/A,UPDATE/S,REACHABLE/S,FILE/K,USER/K,PASSWORD/K,VERBOSE/S";
	static const char *states[] = { "on", "off", "shut", "restricted", "reachable",
	                                "unreachable" };
	LONG args[7] = { 0, 0, 0, 0, 0, 0, 0 };
	const char *from, *file;
	struct DnConnectReq req;
	struct RDArgs *rda;
	struct nml *n = NULL;
	char *buf = NULL;
	LONG addr, len, i, nnew = 0;
	int rc = RETURN_FAIL;

	rda = ReadArgs((STRPTR)template, args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (STRPTR)"dnnodes");
		return RETURN_FAIL;
	}
	from = (const char *)args[0];
	only_reachable = args[2] != 0;
	file = args[3] ? (const char *)args[3] : DEFFILE;

	have = AllocVec(MAXNODES * sizeof(struct node), MEMF_ANY);
	got = AllocVec(MAXNODES * sizeof(struct node), MEMF_ANY);
	n = AllocVec(sizeof(struct nml), MEMF_ANY | MEMF_CLEAR);
	if (!have || !got || !n) {
		Printf("out of memory\n");
		goto out;
	}
	buf = read_file(file, &len);
	if (buf)
		parse_file(buf, len);

	DECnetBase = OpenLibrary((STRPTR)DECNETNAME, DECNET_VERSION);
	if (!DECnetBase) {
		Printf("cannot open %s\n", (STRPTR)DECNETNAME);
		goto out;
	}
	addr = DnNodeAddr((CONST_STRPTR)from);
	if (addr < 0) {
		Printf("unknown node %s\n", (STRPTR)from);
		goto out;
	}
	memset(&req, 0, sizeof req);
	req.dcr_Node = (UWORD)addr;
	req.dcr_ObjNum = NML_OBJECT;
	req.dcr_SrcName = (CONST_STRPTR)"DNNODES";
	req.dcr_User = (CONST_STRPTR)args[4];
	req.dcr_Password = (CONST_STRPTR)args[5];
	req.dcr_Flags = DCRF_DATA;
	req.dcr_Data = nml_version;
	req.dcr_DataLen = sizeof nml_version;
	link = DnConnect(&req);
	if (!link) {
		if (req.dcr_Reason == DNREASON_NODNETD)
			Printf("dnetd is not running\n");
		else
			Printf("connect to NML on %s failed (reason %ld)\n", (STRPTR)from, req.dcr_Reason);
		goto out;
	}
	n->read = link_read;
	n->write = link_write;
	n->log = nml_log;
	n->verbose = args[6] != 0;
	if (nml_known_nodes(n, 0, collect, NULL) < 0) {
		Printf("%s\n", (STRPTR)n->error);
		goto out;
	}

	for (i = 0; i < ngot; i++) {
		BOOL isnew = !known(&got[i]);
		char a[12];
		LONG args2[2];

		args2[0] = got[i].addr >> 10;
		args2[1] = got[i].addr & 1023;
		RawDoFmt((STRPTR)"%ld.%ld", args2, (void (*)())putchproc, a);
		Printf("%-8s %-9s %-11s%s\n", (STRPTR)got[i].name, (STRPTR)a,
		       (STRPTR)(got[i].state >= 0 && got[i].state <= 5 ? states[got[i].state] : "-"),
		       (STRPTR)(isnew ? " new" : ""));
		if (isnew)
			nnew++;
	}
	Printf("\n%ld named nodes from %s, %ld not in %s.\n", ngot, (STRPTR)from, nnew,
	       (STRPTR)file);
	if (nskipped)
		Printf("%ld named node%s not reachable from %s left out (REACHABLE).\n", nskipped,
		       (STRPTR)(nskipped == 1 ? "" : "s"), (STRPTR)from);
	if (args[1] && nnew > 0) {
		if (!append(file, from, len > 0 && buf && buf[len - 1] != '\n')) {
			PrintFault(IoErr(), (STRPTR)file);
			goto out;
		}
		Printf("%ld node%s added to %s.\n", nnew, (STRPTR)(nnew == 1 ? "" : "s"), (STRPTR)file);
		if (!args[3]) {
			LONG alen;
			char *abuf = read_file(ARCFILE, &alen);
			if (abuf) {
				if (append(ARCFILE, from, alen > 0 && abuf[alen - 1] != '\n'))
					Printf("Also added to %s.\n", (STRPTR)ARCFILE);
				else
					PrintFault(IoErr(), (STRPTR)ARCFILE);
				FreeVec(abuf);
			}
		}
	} else if (nnew > 0) {
		Printf("Use UPDATE to add them.\n");
	}
	rc = RETURN_OK;
out:
	if (link)
		DnClose(link);
	if (DECnetBase)
		CloseLibrary(DECnetBase);
	if (buf)
		FreeVec(buf);
	if (n)
		FreeVec(n);
	if (got)
		FreeVec(got);
	if (have)
		FreeVec(have);
	FreeArgs(rda);
	(void)version;
	return rc;
}

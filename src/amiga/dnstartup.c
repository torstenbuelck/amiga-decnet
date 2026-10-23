/*
 * dnstartup - add or remove the DECnet block in S:User-Startup.
 *
 * Usage: dnstartup DIR=drawer [FILE=S:User-Startup]   add or replace it
 *        dnstartup REMOVE [FILE=S:User-Startup]       remove it
 *
 * The block looks like the ones the Commodore Installer writes:
 *
 *   ;BEGIN DECnet
 *   Assign DECnet: "drawer"
 *   Execute DECnet:S/DECnet-Start
 *   ;END DECnet
 *
 * dnetd has to configure the network card before any TCP/IP stack does,
 * because a SANA-II unit is configured only once per boot. So the block
 * goes in front of the first line that starts network software (AmiTCP,
 * startnet, Roadshow's AddNetInterface, Miami, Genesis), else at the end.
 * An old DECnet block is removed first. The previous file is kept as
 * FILE.bak.
 *
 * It also looks at S:Startup-Sequence: network software started there
 * before User-Startup comes too early for DECnet (return code 5).
 *
 * Return codes: 0 done, 5 done but network software starts before
 * User-Startup, 20 error.
 *
 * Requires AmigaOS 2.04 (V37), runs on a 68000.
 *
 * Copyright (C) 2026  Torsten
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

static const char version[] = "$VER: dnstartup 0.1 (23.10.2026)";

#define BEGIN_LINE ";BEGIN DECnet"
#define END_LINE   ";END DECnet"

static const char *networds[] = {
	"amitcp", "startnet", "addnetinterface", "roadshow", "miami", "genesis", NULL
};

static int lower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* Does line[0..len) contain word (lower case), ignoring case? */
static BOOL contains(const char *line, LONG len, const char *word)
{
	LONG i, j, n = strlen(word);

	for (i = 0; i + n <= len; i++) {
		for (j = 0; j < n && lower(line[i + j]) == word[j]; j++)
			;
		if (j == n)
			return TRUE;
	}
	return FALSE;
}

/* Does the trimmed line start with s, ignoring case? */
static BOOL starts(const char *line, LONG len, const char *s)
{
	LONG i = 0, k, n = strlen(s);

	while (i < len && (line[i] == ' ' || line[i] == '\t'))
		i++;
	if (len - i < n)
		return FALSE;
	for (k = 0; k < n; k++)
		if (lower(line[i + k]) != lower(s[k]))
			return FALSE;
	return TRUE;
}

static BOOL comment(const char *line, LONG len)
{
	LONG i = 0;

	while (i < len && (line[i] == ' ' || line[i] == '\t'))
		i++;
	return i == len || line[i] == ';';
}

/* A line that starts network software. */
static BOOL netline(const char *line, LONG len)
{
	int k;

	if (comment(line, len))
		return FALSE;
	for (k = 0; networds[k]; k++)
		if (contains(line, len, networds[k]))
			return TRUE;
	return FALSE;
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

static BOOL write_file(const char *name, const char *buf, LONG len)
{
	BPTR fh = Open((STRPTR)name, MODE_NEWFILE);
	BOOL ok;

	if (!fh)
		return FALSE;
	ok = Write(fh, (APTR)buf, len) == len;
	Close(fh);
	return ok;
}

static BOOL put(BPTR fh, const char *s, LONG n)
{
	return Write(fh, (APTR)s, n) == n;
}

static BOOL puts_(BPTR fh, const char *s)
{
	return put(fh, s, strlen(s));
}

static BOOL put_block(BPTR fh, const char *dir)
{
	return puts_(fh, BEGIN_LINE "\nAssign DECnet: \"") && puts_(fh, dir)
	    && puts_(fh, "\"\nExecute DECnet:S/DECnet-Start\n" END_LINE "\n");
}

/*
 * Where the DECnet block goes: the start of the first line that starts
 * network software, or of the Installer block (;BEGIN x ... ;END x) that
 * contains it, so other blocks stay whole. -1: at the end.
 */
static LONG insert_pos(const char *buf, LONG len)
{
	LONG i, start, block = -1;

	for (start = 0; start < len; start = i + 1) {
		for (i = start; i < len && buf[i] != '\n'; i++)
			;
		if (starts(buf + start, i - start, BEGIN_LINE))
			continue;               /* ours, removed anyway */
		if (starts(buf + start, i - start, ";BEGIN "))
			block = start;
		else if (starts(buf + start, i - start, ";END "))
			block = -1;
		else if (netline(buf + start, i - start))
			return block >= 0 ? block : start;
	}
	return -1;
}

/* Network software in S:Startup-Sequence before User-Startup runs? */
static BOOL net_before_user_startup(void)
{
	LONG len, i, start;
	char *buf = read_file("S:Startup-Sequence", &len);
	BOOL found = FALSE;

	if (!buf)
		return FALSE;
	for (start = 0; start < len; start = i + 1) {
		for (i = start; i < len && buf[i] != '\n'; i++)
			;
		if (!comment(buf + start, i - start) && contains(buf + start, i - start, "user-startup"))
			break;
		if (netline(buf + start, i - start)) {
			found = TRUE;
			break;
		}
	}
	FreeVec(buf);
	return found;
}

int main(void)
{
	static const char template[] = "DIR/K,REMOVE/S,FILE/K";
	LONG args[3] = { 0, 0, 0 };
	struct RDArgs *rda;
	const char *dir, *file;
	char bak[256];
	char *buf = NULL;
	LONG len, i, start, at;
	BPTR fh = 0;
	BOOL remove, inblock = FALSE, done = FALSE, ok = TRUE, had = FALSE;
	int rc = RETURN_FAIL;

	(void)version;
	rda = ReadArgs((STRPTR)template, args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (STRPTR)"dnstartup");
		return RETURN_FAIL;
	}
	dir = (const char *)args[0];
	remove = args[1] != 0;
	file = args[2] ? (const char *)args[2] : "S:User-Startup";
	if (!dir == !remove) {
		PutStr((STRPTR)"dnstartup: give either DIR=drawer or REMOVE\n");
		goto out;
	}
	if (strlen(file) > sizeof bak - 5) {
		PutStr((STRPTR)"dnstartup: file name too long\n");
		goto out;
	}

	buf = read_file(file, &len);
	if (buf) {
		strcpy(bak, file);
		strcat(bak, ".bak");
		if (!write_file(bak, buf, len)) {
			PrintFault(IoErr(), (STRPTR)bak);
			goto out;
		}
	} else if (remove) {
		Printf("%s does not exist, nothing to remove\n", (STRPTR)file);
		rc = RETURN_OK;
		goto out;
	}

	fh = Open((STRPTR)file, MODE_NEWFILE);
	if (!fh) {
		PrintFault(IoErr(), (STRPTR)file);
		goto out;
	}
	at = remove ? -1 : insert_pos(buf, len);
	for (start = 0; ok && start < len; start = i + 1) {
		LONG n;

		for (i = start; i < len && buf[i] != '\n'; i++)
			;
		n = i - start;
		if (inblock) {
			if (starts(buf + start, n, END_LINE))
				inblock = FALSE;
			continue;
		}
		if (starts(buf + start, n, BEGIN_LINE)) {
			inblock = had = TRUE;
			continue;
		}
		if (!remove && !done && start == at) {
			ok = put_block(fh, dir);
			done = TRUE;
		}
		ok = ok && put(fh, buf + start, n) && (i == len || put(fh, "\n", 1));
	}
	if (ok && !remove && !done) {
		if (len > 0 && buf[len - 1] != '\n')
			ok = put(fh, "\n", 1);
		ok = ok && put_block(fh, dir);
	}
	if (!Close(fh))
		ok = FALSE;
	fh = 0;
	if (!ok) {
		PrintFault(IoErr(), (STRPTR)file);
		if (buf)
			Printf("The old file is %s\n", (STRPTR)bak);
		goto out;
	}

	if (remove)
		Printf(had ? "DECnet block removed from %s\n" : "%s has no DECnet block\n",
		       (STRPTR)file);
	else
		Printf(done ? "DECnet block added to %s, before the network software\n"
		            : "DECnet block added to the end of %s\n", (STRPTR)file);
	rc = RETURN_OK;
	if (!remove && net_before_user_startup()) {
		PutStr((STRPTR)"Warning: S:Startup-Sequence starts network software before\n"
		       "User-Startup, so it configures the card before DECnet can.\n"
		       "Move that line below the User-Startup call.\n");
		rc = RETURN_WARN;
	}
out:
	if (buf)
		FreeVec(buf);
	FreeArgs(rda);
	return rc;
}

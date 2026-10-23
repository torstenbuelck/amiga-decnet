/*
 * nodecfg.c - access data per node from ENV:DECnet/nodes (see nodecfg.h).
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <dos/dos.h>

#include <proto/dos.h>

#include <string.h>

#include "nodecfg.h"

int nc_ieq(const char *a, const char *b)
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

/* unsigned decimal at *pp; -1 if none */
static LONG num(const char **pp)
{
	const char *p = *pp;
	LONG v = 0;

	if (*p < '0' || *p > '9')
		return -1;
	while (*p >= '0' && *p <= '9')
		v = v * 10 + (*p++ - '0');
	*pp = p;
	return v;
}

UWORD nc_parse_addr(const char *s)
{
	LONG a, n;

	if ((a = num(&s)) < 1 || a > 63 || *s++ != '.' || (n = num(&s)) < 1 || n > 1023 || *s)
		return 0;
	return (UWORD)(a << 10 | n);
}

BOOL nc_word(const char **pp, char *out, int size)
{
	const char *p = *pp;
	int n = 0;

	while (*p == ' ' || *p == '\t')
		p++;
	if (!*p || *p == '\n' || *p == '\r')
		return FALSE;
	while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
		if (n < size - 1)
			out[n++] = *p;
		p++;
	}
	out[n] = 0;
	*pp = p;
	return TRUE;
}

/* Words after the address: USER name, PASSWORD word, NOBLOCK, PURGE,
   SHOW, in any order. The keywords keep a user or password that happens
   to read SHOW from being taken for an option. Unknown words are skipped. */
static void parse_opts(char f[][40], int n, struct nodecfg *cfg)
{
	int k;

	for (k = 2; k < n; k++) {
		if (nc_ieq(f[k], "USER") && k + 1 < n)
			strcpy(cfg->user, f[++k]);
		else if (nc_ieq(f[k], "PASSWORD") && k + 1 < n)
			strcpy(cfg->pass, f[++k]);
		else if (nc_ieq(f[k], "NOBLOCK"))
			cfg->noblock = 1;
		else if (nc_ieq(f[k], "PURGE"))
			cfg->purge = 1;
		else if (nc_ieq(f[k], "SHOW"))
			cfg->show = 1;
	}
}

BOOL nc_lookup(const char *name, struct nodecfg *cfg,
               void (*list)(const char *, int, void *), void *ud)
{
	char line[200], f[10][40];   /* name, address, then USER x PASSWORD y options */
	struct nodecfg tmp;
	BPTR fh;
	BOOL found = FALSE;
	UWORD addr;

	if (name && (addr = nc_parse_addr(name)) != 0) {
		memset(cfg, 0, sizeof *cfg);
		strncpy(cfg->name, name, sizeof cfg->name - 1);
		cfg->addr = addr;
		found = TRUE;               /* still look for access data */
	}
	fh = Open((STRPTR)"ENV:DECnet/nodes", MODE_OLDFILE);
	if (!fh)
		return found;
	while (FGets(fh, (STRPTR)line, sizeof line)) {
		const char *p = line;
		int k;
		if (line[0] == ';' || line[0] == '#')
			continue;
		memset(f, 0, sizeof f);
		for (k = 0; k < 10 && nc_word(&p, f[k], sizeof f[k]); k++)
			;
		if (k < 2 || (addr = nc_parse_addr(f[1])) == 0)
			continue;
		if (list) {
			memset(&tmp, 0, sizeof tmp);
			parse_opts(f, k, &tmp);
			list(f[0], tmp.show, ud);
			continue;
		}
		if (!nc_ieq(f[0], name) && !(found && cfg->addr == addr))
			continue;
		memset(cfg, 0, sizeof *cfg);
		strncpy(cfg->name, f[0], sizeof cfg->name - 1);
		cfg->addr = addr;
		parse_opts(f, k, cfg);
		found = TRUE;
		break;
	}
	Close(fh);
	return found;
}

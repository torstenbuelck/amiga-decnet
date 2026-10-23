/*
 * nodecfg.h - access data per node from ENV:DECnet/nodes, shared by
 * dn-handler, dndir, dncopy and dnnodes.
 *
 * Line format: NAME ADDR [USER name] [PASSWORD word] [NOBLOCK] [PURGE] [SHOW]
 * (keywords after the address in any order; ';' or '#' starts a comment line)
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NODECFG_H
#define NODECFG_H

#include <exec/types.h>

struct nodecfg {
	char name[24];
	UWORD addr;
	char user[40], pass[40];
	int noblock;
	int purge;                  /* delete older versions after writing */
	int show;                   /* listed in the DN: root */
};

/* Fill cfg for name (name may also be "23.208"). Returns FALSE if unknown.
   With list != NULL, calls list(name, show, ud) for every node instead. */
BOOL nc_lookup(const char *name, struct nodecfg *cfg,
               void (*list)(const char *, int, void *), void *ud);

/* helpers the handler uses as well */
int nc_ieq(const char *a, const char *b);           /* equal, case ignored */
UWORD nc_parse_addr(const char *s);                 /* "area.node", 0 if not */
BOOL nc_word(const char **pp, char *out, int size); /* next blank separated word */

#endif

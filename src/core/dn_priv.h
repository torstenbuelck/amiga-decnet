/*
 * dn_priv.h - internals shared by the core modules.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DN_PRIV_H
#define DN_PRIV_H

#include "dn.h"

#define DN_MAXOBJECTS   16
#define DN_CACHESIZE    32
#define DN_HELLO_MS     15000UL     /* endnode hello timer T3 */
#define DN_CACHE_MS     120000UL    /* on-Ethernet cache lifetime */
#define DN_LONGHDR      20          /* long data header after the flags byte */

struct dn_cache_entry {
	dn_u16 node;
	dn_u32 seen;                /* dn_g.now at last refresh */
};

struct dn_core {
	struct dn_env env;
	int up;
	dn_u16 addr;
	dn_u8 mac[6];
	dn_u32 now;                 /* milliseconds since dn_init */
	dn_u32 nexthello;

	/* designated router */
	dn_u16 router;
	dn_u8  router_prio;
	dn_u32 router_seen;
	dn_u32 router_timeout;      /* ms */

	struct dn_cache_entry cache[DN_CACHESIZE];
	struct dn_object *objects[DN_MAXOBJECTS];

	dn_u8 frame[1520];          /* output buffer */
};

extern struct dn_core dn_g;

void dn_logf(int level, const char *fmt, ...);
#define DN_LOG(level, args) do { if ((level) <= dn_g.env.loglevel) dn_logf args; } while (0)

/* little endian helpers (68000: no unaligned word access) */
#define GET16(p)      ((dn_u16)((p)[0] | ((p)[1] << 8)))
#define PUT16(p, v)   do { (p)[0] = (dn_u8)((v) & 0xff); (p)[1] = (dn_u8)((v) >> 8); } while (0)

/* routing.c */
int  dn_route_send(dn_u16 dst, const dn_u8 *msg, unsigned len, int rqr);
void dn_route_tick(void);

/* nsp.c */
void dn_nsp_input(dn_u16 src, const dn_u8 *msg, unsigned len, int returned);
void dn_nsp_tick(void);
void dn_nsp_shutdown(void);

#endif

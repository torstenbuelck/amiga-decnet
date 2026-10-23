/*
 * routing.c - DECnet Phase IV routing layer, Ethernet endnode
 * (Routing Layer Functional Specification V2.0.0).
 *
 * Endnode hellos, designated router, on-Ethernet cache, long/short data
 * packets. Also holds the core setup and logging.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dn_priv.h"

struct dn_core dn_g;

const dn_u8 dn_all_routers[6]  = { 0xab, 0x00, 0x00, 0x03, 0x00, 0x00 };
const dn_u8 dn_all_endnodes[6] = { 0xab, 0x00, 0x00, 0x04, 0x00, 0x00 };

void dn_logf(int level, const char *fmt, ...)
{
	char buf[200];
	va_list ap;

	if (!dn_g.env.log)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	dn_g.env.log(dn_g.env.ctx, level, buf);
}

void dn_node_mac(dn_u8 *m, dn_u16 a)
{
	m[0] = 0xaa; m[1] = 0x00; m[2] = 0x04; m[3] = 0x00;
	m[4] = (dn_u8)(a & 0xff);
	m[5] = (dn_u8)(a >> 8);
}

static dn_u16 mac_node(const dn_u8 *m)
{
	if (m[0] == 0xaa && m[1] == 0x00 && m[2] == 0x04 && m[3] == 0x00)
		return GET16(m + 4);
	return 0;
}

const char *dn_ntoa(dn_u16 a)
{
	static char buf[4][12];
	static int n;

	n = (n + 1) & 3;
	sprintf(buf[n], "%u.%u", (unsigned)(a >> 10), (unsigned)(a & 0x3ff));
	return buf[n];
}

dn_u16 dn_designated_router(void)
{
	return dn_g.router;
}

int dn_init(const struct dn_env *env, dn_u16 address)
{
	memset(&dn_g, 0, sizeof dn_g);
	dn_g.env = *env;
	dn_g.addr = address;
	dn_node_mac(dn_g.mac, address);
	dn_g.up = 1;
	dn_g.nexthello = 0;     /* first hello at the first tick */
	DN_LOG(DN_LOG_INFO, (DN_LOG_INFO, "node %s up", dn_ntoa(address)));
	return 0;
}

void dn_shutdown(void)
{
	if (!dn_g.up)
		return;
	dn_nsp_shutdown();
	dn_g.up = 0;
}

/* ---- on-Ethernet cache ---------------------------------------------- */

static void cache_add(dn_u16 node)
{
	int i, free_i = -1, old_i = 0;

	for (i = 0; i < DN_CACHESIZE; i++) {
		if (dn_g.cache[i].node == node) {
			dn_g.cache[i].seen = dn_g.now;
			return;
		}
		if (!dn_g.cache[i].node && free_i < 0)
			free_i = i;
		if (dn_g.cache[i].seen < dn_g.cache[old_i].seen)
			old_i = i;
	}
	i = free_i >= 0 ? free_i : old_i;
	dn_g.cache[i].node = node;
	dn_g.cache[i].seen = dn_g.now;
	DN_LOG(DN_LOG_DEBUG, (DN_LOG_DEBUG, "cache: %s is on this Ethernet", dn_ntoa(node)));
}

static int cache_has(dn_u16 node)
{
	int i;

	for (i = 0; i < DN_CACHESIZE; i++)
		if (dn_g.cache[i].node == node)
			return dn_g.now - dn_g.cache[i].seen < DN_CACHE_MS;
	return 0;
}

/* ---- hellos ------------------------------------------------------------ */

static void send_hello(void)
{
	dn_u8 *p = dn_g.frame;
	unsigned n = 2;

	p[n++] = 0x0d;                          /* control, Ethernet endnode hello */
	p[n++] = 2; p[n++] = 0; p[n++] = 0;     /* version 2.0.0 */
	memcpy(p + n, dn_g.mac, 6); n += 6;     /* ID */
	p[n++] = 0x03;                          /* IINFO: endnode */
	PUT16(p + n, DN_BLKSIZE); n += 2;
	p[n++] = 0;                             /* AREA */
	memset(p + n, 0, 8); n += 8;            /* SEED */
	if (dn_g.router)
		dn_node_mac(p + n, dn_g.router);    /* NEIGHBOR */
	else
		memset(p + n, 0, 6);
	n += 6;
	PUT16(p + n, DN_HELLO_MS / 1000); n += 2;
	p[n++] = 0;                             /* MPD */
	p[n++] = 2; p[n++] = 0xaa; p[n++] = 0xaa;  /* test data */
	PUT16(p, n - 2);
	dn_g.env.send_frame(dn_g.env.ctx, dn_all_routers, p, n);
}

static void router_hello(const dn_u8 *m, unsigned len)
{
	dn_u16 id, timer;
	dn_u8 prio;

	/* flags, tiver[3], id[6], iinfo, blksize[2], priority, area, timer[2] */
	if (len < 17)
		return;
	id = mac_node(m + 4);
	if (!id)
		return;
	prio = m[13];
	timer = GET16(m + 15);
	if (!timer)
		timer = 15;

	/* designated router: highest priority, then highest address */
	if (dn_g.router == 0 || id == dn_g.router || prio > dn_g.router_prio ||
	    (prio == dn_g.router_prio && id > dn_g.router)) {
		if (id != dn_g.router) {
			DN_LOG(DN_LOG_INFO, (DN_LOG_INFO, "designated router %s (prio %u, hello %u s)",
			       dn_ntoa(id), prio, timer));
			dn_g.router = id;
			dn_g.nexthello = dn_g.now;     /* announce it at once */
		}
		dn_g.router_prio = prio;
		dn_g.router_seen = dn_g.now;
		dn_g.router_timeout = 3UL * timer * 1000;
	}
}

void dn_route_tick(void)
{
	if (dn_g.router && dn_g.now - dn_g.router_seen > dn_g.router_timeout) {
		DN_LOG(DN_LOG_INFO, (DN_LOG_INFO, "router %s lost", dn_ntoa(dn_g.router)));
		dn_g.router = 0;
		dn_g.nexthello = dn_g.now;
	}
	if ((long)(dn_g.now - dn_g.nexthello) >= 0) {
		send_hello();
		dn_g.nexthello = dn_g.now + DN_HELLO_MS;
	}
}

/* ---- data packets ---------------------------------------------------------- */

int dn_route_send(dn_u16 dst, const dn_u8 *msg, unsigned len, int rqr)
{
	dn_u8 *p = dn_g.frame;
	dn_u8 nexthop[6];
	int direct;
	unsigned n = 2;

	if (len + 3 + DN_LONGHDR > sizeof dn_g.frame)
		return -1;
	direct = !dn_g.router || cache_has(dst) || dst == dn_g.router;
	dn_node_mac(nexthop, direct ? dst : dn_g.router);

	p[n++] = 0x06 | (direct ? 0x20 : 0) | (rqr ? 0x08 : 0);   /* long, IE, RQR */
	p[n++] = 0; p[n++] = 0;                 /* D-AREA, D-SUBAREA */
	dn_node_mac(p + n, dst); n += 6;
	p[n++] = 0; p[n++] = 0;                 /* S-AREA, S-SUBAREA */
	memcpy(p + n, dn_g.mac, 6); n += 6;
	p[n++] = 0;                             /* NL2 */
	p[n++] = 0;                             /* VISIT-CT */
	p[n++] = 0;                             /* S-CLASS */
	p[n++] = 0;                             /* PT */
	memcpy(p + n, msg, len);
	n += len;
	PUT16(p, n - 2);
	dn_g.env.send_frame(dn_g.env.ctx, nexthop, p, n);
	return 0;
}

void dn_input(const dn_u8 *srcmac, const dn_u8 *dstmac, const dn_u8 *payload,
              unsigned len)
{
	const dn_u8 *m;
	unsigned mlen;
	dn_u8 flags;
	dn_u16 dst, src;

	(void)srcmac;
	(void)dstmac;
	if (!dn_g.up || len < 3)
		return;
	mlen = GET16(payload);
	if (mlen + 2 > len)
		return;
	m = payload + 2;
	flags = m[0];
	if (flags & 0x80) {                     /* padding */
		unsigned pad = flags & 0x7f;
		if (pad >= mlen)
			return;
		m += pad;
		mlen -= pad;
		flags = m[0];
	}
	if (flags & 0x40)                       /* VER must be 0 */
		return;

	if (flags & 0x01) {                     /* control message */
		if ((flags & 0x0f) == 0x0b)
			router_hello(m, mlen);
		return;                             /* endnode ignores the rest */
	}

	if ((flags & 0x07) == 0x06) {           /* long data */
		if (mlen < 1 + DN_LONGHDR)
			return;
		dst = mac_node(m + 3);
		src = mac_node(m + 11);
		m += 1 + DN_LONGHDR;
		mlen -= 1 + DN_LONGHDR;
	} else if ((flags & 0x07) == 0x02) {    /* short data */
		if (mlen < 6)
			return;
		dst = GET16(m + 1);
		src = GET16(m + 3);
		m += 6;
		mlen -= 6;
	} else {
		return;
	}
	if (flags & 0x10) {                     /* returned to sender */
		if (src != dn_g.addr)
			return;
		DN_LOG(DN_LOG_INFO, (DN_LOG_INFO, "packet to %s returned (unreachable)", dn_ntoa(dst)));
		dn_nsp_input(dst, m, mlen, 1);
		return;
	}
	if (dst != dn_g.addr || !src)
		return;
	/* Intra-Ethernet: sender is on our Ethernet, talk to it directly. */
	if (flags & 0x20)
		cache_add(src);
	dn_nsp_input(src, m, mlen, 0);
}

void dn_tick(unsigned ms)
{
	if (!dn_g.up)
		return;
	dn_g.now += ms;
	dn_route_tick();
	dn_nsp_tick();
}

/*
 * nsp.c - DECnet Network Services Protocol 4.1 and session control
 * (incoming connects so far).
 *
 * Logical links with a data and an "other data" subchannel, piggybacked
 * acks, flow control as requested by the remote end, retransmission and
 * segmentation.
 *
 * Copyright (C) 2026  Torsten
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>

#include "dn_priv.h"

/* message flags */
#define MSG_DATA        0x00    /* | BOM 0x20 | EOM 0x40 */
#define MSG_BOM         0x20
#define MSG_EOM         0x40
#define MSG_LS          0x10
#define MSG_INT         0x30
#define MSG_DATAACK     0x04
#define MSG_OTHACK      0x14
#define MSG_CONNACK     0x24
#define MSG_NOP         0x08
#define MSG_CI          0x18
#define MSG_RCI         0x68
#define MSG_CC          0x28
#define MSG_DI          0x38
#define MSG_DC          0x48

#define FC_NONE         0
#define FC_SEGMENT      1
#define FC_MESSAGE      2

#define SEQ(x)          ((dn_u16)((x) & 0x0fff))
#define MAXRETRY        10
#define MAXCTLRETRY     5
#define MAXOUTSTANDING  8
#define RTO_INITIAL     3000UL
#define RTO_MIN         500UL
#define RTO_MAX         30000UL
#define RXWINDOW        64      /* how far ahead of rx_last a segment is kept */
#define RXCACHE         16      /* out-of-order segments kept per link */
#define NAK_MS          200UL   /* gap age before the first NAK */
#define NAK_REPEAT_MS   1000UL

enum { L_FREE, L_CR, L_CC, L_RUN, L_DI, L_CI };

struct seg {
	struct seg *next;
	dn_u16 num;
	dn_u8 flags;            /* BOM/EOM */
	dn_u8 txcount;
	dn_u32 sent;
	unsigned len;
	dn_u8 data[1];
};

/* A data segment that came before the one we expect. Some SANA-II drivers
   (a2065.device on a real A3000, under a burst) hand frames over out of
   order; without the cache the whole rest of the burst would be thrown
   away and VMS would retransmit only after its timeout (18 s seen). */
struct rxseg {
	struct rxseg *next;
	dn_u16 num;
	dn_u8 flags;
	unsigned len;
	dn_u8 data[1];
};

struct dn_link {
	int state;
	dn_u16 locaddr, remaddr, node;
	struct dn_object *obj;
	void *user;
	dn_u16 segsize;         /* max data we may put in one segment */
	int remote_fc;          /* flow control the remote end asked for */

	/* data subchannel */
	dn_u16 tx_next;         /* next segment number to assign */
	struct seg *txq;        /* unacknowledged / unsent, in order */
	dn_u16 rx_last;         /* last segment received in order */
	int ack_pending;
	struct rxseg *rxq;      /* out-of-order segments, unsorted */
	int rxqlen;
	dn_u32 gap_since;       /* first out-of-order segment, 0 = no gap */
	dn_u32 nak_time;        /* last NAK sent for this gap */
	long flowcount;         /* segment/message request count from remote */
	int stopped;            /* remote said "do not send data" */

	/* other-data subchannel (link service, interrupts) */
	dn_u16 oth_rx_last;
	int oth_ack_pending;

	/* reassembly */
	dn_u8 *rxbuf;
	unsigned rxlen, rxsize;

	/* control message retransmission (CC, DI) */
	dn_u32 ctl_time;
	int ctl_retries;
	dn_u16 disc_reason;
	dn_u8 ctl_data[16];
	dn_u8 ctl_datalen;
	dn_u8 *ci_msg;          /* our connect initiate, for retransmission */
	unsigned ci_len;
	int ci_acked;           /* connect ack received: just wait */

	dn_u32 rto, srtt;
};

static struct dn_link links[DN_MAXLINKS];
static dn_u16 next_locaddr = 0x2101;
static dn_u8 msgbuf[1600];

static void *xalloc(unsigned n) { return dn_g.env.alloc(dn_g.env.ctx, n); }
static void xfree(void *p) { if (p) dn_g.env.free(dn_g.env.ctx, p); }

/* a comes before b in modulo 4096 sequence space */
static int seq_lt(dn_u16 a, dn_u16 b)
{
	dn_u16 d = SEQ(b - a);
	return d != 0 && d < 2048;
}

static int seq_le(dn_u16 a, dn_u16 b)
{
	return a == b || seq_lt(a, b);
}

/* ---- objects ------------------------------------------------------------- */

int dn_register_object(struct dn_object *obj)
{
	int i;

	for (i = 0; i < DN_MAXOBJECTS; i++)
		if (!dn_g.objects[i]) {
			dn_g.objects[i] = obj;
			return 0;
		}
	return -1;
}

static int name_eq(const char *a, const char *b)
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

struct dn_object *dn_find_object(dn_u8 number, const char *name)
{
	int i;

	for (i = 0; i < DN_MAXOBJECTS; i++) {
		struct dn_object *o = dn_g.objects[i];
		if (!o)
			continue;
		if (number && o->number == number)
			return o;
		if (!number && name && o->name && name_eq(o->name, name))
			return o;
	}
	return 0;
}

/* ---- link table ------------------------------------------------------------ */

static struct dn_link *link_alloc(void)
{
	int i, j;

	for (i = 0; i < DN_MAXLINKS; i++) {
		struct dn_link *l = &links[i];
		if (l->state != L_FREE)
			continue;
		memset(l, 0, sizeof *l);
		/* unique, non-zero local link address */
		for (;;) {
			next_locaddr++;
			if (!next_locaddr)
				continue;
			for (j = 0; j < DN_MAXLINKS; j++)
				if (links[j].state != L_FREE && links[j].locaddr == next_locaddr)
					break;
			if (j == DN_MAXLINKS)
				break;
		}
		l->locaddr = next_locaddr;
		l->tx_next = 1;
		l->rto = RTO_INITIAL;
		return l;
	}
	return 0;
}

static void link_free(struct dn_link *l)
{
	struct seg *s, *n;

	struct rxseg *r, *rn;

	for (s = l->txq; s; s = n) {
		n = s->next;
		xfree(s);
	}
	for (r = l->rxq; r; r = rn) {
		rn = r->next;
		xfree(r);
	}
	xfree(l->rxbuf);
	xfree(l->ci_msg);
	memset(l, 0, sizeof *l);
	l->state = L_FREE;
}

static struct dn_link *link_find(dn_u16 locaddr)
{
	int i;

	for (i = 0; i < DN_MAXLINKS; i++)
		if (links[i].state != L_FREE && links[i].locaddr == locaddr)
			return &links[i];
	return 0;
}

/* Tell the object and forget the link. */
static void link_gone(struct dn_link *l, dn_u16 reason)
{
	struct dn_object *o = l->obj;

	l->state = L_FREE;      /* callbacks must not touch it any more */
	if (o && o->disconnect)
		o->disconnect(l, reason);
	link_free(l);
}

/* ---- output ------------------------------------------------------------------ */

static unsigned hdr(dn_u8 *p, dn_u8 flags, const struct dn_link *l)
{
	p[0] = flags;
	PUT16(p + 1, l->remaddr);
	PUT16(p + 3, l->locaddr);
	return 5;
}

static void send_msg(const struct dn_link *l, unsigned len)
{
	dn_route_send(l->node, msgbuf, len, 0);
}

static void send_data_ack(struct dn_link *l)
{
	unsigned n = hdr(msgbuf, MSG_DATAACK, l);
	PUT16(msgbuf + n, 0x8000 | l->rx_last); n += 2;
	send_msg(l, n);
	l->ack_pending = 0;
}

/* Negative ack: all up to rx_last arrived, retransmit what follows. */
static void send_data_nak(struct dn_link *l)
{
	unsigned n = hdr(msgbuf, MSG_DATAACK, l);
	PUT16(msgbuf + n, 0x9000 | l->rx_last); n += 2;
	send_msg(l, n);
	l->ack_pending = 0;
	l->nak_time = dn_g.now;
}

static void send_oth_ack(struct dn_link *l)
{
	unsigned n = hdr(msgbuf, MSG_OTHACK, l);
	PUT16(msgbuf + n, 0x8000 | l->oth_rx_last); n += 2;
	send_msg(l, n);
	l->oth_ack_pending = 0;
}

static void send_segment(struct dn_link *l, struct seg *s)
{
	unsigned n = hdr(msgbuf, (dn_u8)(MSG_DATA | s->flags), l);

	PUT16(msgbuf + n, 0x8000 | l->rx_last); n += 2;    /* piggybacked ack */
	PUT16(msgbuf + n, s->num); n += 2;
	memcpy(msgbuf + n, s->data, s->len);
	n += s->len;
	send_msg(l, n);
	l->ack_pending = 0;
	s->sent = dn_g.now;
	s->txcount++;
}

static void send_cc(struct dn_link *l)
{
	unsigned n = hdr(msgbuf, MSG_CC, l);

	msgbuf[n++] = 0x01 | (FC_NONE << 2);    /* SERVICES */
	msgbuf[n++] = 0x03;                     /* INFO: NSP 4.1 */
	PUT16(msgbuf + n, DN_SEGSIZE); n += 2;
	msgbuf[n++] = l->ctl_datalen;
	memcpy(msgbuf + n, l->ctl_data, l->ctl_datalen);
	n += l->ctl_datalen;
	send_msg(l, n);
	l->ctl_time = dn_g.now;
}

static void send_di(struct dn_link *l)
{
	unsigned n = hdr(msgbuf, MSG_DI, l);

	PUT16(msgbuf + n, l->disc_reason); n += 2;
	msgbuf[n++] = l->ctl_datalen;
	memcpy(msgbuf + n, l->ctl_data, l->ctl_datalen);
	n += l->ctl_datalen;
	send_msg(l, n);
	l->ctl_time = dn_g.now;
}

static void send_dc(dn_u16 node, dn_u16 dstaddr, dn_u16 srcaddr, dn_u16 reason)
{
	msgbuf[0] = MSG_DC;
	PUT16(msgbuf + 1, dstaddr);
	PUT16(msgbuf + 3, srcaddr);
	PUT16(msgbuf + 5, reason);
	dn_route_send(node, msgbuf, 7, 0);
}

static int may_send(const struct dn_link *l)
{
	if (l->stopped)
		return 0;
	if (l->remote_fc == FC_NONE)
		return 1;
	return l->flowcount > 0;
}

/* Send queued segments that have not gone out yet, as flow control allows. */
static void push(struct dn_link *l)
{
	struct seg *s;
	int outstanding = 0;

	if (l->state != L_RUN)
		return;                         /* data only after CC is acknowledged */
	for (s = l->txq; s; s = s->next) {
		if (s->txcount) {
			outstanding++;
			continue;
		}
		if (outstanding >= MAXOUTSTANDING || !may_send(l))
			break;
		send_segment(l, s);
		outstanding++;
		if (l->remote_fc == FC_SEGMENT ||
		    (l->remote_fc == FC_MESSAGE && (s->flags & MSG_EOM)))
			l->flowcount--;
	}
}

/* ---- acknowledgements --------------------------------------------------------- */

static void data_acked(struct dn_link *l, dn_u16 num, int nak)
{
	struct seg *s;

	num = SEQ(num);
	while ((s = l->txq) != 0 && s->txcount && seq_le(s->num, num)) {
		if (s->txcount == 1) {
			dn_u32 sample = dn_g.now - s->sent;
			l->srtt = l->srtt ? (7 * l->srtt + sample) / 8 : sample;
			l->rto = 2 * l->srtt + 200;
			if (l->rto < RTO_MIN) l->rto = RTO_MIN;
			if (l->rto > RTO_MAX) l->rto = RTO_MAX;
		}
		l->txq = s->next;
		xfree(s);
	}
	if (nak) {
		for (s = l->txq; s && s->txcount; s = s->next)
			send_segment(l, s);
	}
}

/* Process the optional ACKNUM / ACKOTH fields at p. other = message is on
   the other-data subchannel. Returns the number of bytes consumed. */
static unsigned acks(struct dn_link *l, const dn_u8 *p, unsigned len, int other)
{
	unsigned n = 0;
	int i;

	for (i = 0; i < 2 && n + 2 <= len; i++) {
		dn_u16 v = GET16(p + n);
		int qual, cross;

		if (!(v & 0x8000))
			break;
		n += 2;
		qual = (v >> 12) & 3;
		cross = qual >= 2;
		if (cross == other)
			data_acked(l, v, qual & 1);
		/* acks for our other-data messages: we send none yet */
	}
	return n;
}

/* ---- input --------------------------------------------------------------------- */

static unsigned get_image(const dn_u8 *p, unsigned len, char *out, unsigned outsize)
{
	unsigned n;

	if (len < 1)
		return 0;
	n = p[0];
	if (n + 1 > len)
		n = len - 1;
	if (out) {
		unsigned c = n < outsize - 1 ? n : outsize - 1;
		memcpy(out, p + 1, c);
		out[c] = 0;
	}
	return n + 1;
}

/* End user descriptor (format 0/1/2). */
static unsigned get_enduser(const dn_u8 *p, unsigned len, dn_u8 *obj, char *name)
{
	unsigned n;

	if (len < 2)
		return len;
	*obj = p[1];
	name[0] = 0;
	switch (p[0]) {
	case 0:
		return 2;
	case 1:
		n = 2;
		break;
	case 2:
		n = 6;  /* group and user code */
		break;
	default:
		return len;
	}
	if (n > len)
		return len;
	return n + get_image(p + n, len - n, name, 17);
}

static void handle_ci(dn_u16 node, const dn_u8 *m, unsigned len)
{
	struct dn_conninfo ci;
	struct dn_object *obj;
	struct dn_link *l;
	dn_u16 remaddr;
	unsigned i;
	int k;

	if (len < 9)
		return;
	if (GET16(m + 1) != 0)
		return;
	remaddr = GET16(m + 3);

	/* retransmitted CI for a link we already know */
	for (k = 0; k < DN_MAXLINKS; k++) {
		l = &links[k];
		if (l->state != L_FREE && l->node == node && l->remaddr == remaddr) {
			if (l->state == L_CC)
				send_cc(l);
			return;
		}
	}

	memset(&ci, 0, sizeof ci);
	ci.node = node;
	i = 9;
	i += get_enduser(m + i, len - i, &ci.dstobj, ci.dstname);
	if (i < len)
		i += get_enduser(m + i, len - i, &ci.srcobj, ci.srcname);
	if (i < len) {
		dn_u8 menuver = m[i++];
		if (menuver & 0x01) {
			i += get_image(m + i, len - i, ci.user, sizeof ci.user);
			i += get_image(m + i, len - i, ci.password, sizeof ci.password);
			i += get_image(m + i, len - i, ci.account, sizeof ci.account);
		}
		if ((menuver & 0x02) && i < len) {
			unsigned c = m[i] <= 16 ? m[i] : 16;
			if (i + 1 + c <= len) {
				memcpy(ci.data, m + i + 1, c);
				ci.datalen = (dn_u8)c;
			}
		}
	}

	l = link_alloc();
	if (!l) {
		DN_LOG(DN_LOG_INFO, (DN_LOG_INFO, "connect from %s rejected: no free link", dn_ntoa(node)));
		send_dc(node, remaddr, 0, DN_R_RESOURCES);
		return;
	}
	l->node = node;
	l->remaddr = remaddr;
	l->remote_fc = (m[5] >> 2) & 3;
	l->segsize = GET16(m + 7);
	if (!l->segsize || l->segsize > DN_SEGSIZE)
		l->segsize = DN_SEGSIZE;
	l->state = L_CR;

	obj = dn_find_object(ci.dstobj, ci.dstname);
	DN_LOG(DN_LOG_INFO, (DN_LOG_INFO, "connect from %s to object %u%s%s user \"%s\" fc %d segsize %u: %s",
	       dn_ntoa(node), ci.dstobj, ci.dstname[0] ? " " : "", ci.dstname, ci.user,
	       l->remote_fc, l->segsize, obj ? "ok" : "no such object"));
	if (!obj) {
		dn_reject(l, DN_R_NOOBJECT, 0, 0);
		return;
	}
	l->obj = obj;
	obj->connect(l, &ci);
}

static void deliver(struct dn_link *l, const dn_u8 *p, unsigned len, dn_u8 flags)
{
	if (flags & MSG_BOM)
		l->rxlen = 0;
	if (l->rxlen + len > l->rxsize) {
		unsigned size = l->rxlen + len + 256;
		dn_u8 *nb = xalloc(size);
		if (!nb)
			return;
		if (l->rxbuf) {
			memcpy(nb, l->rxbuf, l->rxlen);
			xfree(l->rxbuf);
		}
		l->rxbuf = nb;
		l->rxsize = size;
	}
	memcpy(l->rxbuf + l->rxlen, p, len);
	l->rxlen += len;
	if ((flags & MSG_EOM) && l->obj && l->obj->data) {
		unsigned n = l->rxlen;
		l->rxlen = 0;
		l->obj->data(l, l->rxbuf, n);
	}
}

/* Keeps a segment that arrived ahead of rx_last + 1. */
static void rxq_add(struct dn_link *l, dn_u16 num, dn_u8 flags, const dn_u8 *p, unsigned len)
{
	struct rxseg *r;

	if (SEQ(num - l->rx_last) > RXWINDOW || l->rxqlen >= RXCACHE)
		return;
	for (r = l->rxq; r; r = r->next)
		if (r->num == num)
			return;                 /* have it already */
	r = xalloc(sizeof *r + len);
	if (!r)
		return;
	r->num = num;
	r->flags = flags;
	r->len = len;
	memcpy(r->data, p, len);
	r->next = l->rxq;
	l->rxq = r;
	l->rxqlen++;
}

static struct rxseg *rxq_take(struct dn_link *l, dn_u16 num)
{
	struct rxseg **pp, *r;

	for (pp = &l->rxq; (r = *pp) != 0; pp = &r->next)
		if (r->num == num) {
			*pp = r->next;
			l->rxqlen--;
			return r;
		}
	return 0;
}

/* Drops kept segments that are no longer ahead of rx_last. */
static void rxq_prune(struct dn_link *l)
{
	struct rxseg **pp, *r;

	for (pp = &l->rxq; (r = *pp) != 0; )
		if (!seq_lt(l->rx_last, r->num)) {
			*pp = r->next;
			l->rxqlen--;
			xfree(r);
		} else {
			pp = &r->next;
		}
}

static void handle_data(struct dn_link *l, dn_u8 flags, const dn_u8 *p, unsigned len)
{
	unsigned n = acks(l, p, len, 0);
	dn_u16 num, loc = l->locaddr;
	struct rxseg *r;

	if (n + 2 > len)
		return;
	num = SEQ(GET16(p + n));
	n += 2;
	l->ack_pending = 1;     /* in order: ack it; else (re)ack what we have */
	if (num == SEQ(l->rx_last + 1)) {
		l->rx_last = num;
		deliver(l, p + n, len - n, flags);
		/* segments that came early follow on now; the callback may
		   have closed the link */
		while (l->state == L_RUN && l->locaddr == loc &&
		       (r = rxq_take(l, SEQ(l->rx_last + 1))) != 0) {
			l->rx_last = r->num;
			deliver(l, r->data, r->len, r->flags);
			xfree(r);
		}
		if (l->state == L_RUN && l->locaddr == loc) {
			rxq_prune(l);
			/* still a hole further on: give it its own NAK delay */
			l->gap_since = l->rxq ? (dn_g.now ? dn_g.now : 1) : 0;
			l->nak_time = 0;
		}
	} else if (seq_lt(l->rx_last, num)) {
		/* ahead: a segment before it is missing (or late) */
		rxq_add(l, num, flags, p + n, len - n);
		if (!l->gap_since) {
			l->gap_since = dn_g.now ? dn_g.now : 1;
			l->nak_time = 0;
		}
	}
}

static void handle_other(struct dn_link *l, dn_u8 flags, const dn_u8 *p, unsigned len)
{
	unsigned n = acks(l, p, len, 1);
	dn_u16 num;

	if (n + 2 > len)
		return;
	num = SEQ(GET16(p + n));
	n += 2;
	l->oth_ack_pending = 1;
	if (num != SEQ(l->oth_rx_last + 1))
		return;                         /* duplicate: just re-ack */
	l->oth_rx_last = num;

	if (flags == MSG_LS) {
		dn_u8 lsflags, fcval;
		if (n + 2 > len)
			return;
		lsflags = p[n];
		fcval = p[n + 1];
		if (((lsflags >> 2) & 3) == 0) {        /* data request count */
			switch (lsflags & 3) {
			case 1: l->stopped = 1; break;
			case 2: l->stopped = 0; break;
			}
			l->flowcount += (signed char)fcval;
		}
		DN_LOG(DN_LOG_DEBUG, (DN_LOG_DEBUG, "link %04x: link service flags %02x fcval %d -> count %ld%s",
		       l->locaddr, lsflags, (signed char)fcval, l->flowcount, l->stopped ? " stopped" : ""));
	} else {
		DN_LOG(DN_LOG_INFO, (DN_LOG_INFO, "link %04x: interrupt message (%u bytes) ignored",
		       l->locaddr, len - n));
	}
}

void dn_nsp_input(dn_u16 node, const dn_u8 *m, unsigned len, int returned)
{
	dn_u8 flags;
	dn_u16 dstaddr, srcaddr;
	struct dn_link *l;

	if (len < 1)
		return;
	flags = m[0];

	if (returned) {
		/* our CI came back: destination unreachable */
		if ((flags == MSG_CI || flags == MSG_RCI) && len >= 5) {
			l = link_find(GET16(m + 3));
			if (l)
				link_gone(l, DN_R_UNREACHABLE);
		}
		return;
	}

	switch (flags) {
	case MSG_CI:
	case MSG_RCI:
		handle_ci(node, m, len);
		return;
	case MSG_NOP:
		return;
	case MSG_CONNACK:
		if (len >= 3 && (l = link_find(GET16(m + 1))) != 0 && l->state == L_CI) {
			l->ci_acked = 1;
			l->ctl_time = dn_g.now;
		}
		return;
	}
	if (len < 5)
		return;
	dstaddr = GET16(m + 1);
	srcaddr = GET16(m + 3);
	l = link_find(dstaddr);
	if (!l || l->node != node || (l->remaddr && l->remaddr != srcaddr)) {
		if (flags != MSG_DC)
			send_dc(node, srcaddr, dstaddr, DN_R_NOLINK);
		return;
	}

	switch (flags) {
	case MSG_DI: {
		dn_u16 reason = len >= 7 ? GET16(m + 5) : 0;
		send_dc(node, srcaddr, l->locaddr, DN_R_DISCCOMPLETE);
		DN_LOG(DN_LOG_INFO, (DN_LOG_INFO, "link %04x to %s: remote disconnect, reason %u",
		       l->locaddr, dn_ntoa(node), reason));
		link_gone(l, reason);
		return;
	}
	case MSG_DC: {
		dn_u16 reason = len >= 7 ? GET16(m + 5) : 0;
		DN_LOG(DN_LOG_INFO, (DN_LOG_INFO, "link %04x to %s: disconnect confirm, reason %u",
		       l->locaddr, dn_ntoa(node), reason));
		link_gone(l, l->state == L_DI ? l->disc_reason : reason);
		return;
	}
	}

	if (l->state == L_CI) {
		if (flags == MSG_CC && len >= 9) {
			unsigned dl = len >= 10 ? m[9] : 0;
			if (10 + dl > len)
				dl = len > 10 ? len - 10 : 0;
			l->remaddr = srcaddr;
			l->remote_fc = (m[5] >> 2) & 3;
			l->segsize = GET16(m + 7);
			if (!l->segsize || l->segsize > DN_SEGSIZE)
				l->segsize = DN_SEGSIZE;
			l->state = L_RUN;
			xfree(l->ci_msg);
			l->ci_msg = 0;
			DN_LOG(DN_LOG_INFO, (DN_LOG_INFO, "link %04x to %s: connected, fc %d segsize %u",
			       l->locaddr, dn_ntoa(node), l->remote_fc, l->segsize));
			send_data_ack(l);       /* acknowledges the CC */
			if (l->obj && l->obj->connected)
				l->obj->connected(l, m + 10, dl);
		}
		return;
	}
	if (l->state == L_DI || l->state == L_CR)
		return;
	if (l->state == L_CC)
		l->state = L_RUN;               /* anything on the link confirms CC */

	switch (flags) {
	case MSG_DATA:
	case MSG_DATA | MSG_BOM:
	case MSG_DATA | MSG_EOM:
	case MSG_DATA | MSG_BOM | MSG_EOM:
		handle_data(l, flags, m + 5, len - 5);
		break;
	case MSG_LS:
	case MSG_INT:
		handle_other(l, flags, m + 5, len - 5);
		break;
	case MSG_DATAACK:
		acks(l, m + 5, len - 5, 0);
		break;
	case MSG_OTHACK:
		acks(l, m + 5, len - 5, 1);
		break;
	default:
		DN_LOG(DN_LOG_DEBUG, (DN_LOG_DEBUG, "link %04x: unknown NSP message %02x", l->locaddr, flags));
		return;
	}

	/* The callback may have freed or disconnected the link. */
	if (l->state == L_RUN) {
		push(l);
		if (l->ack_pending)
			send_data_ack(l);
		if (l->oth_ack_pending)
			send_oth_ack(l);
	}
}

/* ---- timers -------------------------------------------------------------------- */

void dn_nsp_tick(void)
{
	int i;

	for (i = 0; i < DN_MAXLINKS; i++) {
		struct dn_link *l = &links[i];
		struct seg *s;

		switch (l->state) {
		case L_CI:
			/* after a connect ack the remote object is starting: wait
			   up to a minute for the confirm, else retransmit */
			if (dn_g.now - l->ctl_time < (l->ci_acked ? 60000UL : l->rto))
				break;
			if (l->ci_acked || ++l->ctl_retries > MAXCTLRETRY) {
				DN_LOG(DN_LOG_INFO, (DN_LOG_INFO, "link %04x to %s: connect timed out",
				       l->locaddr, dn_ntoa(l->node)));
				link_gone(l, DN_R_UNREACHABLE);
				break;
			}
			l->ci_msg[0] = MSG_RCI;
			dn_route_send(l->node, l->ci_msg, l->ci_len, 1);
			l->ctl_time = dn_g.now;
			break;
		case L_CC:
		case L_DI:
			if (dn_g.now - l->ctl_time < l->rto)
				break;
			if (++l->ctl_retries > MAXCTLRETRY) {
				DN_LOG(DN_LOG_INFO, (DN_LOG_INFO, "link %04x to %s: no answer, dropped",
				       l->locaddr, dn_ntoa(l->node)));
				link_gone(l, l->state == L_DI ? l->disc_reason : DN_R_UNREACHABLE);
				break;
			}
			if (l->state == L_CC)
				send_cc(l);
			else
				send_di(l);
			break;
		case L_RUN:
			/* a gap that the next segments did not fill: ask for a
			   retransmission now instead of waiting for the sender's
			   timeout */
			if (l->gap_since && dn_g.now - l->gap_since >= NAK_MS &&
			    (!l->nak_time || dn_g.now - l->nak_time >= NAK_REPEAT_MS)) {
				DN_LOG(DN_LOG_DEBUG, (DN_LOG_DEBUG, "link %04x: segment %u missing, NAK",
				       l->locaddr, SEQ(l->rx_last + 1)));
				send_data_nak(l);
			}
			s = l->txq;
			if (s && s->txcount && dn_g.now - s->sent >= l->rto) {
				if (s->txcount > MAXRETRY) {
					DN_LOG(DN_LOG_INFO, (DN_LOG_INFO, "link %04x to %s: retransmit limit, link lost",
					       l->locaddr, dn_ntoa(l->node)));
					link_gone(l, DN_R_UNREACHABLE);
					break;
				}
				l->rto = l->rto * 2 > RTO_MAX ? RTO_MAX : l->rto * 2;
				DN_LOG(DN_LOG_DEBUG, (DN_LOG_DEBUG, "link %04x: retransmit segment %u", l->locaddr, s->num));
				send_segment(l, s);
			}
			push(l);
			break;
		}
	}
}

void dn_nsp_shutdown(void)
{
	int i;

	for (i = 0; i < DN_MAXLINKS; i++) {
		struct dn_link *l = &links[i];
		if (l->state == L_RUN || l->state == L_CC) {
			l->disc_reason = 11;    /* local node shutdown */
			l->ctl_datalen = 0;
			send_di(l);
		}
		if (l->state != L_FREE)
			link_gone(l, 11);
	}
}

/* ---- link API ------------------------------------------------------------------ */

static unsigned put_image(dn_u8 *p, const char *s, unsigned max)
{
	unsigned n = s ? (unsigned)strlen(s) : 0;

	if (n > max)
		n = max;
	p[0] = (dn_u8)n;
	memcpy(p + 1, s, n);
	return n + 1;
}

struct dn_link *dn_connect(const struct dn_connreq *req, struct dn_object *handler)
{
	struct dn_link *l = link_alloc();
	dn_u8 *m;
	unsigned n = 0;
	int access = (req->user && req->user[0]) || (req->password && req->password[0]) ||
	             (req->account && req->account[0]);

	if (!l)
		return 0;
	m = xalloc(200);
	if (!m) {
		link_free(l);
		return 0;
	}
	l->node = req->node;
	l->obj = handler;
	l->segsize = DN_SEGSIZE;

	m[n++] = MSG_CI;
	PUT16(m + n, 0); n += 2;                /* DSTADDR */
	PUT16(m + n, l->locaddr); n += 2;       /* SRCADDR */
	m[n++] = 0x01 | (FC_NONE << 2);         /* SERVICES */
	m[n++] = 0x03;                          /* INFO: NSP 4.1 */
	PUT16(m + n, DN_SEGSIZE); n += 2;
	/* DSTNAME */
	if (req->objnum) {
		m[n++] = 0;
		m[n++] = req->objnum;
	} else {
		m[n++] = 1;
		m[n++] = 0;
		n += put_image(m + n, req->objname, 16);
	}
	/* SRCNAME: format 1, object 0, task name */
	m[n++] = 1;
	m[n++] = 0;
	n += put_image(m + n, req->srcname && req->srcname[0] ? req->srcname : "DECNET", 16);
	/* MENUVER */
	m[n++] = (dn_u8)((access ? 0x01 : 0) | (req->datalen ? 0x02 : 0));
	if (access) {
		n += put_image(m + n, req->user, 39);
		n += put_image(m + n, req->password, 39);
		n += put_image(m + n, req->account, 39);
	}
	if (req->datalen) {
		unsigned c = req->datalen > 16 ? 16 : req->datalen;
		m[n++] = (dn_u8)c;
		memcpy(m + n, req->data, c);
		n += c;
	}
	l->ci_msg = m;
	l->ci_len = n;
	l->state = L_CI;
	l->ctl_time = dn_g.now;
	DN_LOG(DN_LOG_INFO, (DN_LOG_INFO, "link %04x: connecting to %s object %u%s%s",
	       l->locaddr, dn_ntoa(req->node), req->objnum, req->objnum ? "" : " ",
	       req->objnum ? "" : (req->objname ? req->objname : "")));
	dn_route_send(l->node, m, n, 1);
	return l;
}

int dn_accept(struct dn_link *l, const dn_u8 *data, unsigned len)
{
	if (l->state != L_CR)
		return -1;
	if (len > 16)
		len = 16;
	memcpy(l->ctl_data, data, len);
	l->ctl_datalen = (dn_u8)len;
	l->state = L_CC;
	l->ctl_retries = 0;
	send_cc(l);
	return 0;
}

int dn_reject(struct dn_link *l, dn_u16 reason, const dn_u8 *data, unsigned len)
{
	if (l->state != L_CR)
		return -1;
	if (len > 16)
		len = 16;
	memcpy(l->ctl_data, data, len);
	l->ctl_datalen = (dn_u8)len;
	l->disc_reason = reason;
	l->state = L_DI;
	l->ctl_retries = 0;
	send_di(l);
	return 0;
}

int dn_send(struct dn_link *l, const dn_u8 *buf, unsigned len)
{
	struct seg **tail;
	unsigned off = 0;

	if (l->state != L_RUN && l->state != L_CC)
		return -1;
	for (tail = &l->txq; *tail; tail = &(*tail)->next)
		;
	do {
		unsigned n = len - off > l->segsize ? l->segsize : len - off;
		struct seg *s = xalloc(sizeof(struct seg) + n);
		if (!s)
			return -1;
		memset(s, 0, sizeof *s);
		s->num = l->tx_next;
		l->tx_next = SEQ(l->tx_next + 1);
		s->flags = (dn_u8)((off == 0 ? MSG_BOM : 0) | (off + n == len ? MSG_EOM : 0));
		s->len = n;
		memcpy(s->data, buf + off, n);
		*tail = s;
		tail = &s->next;
		off += n;
	} while (off < len);
	push(l);
	return 0;
}

int dn_disconnect(struct dn_link *l, dn_u16 reason, const dn_u8 *data, unsigned len)
{
	if (l->state == L_CR)
		return dn_reject(l, reason, data, len);
	if (l->state == L_CI) {
		/* not connected yet: just forget it (a late CC gets "no link") */
		link_gone(l, reason);
		return 0;
	}
	if (l->state != L_RUN && l->state != L_CC)
		return -1;
	if (len > 16)
		len = 16;
	memcpy(l->ctl_data, data, len);
	l->ctl_datalen = (dn_u8)len;
	l->disc_reason = reason;
	l->state = L_DI;
	l->ctl_retries = 0;
	send_di(l);
	return 0;
}

void dn_set_user(struct dn_link *l, void *p) { l->user = p; }
void *dn_get_user(struct dn_link *l) { return l->user; }

/* Segments queued and not yet acknowledged. */
unsigned dn_pending(struct dn_link *l)
{
	struct seg *s;
	unsigned n = 0;

	for (s = l->txq; s; s = s->next)
		n++;
	return n;
}
dn_u16 dn_link_node(struct dn_link *l) { return l->node; }

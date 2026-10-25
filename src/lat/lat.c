/*
 * lat.c - portable LAT terminal server side, see lat.h.
 *
 * A virtual circuit runs between us (master) and one node (slave). The
 * master sends a run message whenever it has something to say or must
 * answer; the slave answers every one of them. One message may be
 * outstanding (sequence/ack numbers, 8 bit); a lost one is sent again.
 * Sessions live in slots of the run messages: start (0x9x), data
 * (0x0x), data_b (0xax: port settings, BREAK), reject/stop (0xcx/0xdx).
 * The low nibble of a slot command grants credits: the number of data
 * slots the other side may send on that session.
 *
 * As host (lat_offer) the roles turn round: a terminal server opens the
 * circuit (start, we acknowledge), sends the run messages and starts the
 * sessions with start slots; we answer every run message (empty, 0x00)
 * and send our output in messages that ask for an answer (0x01), one at
 * a time. The service is announced every LAT_ANNOUNCE_S seconds.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "lat.h"

/* message types (first byte; bit 0 response requested, bit 1 to host) */
#define M_RUN           0x00
#define M_START         0x04
#define M_STOP          0x08
#define M_ANNOUNCE      0x28
#define TO_HOST         0x02

/* slot types (high nibble) */
#define SL_DATA         0x00
#define SL_START        0x90
#define SL_DATAB        0xa0
#define SL_ATTN         0xb0
#define SL_REJECT       0xc0
#define SL_STOP         0xd0

#define MAXMSG          1500
#define CIRC_TIMER      8       /* 80 ms, as we tell the node */
#define KEEPALIVE       20      /* s */
#define START_RETRY_MS  1500
#define START_TRIES     4
#define RETRANS_MS      250
#define RETRANS_LIMIT   16
#define GRANT_LOW       4       /* grant more when the node has this few */
#define MAX_CREDIT      15

/* session states */
#define S_FREE          0
#define S_WAIT          1       /* start slot not yet sent */
#define S_STARTING      2       /* start slot sent, no answer yet */
#define S_RUN           3
#define S_STOP          4       /* stop slot to send */
#define S_OFFERED       5       /* host: waits for lat_answer */
#define S_ACCEPT        6       /* host: start slot (accept) to send */
#define S_REJECT        7       /* host: reject slot to send */

/* circuit states */
#define C_FREE          0
#define C_START         1       /* start message sent */
#define C_RUN           2

struct sess {
	int state;
	unsigned char rid;              /* the node's slot id */
	int credit;                     /* data slots we may send */
	int rcredit;                    /* data slots the node may send */
	int brk;                        /* BREAK to send */
	unsigned maxdata;               /* data slot size the other side takes */
	unsigned char tx[LAT_TXBUF];
	unsigned txlen;
	char service[LAT_NAMELEN + 1];
	char port[LAT_NAMELEN + 1];
	int reason;                     /* S_REJECT */
};

struct circ {
	int state;
	int host;                       /* we are the host (slave) on it */
	int had_session;                /* host: a session was started */
	unsigned long rx_idle;          /* host: ms since the node last sent */
	unsigned char mac[6];
	char node[LAT_NAMELEN + 1];
	unsigned lid, rid;              /* circuit ids: ours, the node's */
	unsigned char seq;              /* last sequence number sent */
	unsigned char rseq;             /* last one received */
	int need_ack;                   /* last[] not yet acknowledged */
	int reply_due;                  /* the node expects an answer */
	unsigned char last[MAXMSG];
	unsigned lastlen;
	unsigned long timer;            /* ms since the start or last send */
	unsigned long idle;             /* ms since we last sent */
	int tries;
	struct sess s[LAT_MAXSESS];
};

struct svc {
	int used;
	struct lat_service s;
	unsigned long life_ms;          /* forget it when age_ms exceeds */
};

static struct lat_env env;
static struct svc svcs[LAT_MAXSVC];
static struct circ circs[LAT_MAXCIRC];
static unsigned next_lid;

/* host role */
static int offering;
static char offer_name[LAT_NAMELEN + 1];
static char offer_ident[64];
static unsigned long announce_ms;
static unsigned char incarnation;
static const unsigned char lat_multicast[6] = { 0x09, 0x00, 0x2b, 0x00, 0x00, 0x0f };

static void logf_(int level, const char *fmt, ...)
{
	char buf[200];
	va_list ap;

	if (!env.log || env.verbose < level)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	env.log(env.ctx, buf);
}

static unsigned get16(const unsigned char *p)
{
	return p[0] | (p[1] << 8);
}

static void put16(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static int upcase(int c)
{
	return c >= 'a' && c <= 'z' ? c - 32 : c;
}

static int name_eq(const char *a, const char *b)
{
	while (*a && upcase(*a) == upcase(*b))
		a++, b++;
	return *a == 0 && *b == 0;
}

/* counted string at p (max: room in the message); returns its end */
static unsigned get_ascic(const unsigned char *m, unsigned p, unsigned len,
                          char *out, unsigned outsize)
{
	unsigned n, k;

	if (p >= len) {
		out[0] = 0;
		return len;
	}
	n = m[p++];
	if (p + n > len)
		n = len - p;
	k = n < outsize - 1 ? n : outsize - 1;
	memcpy(out, m + p, k);
	out[k] = 0;
	return p + n;
}

static unsigned put_ascic(unsigned char *m, unsigned p, const char *s)
{
	unsigned n = (unsigned)strlen(s);

	if (n > 255)
		n = 255;
	m[p++] = (unsigned char)n;
	memcpy(m + p, s, n);
	return p + n;
}

/* ---- services ------------------------------------------------------------ */

static void add_service(const unsigned char *mac, const char *node, const char *name,
                        const char *ident, int rating, unsigned timer)
{
	struct svc *v, *free_ = NULL, *oldest = NULL;
	int i;

	for (i = 0; i < LAT_MAXSVC; i++) {
		v = &svcs[i];
		if (!v->used) {
			if (!free_)
				free_ = v;
			continue;
		}
		if (name_eq(v->s.name, name) && name_eq(v->s.node, node))
			break;
		if (!oldest || v->s.age_ms > oldest->s.age_ms)
			oldest = v;
	}
	if (i == LAT_MAXSVC) {
		v = free_ ? free_ : oldest;
		if (!v)
			return;
		memset(v, 0, sizeof *v);
		v->used = 1;
		snprintf(v->s.name, sizeof v->s.name, "%s", name);
		snprintf(v->s.node, sizeof v->s.node, "%s", node);
		logf_(1, "service %s on node %s (rating %d)", name, node, rating);
	}
	memcpy(v->s.mac, mac, 6);
	snprintf(v->s.ident, sizeof v->s.ident, "%s", ident);
	v->s.rating = rating;
	v->s.age_ms = 0;
	v->life_ms = (timer ? timer : 60) * 3000UL + 5000;
}

static void drop_node_services(const char *node)
{
	int i;

	for (i = 0; i < LAT_MAXSVC; i++)
		if (svcs[i].used && name_eq(svcs[i].s.node, node))
			svcs[i].used = 0;
}

/* service announcement: header, groups, node, greeting, services */
static void do_announce(const unsigned char *src, const unsigned char *m, unsigned len)
{
	char node[LAT_NAMELEN + 1], greeting[80], name[LAT_NAMELEN + 1], ident[64];
	unsigned p, n, i, timer;
	int rating;

	if (len < 13)
		return;
	timer = m[10];
	p = 13 + m[12];                 /* past the group mask */
	p = get_ascic(m, p, len, node, sizeof node);
	p = get_ascic(m, p, len, greeting, sizeof greeting);
	if (p >= len || !node[0])
		return;
	if (m[11] & 1) {                /* node not accepting connections */
		drop_node_services(node);
		return;
	}
	n = m[p++];
	for (i = 0; i < n && p < len; i++) {
		rating = m[p++];
		p = get_ascic(m, p, len, name, sizeof name);
		p = get_ascic(m, p, len, ident, sizeof ident);
		if (name[0])
			add_service(src, node, name, ident, rating, timer);
	}
}

const struct lat_service *lat_service(int i)
{
	int k;

	for (k = 0; k < LAT_MAXSVC; k++)
		if (svcs[k].used && i-- == 0)
			return &svcs[k].s;
	return NULL;
}

const struct lat_service *lat_find_service(const char *name)
{
	const struct lat_service *best = NULL;
	int k;

	for (k = 0; k < LAT_MAXSVC; k++)
		if (svcs[k].used && name_eq(svcs[k].s.name, name) &&
		    (!best || svcs[k].s.rating > best->rating))
			best = &svcs[k].s;
	return best;
}

/* ---- circuits ------------------------------------------------------------ */

static void send_raw(struct circ *c, const unsigned char *m, unsigned len)
{
	env.send_frame(env.ctx, c->mac, m, len);
}

static void session_down(struct circ *c, int si, int reason)
{
	struct sess *s = &c->s[si];

	if (s->state == S_FREE)
		return;
	s->state = S_FREE;
	logf_(1, "session %d down: %s", (int)(c - circs) * LAT_MAXSESS + si,
	      lat_reason_text(reason));
	env.event(env.ctx, (int)(c - circs) * LAT_MAXSESS + si, LAT_EV_DOWN, reason);
}

static void circuit_down(struct circ *c, int reason)
{
	int i;

	for (i = 0; i < LAT_MAXSESS; i++)
		session_down(c, i, reason);
	logf_(1, "circuit to %s closed", c->node);
	c->state = C_FREE;
}

static int sessions_left(struct circ *c)
{
	int i, n = 0;

	for (i = 0; i < LAT_MAXSESS; i++)
		if (c->s[i].state != S_FREE)
			n++;
	return n;
}

/* start message: our parameters and names */
static void send_start(struct circ *c)
{
	unsigned char m[200];
	unsigned p = 8;

	memset(m, 0, sizeof m);
	m[0] = M_START | TO_HOST;
	m[1] = 0;
	put16(m + 2, 0);
	put16(m + 4, c->lid);
	m[6] = 0;                       /* sequence */
	m[7] = 0xff;                    /* ack: nothing yet */
	put16(m + p, MAXMSG);
	p += 2;
	m[p++] = 5;                     /* LAT 5.2 */
	m[p++] = 2;
	m[p++] = LAT_MAXSESS;
	m[p++] = 0;                     /* extra buffers */
	m[p++] = CIRC_TIMER;
	m[p++] = KEEPALIVE;
	put16(m + p, 0);                /* facility */
	p += 2;
	m[p++] = 3;                     /* product type and version, as latd */
	m[p++] = 3;
	p = put_ascic(m, p, c->node);
	p = put_ascic(m, p, env.node);
	p = put_ascic(m, p, env.ident);
	m[p++] = 0;
	send_raw(c, m, p);
	c->timer = 0;
	logf_(2, "start -> %s (circuit %04x)", c->node, c->lid);
}

/* stop message: no ack expected, the circuit is gone */
static void send_stop(struct circ *c, int reason)
{
	unsigned char m[12];

	memset(m, 0, sizeof m);
	m[0] = M_STOP | TO_HOST;
	put16(m + 2, c->rid);
	put16(m + 4, 0);
	m[6] = ++c->seq;
	m[7] = c->rseq;
	m[8] = (unsigned char)reason;
	send_raw(c, m, 9);
}

/* append one slot */
static unsigned add_slot(unsigned char *m, unsigned p, unsigned char dst,
                         unsigned char src, unsigned char cmd,
                         const unsigned char *data, unsigned len)
{
	m[p++] = dst;
	m[p++] = src;
	m[p++] = (unsigned char)len;
	m[p++] = cmd;
	if (len)
		memcpy(m + p, data, len);
	p += len;
	if (p & 1)
		m[p++] = 0;
	m[1]++;
	return p;
}

/* credits to hand out with the next slot of a session */
static int grant(struct sess *s)
{
	int g;

	if (s->rcredit > GRANT_LOW)
		return 0;
	g = MAX_CREDIT - s->rcredit;
	s->rcredit += g;
	return g;
}

/* build and send the next run message if we may (nothing outstanding)
   and have something to say; force: send even an empty one */
static void circ_pump(struct circ *c, int force)
{
	unsigned char m[MAXMSG];
	unsigned p = 8, n;
	int i, g, any = 0;
	struct sess *s;

	if (c->state != C_RUN || c->need_ack)
		return;
	memset(m, 0, 8);
	m[0] = c->host ? M_RUN : M_RUN | TO_HOST;
	for (i = 0; i < LAT_MAXSESS && p < MAXMSG - 300; i++) {
		s = &c->s[i];
		switch (s->state) {
		case S_ACCEPT: {
			/* host: our start slot answers the terminal server's */
			unsigned char b[40];
			unsigned q = 0;

			b[q++] = 1;             /* service class */
			b[q++] = 1;             /* attention slot size */
			b[q++] = 0xfe;          /* data slot size */
			b[q++] = 0;             /* no service, no port name */
			b[q++] = 0;
			b[q++] = 1;             /* parameter 1, 2 bytes: 1024 */
			b[q++] = 2;
			b[q++] = 4;
			b[q++] = 0;
			b[q++] = 4;             /* parameter 4: our port name */
			q = put_ascic(b, q, offer_name);
			b[q++] = 0;
			s->rcredit = MAX_CREDIT;
			p = add_slot(m, p, s->rid, (unsigned char)(i + 1), SL_START | MAX_CREDIT, b, q);
			s->state = S_RUN;
			any = 1;
			break;
		}
		case S_REJECT:
			p = add_slot(m, p, s->rid, 0, (unsigned char)(SL_REJECT | (s->reason & 15)),
			             NULL, 0);
			s->state = S_FREE;
			any = 1;
			break;
		case S_WAIT: {
			unsigned char b[60];
			unsigned q = 0;

			b[q++] = 1;             /* service class: interactive */
			b[q++] = 1;             /* attention slot size */
			b[q++] = 0xfe;          /* data slot size */
			q = put_ascic(b, q, s->service);
			b[q++] = 0;             /* source service */
			b[q++] = 1;             /* parameter 1, 2 bytes: 1024 */
			b[q++] = 2;
			b[q++] = 4;
			b[q++] = 0;
			b[q++] = 5;             /* parameter 5: our port name */
			q = put_ascic(b, q, s->port);
			s->rcredit = MAX_CREDIT;
			p = add_slot(m, p, 0, (unsigned char)(i + 1), SL_START | MAX_CREDIT, b, q);
			s->state = S_STARTING;
			any = 1;
			break;
		}
		case S_RUN:
			if (s->brk && s->credit > 0) {
				unsigned char b = 0x10;     /* BREAK */

				p = add_slot(m, p, s->rid, (unsigned char)(i + 1),
				             (unsigned char)(SL_DATAB | grant(s)), &b, 1);
				s->brk = 0;
				s->credit--;
				any = 1;
			}
			if (s->txlen && s->credit > 0) {
				n = s->txlen < s->maxdata ? s->txlen : s->maxdata;
				p = add_slot(m, p, s->rid, (unsigned char)(i + 1),
				             (unsigned char)(SL_DATA | grant(s)), s->tx, n);
				memmove(s->tx, s->tx + n, s->txlen - n);
				s->txlen -= n;
				s->credit--;
				any = 1;
			} else if ((g = grant(s)) > 0) {
				p = add_slot(m, p, s->rid, (unsigned char)(i + 1),
				             (unsigned char)(SL_DATA | g), NULL, 0);
				any = 1;
			}
			break;
		case S_STOP:
			/* source slot 0: some hosts reject the final stop
			   otherwise (latd) */
			p = add_slot(m, p, s->rid, 0, SL_STOP | LAT_R_USER, NULL, 0);
			session_down(c, i, LAT_R_USER);
			any = 1;
			break;
		}
	}
	if (!any && !force && !c->reply_due)
		return;
	if (c->host && any)
		m[0] |= 1;                      /* answer this one */
	put16(m + 2, c->rid);
	put16(m + 4, c->lid);
	m[6] = ++c->seq;
	m[7] = c->rseq;
	memcpy(c->last, m, p);
	c->lastlen = p;
	/* the master needs an answer to each message, the host only to
	   those that carry slots */
	c->need_ack = !c->host || any;
	c->reply_due = 0;
	c->timer = 0;
	c->idle = 0;
	c->tries = 0;
	send_raw(c, m, p);
	logf_(3, "run -> %s seq %u ack %u, %u slots", c->node, m[6], m[7], m[1]);
}

static void resend_last(struct circ *c)
{
	if (!c->lastlen)
		return;
	c->last[7] = c->rseq;
	c->timer = 0;
	send_raw(c, c->last, c->lastlen);
}

static struct circ *find_circ(const unsigned char *src, unsigned lid)
{
	int i;

	for (i = 0; i < LAT_MAXCIRC; i++)
		if (circs[i].state != C_FREE && circs[i].lid == lid &&
		    !memcmp(circs[i].mac, src, 6))
			return &circs[i];
	return NULL;
}

/* host: a terminal server starts a session (start slot to slot 0):
   service class, slot sizes, service, source port, parameters */
static void host_session(struct circ *c, const unsigned char *b, unsigned n,
                         unsigned char rid, unsigned credits)
{
	char service[LAT_NAMELEN + 1], port[LAT_NAMELEN + 1], skip[LAT_NAMELEN + 1];
	unsigned q = 3, type, l;
	struct sess *s;
	int i;

	port[0] = 0;
	q = get_ascic(b, q, n, service, sizeof service);
	q = get_ascic(b, q, n, skip, sizeof skip);
	while (q + 1 < n && (type = b[q]) != 0) {
		l = b[q + 1];
		if (type == 5)
			get_ascic(b, q + 1, n, port, sizeof port);
		q += 2 + l;
	}
	for (i = 0; i < LAT_MAXSESS && c->s[i].state != S_FREE; i++)
		;
	if (i == LAT_MAXSESS)
		return;                         /* nothing to answer with */
	s = &c->s[i];
	memset(s, 0, sizeof *s);
	s->rid = rid;
	if (!offering || !name_eq(service, offer_name)) {
		logf_(1, "%s asks for service %s: not ours", c->node, service);
		s->state = S_REJECT;
		s->reason = LAT_R_NOSVC;
		return;
	}
	s->credit = (int)credits;
	s->maxdata = n > 2 && b[2] ? b[2] : 0xfe;
	snprintf(s->service, sizeof s->service, "%s", service);
	snprintf(s->port, sizeof s->port, "%s", port);
	c->had_session = 1;
	logf_(1, "session %d from %s/%s", (int)(c - circs) * LAT_MAXSESS + i, c->node, port);
	if (env.incoming) {
		s->state = S_OFFERED;
		env.incoming(env.ctx, (int)(c - circs) * LAT_MAXSESS + i, c->node, port);
	} else
		s->state = S_ACCEPT;
}

static void do_slots(struct circ *c, const unsigned char *m, unsigned len)
{
	unsigned p = 8, i, n, cmd, credits;
	struct sess *s;
	int si;

	for (i = 0; i < m[1] && p + 4 <= len; i++) {
		n = m[p + 2];
		cmd = m[p + 3];
		credits = cmd & 0x0f;
		si = m[p] - 1;
		s = si >= 0 && si < LAT_MAXSESS && c->s[si].state != S_FREE ? &c->s[si] : NULL;
		if (p + 4 + n > len)
			n = len - p - 4;
		switch (cmd & 0xf0) {
		case SL_DATA:
			if (!s)
				break;
			s->credit += credits;
			if (n) {
				s->rcredit--;
				env.data(env.ctx, (int)(c - circs) * LAT_MAXSESS + si, m + p + 4, n);
			}
			break;
		case SL_START:
			if (c->host && m[p] == 0) {
				host_session(c, m + p + 4, n, m[p + 1], credits);
				break;
			}
			if (s && s->state == S_STARTING) {
				s->rid = m[p + 1];
				s->credit += credits;
				s->maxdata = n > 2 && m[p + 6] ? m[p + 6] : 0xfe;
				s->state = S_RUN;
				logf_(1, "session %d up on %s", (int)(c - circs) * LAT_MAXSESS + si, c->node);
				env.event(env.ctx, (int)(c - circs) * LAT_MAXSESS + si, LAT_EV_UP, 0);
			}
			break;
		case SL_DATAB:
			/* port settings from the host: counts as a data slot */
			if (s) {
				s->credit += credits;
				s->rcredit--;
			}
			break;
		case SL_ATTN:
			break;
		case SL_REJECT:
		case SL_STOP:
			/* low nibble: the reason */
			if (s)
				session_down(c, si, (int)credits);
			break;
		}
		p += 4 + n;
		if (p & 1)
			p++;
	}
}

static void do_run(const unsigned char *src, const unsigned char *m, unsigned len)
{
	struct circ *c;

	if (len < 8)
		return;
	c = find_circ(src, get16(m + 2));
	if (!c || c->state != C_RUN || c->host != ((m[0] & TO_HOST) != 0))
		return;
	c->rx_idle = 0;
	if (m[6] == c->rseq) {
		/* the node did not see our answer: send it again */
		logf_(2, "duplicate seq %u from %s", m[6], c->node);
		resend_last(c);
		return;
	}
	c->rseq = m[6];
	if (m[7] == c->seq)
		c->need_ack = 0;
	else if (c->need_ack) {
		/* acks an older message: ours got lost */
		resend_last(c);
	}
	do_slots(c, m, len);
	if (c->host) {
		/* the terminal server ends the circuit itself; answer
		   every message */
		c->reply_due = 1;
		circ_pump(c, 0);
		return;
	}
	if (!sessions_left(c)) {
		/* the last session is over: end the circuit now. Answering
		   instead would go on for ever, as VMS keeps asking for
		   answers on an idle circuit */
		send_stop(c, 1);
		circuit_down(c, LAT_R_USER);
		return;
	}
	if ((m[0] & 1) || m[1])
		c->reply_due = 1;
	circ_pump(c, 0);
}

/* host: start message of a terminal server: header, parameters, our
   node name, its node name, its description */
static void send_start_ack(struct circ *c)
{
	unsigned char m[200];
	unsigned p = 8;

	memset(m, 0, sizeof m);
	m[0] = M_START;
	put16(m + 2, c->rid);
	put16(m + 4, c->lid);
	m[6] = c->seq;
	m[7] = c->rseq;
	put16(m + p, MAXMSG);
	p += 2;
	m[p++] = 5;
	m[p++] = 2;
	m[p++] = LAT_MAXSESS;
	m[p++] = 0;
	m[p++] = CIRC_TIMER;
	m[p++] = KEEPALIVE;
	put16(m + p, 0);
	p += 2;
	m[p++] = 3;
	m[p++] = 3;
	p = put_ascic(m, p, env.node);
	p = put_ascic(m, p, c->node);
	p = put_ascic(m, p, env.ident);
	m[p++] = 0;
	memcpy(c->last, m, p);
	c->lastlen = p;
	c->need_ack = 1;
	c->timer = 0;
	c->tries = 0;
	send_raw(c, m, p);
}

static void do_host_start(const unsigned char *src, const unsigned char *m, unsigned len)
{
	char dest[LAT_NAMELEN + 1], node[LAT_NAMELEN + 1];
	struct circ *c;
	unsigned p;
	int i;

	if (len < 21 || !offering)
		return;
	p = get_ascic(m, 20, len, dest, sizeof dest);
	get_ascic(m, p, len, node, sizeof node);
	if (!name_eq(dest, env.node))
		return;
	for (i = 0; i < LAT_MAXCIRC; i++) {
		c = &circs[i];
		if (c->state != C_FREE && c->host && !memcmp(c->mac, src, 6) &&
		    c->rid == get16(m + 4)) {
			resend_last(c);         /* our acknowledgement got lost */
			return;
		}
	}
	for (i = 0; i < LAT_MAXCIRC && circs[i].state != C_FREE; i++)
		;
	if (i == LAT_MAXCIRC) {
		logf_(1, "no free circuit for %s", node);
		return;
	}
	c = &circs[i];
	memset(c, 0, sizeof *c);
	c->state = C_RUN;
	c->host = 1;
	memcpy(c->mac, src, 6);
	snprintf(c->node, sizeof c->node, "%s", node);
	if (++next_lid == 0)
		next_lid = 1;
	c->lid = next_lid;
	c->rid = get16(m + 4);
	c->rseq = m[6];
	c->seq = 0;
	logf_(1, "circuit from %s (%04x/%04x)", c->node, c->lid, c->rid);
	send_start_ack(c);
}

static void do_start(const unsigned char *src, const unsigned char *m, unsigned len)
{
	struct circ *c;

	if (len < 8)
		return;
	c = find_circ(src, get16(m + 2));
	if (!c || c->state != C_START)
		return;
	c->rid = get16(m + 4);
	c->rseq = m[6];
	c->seq = 0;
	c->state = C_RUN;
	c->need_ack = 0;
	c->idle = 0;
	logf_(1, "circuit to %s up (%04x/%04x)", c->node, c->lid, c->rid);
	circ_pump(c, 0);
}

static void do_stop(const unsigned char *src, const unsigned char *m, unsigned len)
{
	struct circ *c;

	if (len < 9)
		return;
	c = find_circ(src, get16(m + 2));
	if (!c)
		return;
	logf_(1, "%s stopped the circuit (reason %u)", c->node, m[8]);
	circuit_down(c, LAT_R_CIRCUIT);
}

void lat_input(const unsigned char *src, const unsigned char *dst,
               const unsigned char *m, unsigned len)
{
	(void)dst;
	if (len < 2 || !memcmp(src, env.mac, 6))
		return;
	if (m[0] & TO_HOST) {
		/* from a terminal server: for us as host */
		switch (m[0] & 0xfc) {
		case M_RUN:
			do_run(src, m, len);
			break;
		case M_START:
			do_host_start(src, m, len);
			break;
		case M_STOP:
			do_stop(src, m, len);
			break;
		}
		return;
	}
	switch (m[0] & 0xfc) {
	case M_RUN:
		do_run(src, m, len);
		break;
	case M_START:
		do_start(src, m, len);
		break;
	case M_STOP:
		do_stop(src, m, len);
		break;
	case M_ANNOUNCE:
		do_announce(src, m, len);
		break;
	}
}

/* host: announce our service (as latd does) */
static void send_announce(void)
{
	unsigned char m[200];
	unsigned p = 0;

	m[p++] = M_ANNOUNCE;
	m[p++] = CIRC_TIMER;
	m[p++] = 5;                     /* protocol versions 5 to 5 */
	m[p++] = 5;
	m[p++] = 5;                     /* LAT 5.2 */
	m[p++] = 2;
	m[p++] = --incarnation;
	m[p++] = 0x1f;                  /* change flags */
	put16(m + p, MAXMSG);
	p += 2;
	m[p++] = LAT_ANNOUNCE_S;
	m[p++] = 2;                     /* accepting connections */
	m[p++] = 1;                     /* group mask: group 0 */
	m[p++] = 1;
	p = put_ascic(m, p, env.node);
	p = put_ascic(m, p, env.ident);
	m[p++] = 1;                     /* one service */
	m[p++] = 100;                   /* rating */
	p = put_ascic(m, p, offer_name);
	p = put_ascic(m, p, offer_ident);
	m[p++] = 1;                     /* node service classes: 1 */
	m[p++] = 1;
	m[p++] = 0;
	m[p++] = 0;
	env.send_frame(env.ctx, lat_multicast, m, p);
}

void lat_tick(unsigned ms)
{
	struct circ *c;
	int i;

	for (i = 0; i < LAT_MAXSVC; i++)
		if (svcs[i].used && (svcs[i].s.age_ms += ms) > svcs[i].life_ms)
			svcs[i].used = 0;
	if (offering && (announce_ms += ms) >= LAT_ANNOUNCE_S * 1000UL) {
		announce_ms = 0;
		send_announce();
	}
	for (i = 0; i < LAT_MAXCIRC; i++) {
		c = &circs[i];
		c->timer += ms;
		c->idle += ms;
		if (c->state == C_RUN && c->host) {
			c->rx_idle += ms;
			if (c->need_ack && c->timer >= RETRANS_MS) {
				if (++c->tries > RETRANS_LIMIT) {
					logf_(1, "%s does not answer any more", c->node);
					circuit_down(c, LAT_R_LOST);
					continue;
				}
				resend_last(c);
			}
			/* the terminal server keeps the circuit alive (keepalive
			   20 s here, its own may be longer) */
			if (c->rx_idle > 3 * 60000UL) {
				logf_(1, "nothing from %s for 3 minutes", c->node);
				circuit_down(c, LAT_R_LOST);
				continue;
			}
			/* no session (any more) after a while: end it */
			if (!sessions_left(c) && !c->need_ack &&
			    (c->had_session || c->rx_idle > 30000UL)) {
				send_stop(c, 1);
				circuit_down(c, LAT_R_USER);
				continue;
			}
			circ_pump(c, 0);
			continue;
		}
		switch (c->state) {
		case C_START:
			if (c->timer < START_RETRY_MS)
				break;
			if (++c->tries >= START_TRIES) {
				logf_(1, "no answer from %s", c->node);
				circuit_down(c, LAT_R_NOANSWER);
				break;
			}
			send_start(c);
			break;
		case C_RUN:
			if (c->need_ack) {
				if (c->timer < RETRANS_MS)
					break;
				if (++c->tries > RETRANS_LIMIT) {
					logf_(1, "%s does not answer any more", c->node);
					send_stop(c, 6);
					circuit_down(c, LAT_R_LOST);
					break;
				}
				resend_last(c);
				break;
			}
			if (!sessions_left(c)) {
				send_stop(c, 1);
				circuit_down(c, LAT_R_USER);
				break;
			}
			circ_pump(c, c->idle >= (KEEPALIVE - 3) * 1000UL);
			break;
		}
	}
}

/* ---- sessions ------------------------------------------------------------ */

static struct sess *sess_of(int sid, struct circ **cp)
{
	struct circ *c;

	if (sid < 0 || sid >= LAT_MAXCIRC * LAT_MAXSESS)
		return NULL;
	c = &circs[sid / LAT_MAXSESS];
	if (c->state == C_FREE || c->s[sid % LAT_MAXSESS].state == S_FREE)
		return NULL;
	if (cp)
		*cp = c;
	return &c->s[sid % LAT_MAXSESS];
}

int lat_connect(const char *service, const char *port)
{
	const struct lat_service *v = lat_find_service(service);
	struct circ *c = NULL;
	struct sess *s;
	int i;

	if (!v)
		return -LAT_R_NOSVC;
	for (i = 0; i < LAT_MAXCIRC; i++)
		if (circs[i].state != C_FREE && !circs[i].host &&
		    !memcmp(circs[i].mac, v->mac, 6)) {
			c = &circs[i];
			break;
		}
	if (!c) {
		for (i = 0; i < LAT_MAXCIRC && circs[i].state != C_FREE; i++)
			;
		if (i == LAT_MAXCIRC)
			return -5;
		c = &circs[i];
		memset(c, 0, sizeof *c);
		memcpy(c->mac, v->mac, 6);
		snprintf(c->node, sizeof c->node, "%s", v->node);
		if (++next_lid == 0)
			next_lid = 1;
		c->lid = next_lid;
		c->state = C_START;
		send_start(c);
	}
	for (i = 0; i < LAT_MAXSESS && c->s[i].state != S_FREE; i++)
		;
	if (i == LAT_MAXSESS)
		return -5;
	s = &c->s[i];
	memset(s, 0, sizeof *s);
	s->state = S_WAIT;
	snprintf(s->service, sizeof s->service, "%s", v->name);
	snprintf(s->port, sizeof s->port, "%s", port ? port : "");
	circ_pump(c, 0);
	return (int)(c - circs) * LAT_MAXSESS + i;
}

int lat_write(int sid, const unsigned char *buf, unsigned len)
{
	struct circ *c;
	struct sess *s = sess_of(sid, &c);
	unsigned n;

	if (!s || s->state == S_STOP)
		return -1;
	n = LAT_TXBUF - s->txlen;
	if (n > len)
		n = len;
	memcpy(s->tx + s->txlen, buf, n);
	s->txlen += n;
	circ_pump(c, 0);
	return (int)n;
}

unsigned lat_room(int sid)
{
	struct sess *s = sess_of(sid, NULL);

	return s && s->state != S_STOP ? LAT_TXBUF - s->txlen : 0;
}

int lat_break(int sid)
{
	struct circ *c;
	struct sess *s = sess_of(sid, &c);

	if (!s || s->state != S_RUN)
		return -1;
	s->brk = 1;
	circ_pump(c, 0);
	return 0;
}

void lat_disconnect(int sid)
{
	struct circ *c;
	struct sess *s = sess_of(sid, &c);

	if (!s)
		return;
	if (s->state == S_WAIT) {
		session_down(c, sid % LAT_MAXSESS, LAT_R_USER);
		return;
	}
	if (s->state == S_OFFERED) {
		lat_answer(sid, 5);
		return;
	}
	s->state = S_STOP;
	circ_pump(c, 0);
}

void lat_shutdown(void)
{
	int i;

	for (i = 0; i < LAT_MAXCIRC; i++)
		if (circs[i].state == C_RUN) {
			send_stop(&circs[i], 1);
			circuit_down(&circs[i], LAT_R_USER);
		} else if (circs[i].state == C_START)
			circuit_down(&circs[i], LAT_R_USER);
}

void lat_offer(const char *name, const char *ident)
{
	if (!name || !*name) {
		offering = 0;
		return;
	}
	snprintf(offer_name, sizeof offer_name, "%s", name);
	snprintf(offer_ident, sizeof offer_ident, "%s", ident ? ident : "");
	offering = 1;
	announce_ms = 0;
	send_announce();
}

int lat_answer(int sid, int reason)
{
	struct circ *c;
	struct sess *s = sess_of(sid, &c);

	if (!s || s->state != S_OFFERED)
		return -1;
	if (reason) {
		s->state = S_REJECT;
		s->reason = reason;
	} else
		s->state = S_ACCEPT;
	circ_pump(c, 0);
	return 0;
}

const char *lat_reason_text(int reason)
{
	static const char *slot[16] = {
		"unknown reason", "disconnected", "system shutdown in progress",
		"invalid slot received", "invalid service class",
		"insufficient resources", "service in use", "no such service",
		"service is disabled", "service not offered on the requested port",
		"port name is unknown", "invalid password", "entry is not in the queue",
		"immediate access rejected", "access denied", "corrupted solicit request"
	};

	if (reason >= 0 && reason < 16)
		return slot[reason];
	switch (reason) {
	case LAT_R_NOANSWER: return "the node does not answer";
	case LAT_R_LOST:     return "the node stopped answering";
	case LAT_R_CIRCUIT:  return "the node closed the circuit";
	}
	return "unknown reason";
}

void lat_init(const struct lat_env *e, unsigned seed)
{
	env = *e;
	memset(svcs, 0, sizeof svcs);
	memset(circs, 0, sizeof circs);
	next_lid = seed & 0xffff;
}

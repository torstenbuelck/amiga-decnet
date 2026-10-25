/*
 * dnenv.c - Linux environment for the portable DECnet core.
 *
 * An AF_PACKET socket (needs root / CAP_NET_RAW) in promiscuous mode, so
 * frames to our DECnet MAC arrive although it is not the interface's own.
 * ETH_P_ALL is used because a protocol-specific socket sees nothing on a
 * bridge port (eth0 in br0): the bridge takes the frames first.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "dnenv.h"

static int sock = -1;
static int ifindex;
static unsigned char mymac[6];
static volatile sig_atomic_t stop;
static unsigned long last_tick;
static void (*other_input)(const unsigned char *src, const unsigned char *dst,
                           const unsigned char *payload, unsigned len);
static unsigned other_type;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

int dnenv_stopped(void)
{
	return stop;
}

static void send_eth(unsigned type, const dn_u8 *dst, const dn_u8 *payload, unsigned len)
{
	unsigned char f[1600];
	struct sockaddr_ll sa;
	unsigned n = 14 + len;

	if (n > sizeof f)
		return;
	memcpy(f, dst, 6);
	memcpy(f + 6, mymac, 6);
	f[12] = (unsigned char)(type >> 8);
	f[13] = (unsigned char)type;
	memcpy(f + 14, payload, len);
	while (n < 60)
		f[n++] = 0;
	memset(&sa, 0, sizeof sa);
	sa.sll_family = AF_PACKET;
	sa.sll_ifindex = ifindex;
	sa.sll_halen = 6;
	memcpy(sa.sll_addr, dst, 6);
	if (sendto(sock, f, n, 0, (struct sockaddr *)&sa, sizeof sa) < 0)
		perror("sendto");
}

static void send_frame(void *ctx, const dn_u8 *dst, const dn_u8 *payload, unsigned len)
{
	(void)ctx;
	send_eth(DN_ETHERTYPE, dst, payload, len);
}

void dnenv_other(unsigned type, void (*input)(const unsigned char *src,
                 const unsigned char *dst, const unsigned char *payload, unsigned len))
{
	other_type = type;
	other_input = input;
}

void dnenv_send(unsigned type, const unsigned char *dst, const unsigned char *payload,
                unsigned len)
{
	send_eth(type, dst, payload, len);
}

void dnenv_mac(unsigned char *mac)
{
	memcpy(mac, mymac, 6);
}

static void *xalloc(void *ctx, unsigned n) { (void)ctx; return malloc(n); }
static void xfree(void *ctx, void *p) { (void)ctx; free(p); }

static void logmsg(void *ctx, int level, const char *msg)
{
	struct timespec ts;
	struct tm tm;

	(void)ctx;
	(void)level;
	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	fprintf(stderr, "%02d:%02d:%02d.%03ld %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
	        ts.tv_nsec / 1000000, msg);
}

static unsigned long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int dn_aton(const char *s, dn_u16 *out)
{
	unsigned a, n;
	char c;

	if (sscanf(s, "%u.%u%c", &a, &n, &c) != 2 || a < 1 || a > 63 || n < 1 || n > 1023)
		return -1;
	*out = (dn_u16)((a << 10) | n);
	return 0;
}

int dnenv_open(const char *ifname, dn_u16 addr, int verbose)
{
	struct dn_env env;
	struct sockaddr_ll sa;
	struct packet_mreq mr;

	if (!ifname)
		ifname = if_nametoindex("br0") ? "br0" : "eth0";
	ifindex = if_nametoindex(ifname);
	if (!ifindex) {
		fprintf(stderr, "no interface %s\n", ifname);
		return -1;
	}
	sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (sock < 0) {
		perror("socket (need root)");
		return -1;
	}
	memset(&sa, 0, sizeof sa);
	sa.sll_family = AF_PACKET;
	sa.sll_protocol = htons(ETH_P_ALL);
	sa.sll_ifindex = ifindex;
	if (bind(sock, (struct sockaddr *)&sa, sizeof sa) < 0) {
		perror("bind");
		return -1;
	}
	memset(&mr, 0, sizeof mr);
	mr.mr_ifindex = ifindex;
	mr.mr_type = PACKET_MR_PROMISC;
	if (setsockopt(sock, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof mr) < 0)
		perror("promiscuous");

	dn_node_mac(mymac, addr);
	memset(&env, 0, sizeof env);
	env.send_frame = send_frame;
	env.alloc = xalloc;
	env.free = xfree;
	env.log = logmsg;
	env.loglevel = verbose > 1 ? DN_LOG_DEBUG : verbose ? DN_LOG_INFO : DN_LOG_ERR;
	dn_init(&env, addr);
	dn_seed_links((dn_u16)(time(NULL) ^ (getpid() << 4)));

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	last_tick = now_ms();
	dn_tick(0);
	return 0;
}

void dnenv_pump(int timeout_ms)
{
	struct pollfd pfd;
	unsigned long t;
	int r;

	pfd.fd = sock;
	pfd.events = POLLIN;
	pfd.revents = 0;
	r = poll(&pfd, 1, timeout_ms > 100 ? 100 : timeout_ms);
	if (r > 0 && (pfd.revents & POLLIN)) {
		unsigned char f[1600];
		struct sockaddr_ll from;
		socklen_t fl = sizeof from;
		ssize_t n;

		while ((n = recvfrom(sock, f, sizeof f, MSG_DONTWAIT, (struct sockaddr *)&from, &fl)) > 0) {
			if (n > 14 && from.sll_pkttype != PACKET_OUTGOING &&
			    f[12] == (DN_ETHERTYPE >> 8) && f[13] == (DN_ETHERTYPE & 0xff) &&
			    memcmp(f + 6, mymac, 6) != 0 &&
			    (!memcmp(f, mymac, 6) || (f[0] & 1)))
				dn_input(f + 6, f, f + 14, (unsigned)n - 14);
			else if (n > 14 && other_input && from.sll_pkttype != PACKET_OUTGOING &&
			         f[12] == (other_type >> 8) && f[13] == (other_type & 0xff) &&
			         memcmp(f + 6, mymac, 6) != 0 &&
			         (!memcmp(f, mymac, 6) || (f[0] & 1)))
				other_input(f + 6, f, f + 14, (unsigned)n - 14);
			fl = sizeof from;
		}
	} else if (r < 0 && errno != EINTR) {
		perror("poll");
		stop = 1;
	}
	t = now_ms();
	if (t - last_tick >= 100) {
		dn_tick((unsigned)(t - last_tick));
		last_tick = t;
	}
}

void dnenv_close(void)
{
	dn_shutdown();
	if (sock >= 0)
		close(sock);
	sock = -1;
}

/* ---- blocking links ---------------------------------------------------------- */

struct msg {
	struct msg *next;
	long len;
	unsigned char data[1];
};

struct dns {
	struct dn_link *link;
	int state;              /* 0 connecting, 1 up, 2 gone */
	int reason;
	struct msg *head, *tail;
};

static void cb_connected(struct dn_link *l, const dn_u8 *data, unsigned len)
{
	struct dns *s = dn_get_user(l);

	(void)data;
	(void)len;
	if (s)
		s->state = 1;
}

static void cb_data(struct dn_link *l, const dn_u8 *buf, unsigned len)
{
	struct dns *s = dn_get_user(l);
	struct msg *m;

	if (!s)
		return;
	m = malloc(sizeof *m + len);
	if (!m)
		return;
	m->next = NULL;
	m->len = len;
	memcpy(m->data, buf, len);
	if (s->tail)
		s->tail->next = m;
	else
		s->head = m;
	s->tail = m;
}

static void cb_disconnect(struct dn_link *l, dn_u16 reason)
{
	struct dns *s = dn_get_user(l);

	if (s) {
		s->state = 2;
		s->reason = reason;
		s->link = NULL;
	}
}

static struct dn_object handler = {
	0, NULL, NULL, cb_data, cb_disconnect, NULL, cb_connected
};

struct dns *dns_connect(const struct dn_connreq *req, int timeout_ms, int *reason)
{
	struct dns *s = calloc(1, sizeof *s);
	unsigned long until = now_ms() + timeout_ms;

	if (!s)
		return NULL;
	s->link = dn_connect(req, &handler);
	if (!s->link) {
		free(s);
		if (reason)
			*reason = DN_R_RESOURCES;
		return NULL;
	}
	dn_set_user(s->link, s);
	while (s->state == 0 && now_ms() < until && !stop)
		dnenv_pump(100);
	if (s->state != 1) {
		if (reason)
			*reason = s->state == 2 ? s->reason : DN_R_UNREACHABLE;
		if (s->link) {
			dn_set_user(s->link, NULL);
			dn_disconnect(s->link, 0, NULL, 0);
		}
		free(s);
		return NULL;
	}
	return s;
}

/* incoming links: one object, one waiting caller */
static struct dns *accepted;
static int listening;
static struct dn_conninfo accepted_ci;

static void cb_connect(struct dn_link *l, const struct dn_conninfo *ci)
{
	struct dns *s;

	if (!listening || accepted || !(s = calloc(1, sizeof *s))) {
		dn_reject(l, DN_R_RESOURCES, NULL, 0);
		return;
	}
	s->link = l;
	s->state = 1;
	dn_set_user(l, s);
	accepted_ci = *ci;
	accepted = s;
	dn_accept(l, NULL, 0);
}

static struct dn_object listener = {
	0, NULL, cb_connect, cb_data, cb_disconnect, NULL, NULL
};

struct dns *dns_accept(dn_u8 objnum, int timeout_ms, struct dn_conninfo *ci)
{
	unsigned long until = now_ms() + timeout_ms;
	struct dns *s;

	if (!listener.number) {
		listener.number = objnum;
		if (dn_register_object(&listener))
			return NULL;
	}
	listening = 1;
	while (!accepted && (timeout_ms < 0 || now_ms() < until) && !stop)
		dnenv_pump(100);
	listening = 0;
	s = accepted;
	accepted = NULL;
	if (s && ci)
		*ci = accepted_ci;
	return s;
}

long dns_read(struct dns *s, unsigned char *buf, long max, int timeout_ms)
{
	unsigned long until = now_ms() + timeout_ms;
	struct msg *m;
	long n;

	while (!s->head && s->state == 1 && now_ms() < until && !stop)
		dnenv_pump(100);
	m = s->head;
	if (!m)
		return s->state == 1 ? 0 : -1;
	s->head = m->next;
	if (!s->head)
		s->tail = NULL;
	n = m->len > max ? max : m->len;
	memcpy(buf, m->data, n);
	free(m);
	return n;
}

int dns_write(struct dns *s, const unsigned char *buf, long n)
{
	if (s->state != 1 || !s->link)
		return -1;
	if (dn_send(s->link, buf, (unsigned)n) < 0)
		return -1;
	dnenv_pump(0);
	return 0;
}

void dns_close(struct dns *s)
{
	unsigned long until = now_ms() + 5000;
	struct msg *m, *n;

	if (s->link && s->state == 1) {
		dn_disconnect(s->link, 0, NULL, 0);
		while (s->state == 1 && now_ms() < until)
			dnenv_pump(100);
	}
	if (s->link)
		dn_set_user(s->link, NULL);
	for (m = s->head; m; m = n) {
		n = m->next;
		free(m);
	}
	free(s);
}

int dns_reason(struct dns *s)
{
	return s->reason;
}

/*
 * dnenv.c - Linux environment for the portable DECnet core.
 *
 * An AF_PACKET socket (needs root / CAP_NET_RAW) in promiscuous mode, so
 * frames to our DECnet MAC arrive although it is not the interface's own.
 * ETH_P_ALL is used because a protocol-specific socket sees nothing on a
 * bridge port (eth0 in br0): the bridge takes the frames first.
 *
 * Gateway mode (dnenv_set_gateway): instead of the raw socket, the frames
 * go through Hans Hübner's decnet-gateway tunnel (UDP, ChaCha20-Poly1305,
 * src/dngw); the node then has the address of the client line.
 *
 * Test aid: with DN_NOMULTICAST=1 in the environment, multicast frames are
 * dropped on input, as many WLANs (FRITZ!Box) do for non-IP multicast.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
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
#include "../dngw/dngw.h"

static int sock = -1;
static int ifindex;
static unsigned char mymac[6];
static volatile sig_atomic_t stop;
static unsigned long last_tick;
static void (*other_input)(const unsigned char *src, const unsigned char *dst,
                           const unsigned char *payload, unsigned len);
static unsigned other_type;
static dn_u16 static_router;
static int no_multicast;

/* gateway mode */
static int gw_mode, gw_alive, gw_verbose;
static struct dngw gw;
static uint32_t gw_keyid;
static uint8_t gw_key[32];
static char gw_host[256];
static unsigned gw_port;
static const char *gw_state;
static struct sockaddr_storage gw_addr;
static socklen_t gw_alen;
static unsigned long gw_last_tx, gw_last_rx, gw_last_resolve;
static uint32_t gw_saved_epoch;

static unsigned long now_ms(void);
static void gw_send(const unsigned char *frame, unsigned len);

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
	if (gw_mode) {                  /* no padding: the gateway's LAN side pads */
		if (n <= DNGW_MAXFRAME)
			gw_send(f, n);
		return;
	}
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

void dnenv_set_router(dn_u16 addr)
{
	static_router = addr;
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

/* ---- gateway tunnel --------------------------------------------------------- */

int dnenv_set_gateway(const char *linefile, const char *statefile, dn_u16 *addr)
{
	char line[512];
	unsigned area, node;
	FILE *f = fopen(linefile, "r");

	if (!f) {
		perror(linefile);
		return -1;
	}
	if (!fgets(line, sizeof line, f) ||
	    !dngw_parse_line(line, gw_host, sizeof gw_host, &gw_port, &gw_keyid,
	                     &area, &node, gw_key)) {
		fprintf(stderr, "%s: not a DNGW1 client line\n", linefile);
		fclose(f);
		return -1;
	}
	fclose(f);
	memset(line, 0, sizeof line);
	gw_state = statefile;
	gw_mode = 1;
	*addr = (dn_u16)((area << 10) | node);
	return 0;
}

static int gw_resolve(void)
{
	struct addrinfo hints, *res;
	char port[8];
	int r;

	gw_last_resolve = now_ms();
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	snprintf(port, sizeof port, "%u", gw_port);
	r = getaddrinfo(gw_host, port, &hints, &res);
	if (r) {
		fprintf(stderr, "gateway %s: %s\n", gw_host, gai_strerror(r));
		return -1;
	}
	if (sock >= 0 && res->ai_family != gw_addr.ss_family) {
		close(sock);
		sock = -1;
	}
	if (sock < 0) {
		sock = socket(res->ai_family, SOCK_DGRAM, 0);
		if (sock < 0) {
			perror("socket");
			freeaddrinfo(res);
			return -1;
		}
	}
	memcpy(&gw_addr, res->ai_addr, res->ai_addrlen);
	gw_alen = res->ai_addrlen;
	freeaddrinfo(res);
	return 0;
}

static int gw_save_epoch(uint32_t epoch)
{
	FILE *f;

	if (!gw_state)
		return 0;
	f = fopen(gw_state, "w");
	if (!f || fprintf(f, "%lu\n", (unsigned long)epoch) < 0 || fflush(f) ||
	    fsync(fileno(f)) || fclose(f)) {
		perror(gw_state);
		return -1;
	}
	gw_saved_epoch = epoch;
	return 0;
}

static void gw_send(const unsigned char *frame, unsigned len)
{
	unsigned char pkt[DNGW_MAXPKT];
	unsigned n;

	/* a new epoch is kept before its first counter goes out */
	if (dngw_epoch(&gw) != gw_saved_epoch)
		gw_save_epoch(dngw_epoch(&gw));
	n = dngw_seal(&gw, frame, len, pkt);
	if (sendto(sock, pkt, n, 0, (struct sockaddr *)&gw_addr, gw_alen) < 0 &&
	    errno != EAGAIN)
		perror("sendto gateway");
	gw_last_tx = now_ms();
	if (gw_verbose > 2 && len)
		fprintf(stderr, "-> %u bytes, type %02x%02x, to %02x:%02x:%02x:%02x:%02x:%02x\n",
		        len, frame[12], frame[13], frame[0], frame[1], frame[2], frame[3],
		        frame[4], frame[5]);
}

static int gw_open(void)
{
	unsigned long saved = 0;
	int have = 0;
	uint32_t epoch;
	FILE *f;

	if (gw_resolve())
		return -1;
	if (gw_state && (f = fopen(gw_state, "r")) != NULL) {
		have = fscanf(f, "%lu", &saved) == 1;
		fclose(f);
	}
	if (!gw_state)
		fprintf(stderr, "warning: no epoch state file; the clock must not go back\n");
	epoch = dngw_start(&gw, gw_keyid, gw_key, (uint32_t)time(NULL), (uint32_t)saved, have);
	if (gw_save_epoch(epoch))
		return -1;
	fprintf(stderr, "tunnel to %s:%u as %u.%u (%02x:%02x:%02x:%02x:%02x:%02x)\n",
	        gw_host, gw_port, (unsigned)(mymac[5] >> 2),
	        (unsigned)(((mymac[5] & 3) << 8) | mymac[4]), mymac[0], mymac[1], mymac[2],
	        mymac[3], mymac[4], mymac[5]);
	gw_send(NULL, 0);                       /* keepalive at start */
	return 0;
}

int dnenv_open(const char *ifname, dn_u16 addr, int verbose)
{
	struct dn_env env;
	struct sockaddr_ll sa;
	struct packet_mreq mr;

	if (gw_mode) {
		dn_node_mac(mymac, addr);
		gw_verbose = verbose;
		if (gw_open())
			return -1;
		goto core;
	}
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
core:
	memset(&env, 0, sizeof env);
	env.send_frame = send_frame;
	env.alloc = xalloc;
	env.free = xfree;
	env.log = logmsg;
	env.loglevel = verbose > 1 ? DN_LOG_DEBUG : verbose ? DN_LOG_INFO : DN_LOG_ERR;
	dn_init(&env, addr);
	if (static_router)
		dn_set_router(static_router);
	dn_seed_links((dn_u16)(time(NULL) ^ (getpid() << 4)));
	no_multicast = getenv("DN_NOMULTICAST") && *getenv("DN_NOMULTICAST") == '1';
	if (no_multicast)
		fprintf(stderr, "DN_NOMULTICAST: dropping multicast frames (test)\n");

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	last_tick = now_ms();
	dn_tick(0);
	return 0;
}

/* a received frame (raw socket or tunnel): to the core or the other
   ethertype's handler if it is for us */
static void deliver(const unsigned char *f, unsigned n)
{
	if (no_multicast && n > 14 && (f[0] & 1) && memcmp(f, "\xff\xff\xff\xff\xff\xff", 6))
		return;
	if (n <= 14 || !memcmp(f + 6, mymac, 6) || (memcmp(f, mymac, 6) && !(f[0] & 1)))
		return;
	if (f[12] == (DN_ETHERTYPE >> 8) && f[13] == (DN_ETHERTYPE & 0xff))
		dn_input(f + 6, f, f + 14, n - 14);
	else if (other_input && f[12] == (other_type >> 8) && f[13] == (other_type & 0xff))
		other_input(f + 6, f, f + 14, n - 14);
}

static void gw_pump(int timeout_ms)
{
	struct pollfd pfd;
	unsigned char pkt[DNGW_MAXPKT + 64], *f;
	unsigned len;
	unsigned long t;
	ssize_t n;
	int r;

	pfd.fd = sock;
	pfd.events = POLLIN;
	pfd.revents = 0;
	r = poll(&pfd, 1, timeout_ms > 100 ? 100 : timeout_ms);
	if (r > 0 && (pfd.revents & POLLIN)) {
		while ((n = recv(sock, pkt, sizeof pkt, MSG_DONTWAIT)) > 0) {
			if (!dngw_open(&gw, pkt, (unsigned)n, &f, &len))
				continue;               /* silently, as the protocol says */
			gw_last_rx = now_ms();
			if (!gw_alive) {
				fprintf(stderr, "gateway %s answers\n", gw_host);
				gw_alive = 1;
			}
			if (len) {
				if (gw_verbose > 2)
					fprintf(stderr, "<- %u bytes, type %02x%02x, from %02x:%02x:%02x:%02x:%02x:%02x\n",
					        len, f[12], f[13], f[6], f[7], f[8], f[9], f[10], f[11]);
				deliver(f, len);
			}
		}
	} else if (r < 0 && errno != EINTR) {
		perror("poll");
		stop = 1;
	}
	t = now_ms();
	if (t - gw_last_tx >= DNGW_KEEPALIVE_S * 1000UL)
		gw_send(NULL, 0);
	if (gw_alive && t - gw_last_rx > DNGW_LOST_S * 1000UL) {
		fprintf(stderr, "gateway %s not answering\n", gw_host);
		gw_alive = 0;
	}
	if (!gw_alive && t - gw_last_resolve >= 30000)
		gw_resolve();               /* a home line's address changes */
	if (t - last_tick >= 100) {
		dn_tick((unsigned)(t - last_tick));
		last_tick = t;
	}
}

void dnenv_pump(int timeout_ms)
{
	struct pollfd pfd;
	unsigned long t;
	int r;

	if (gw_mode) {
		gw_pump(timeout_ms);
		return;
	}

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
			if (from.sll_pkttype != PACKET_OUTGOING)
				deliver(f, (unsigned)n);
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

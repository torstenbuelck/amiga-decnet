/*
 * dntool.c - DAP client test tool on the portable core (Linux).
 *
 * Usage: dntool [-a area.node] [-i iface] [-v] [-u user] [-p password] [-n]
 *               [-t termtype]
 *               dir NODE SPEC
 *               get NODE SPEC [LOCALFILE]
 *               put NODE LOCALFILE [SPEC]
 *               nodes NODE [permanent]
 *               task NODE NAME
 *               login NODE
 *               serve
 *   login is a CTERM terminal session (like SET HOST); Ctrl-] ends it.
 *   serve is the other end: it waits for SET HOST to this node, asks for
 *   the password given with -p (no -p: every session is refused) and runs
 *   a small test loop (lines echoed, "keys" shows key codes, "exit")
 *   -t sets the terminal type it reports (default VT200; VMS knows VT100,
 *   VT102, VT200). VMS still needs SET TERMINAL/INQUIRE (or /DEVICE) for
 *   full-screen programs: CTERM carries no ANSI/CRT capabilities.
 *   task connects to the named object (on VMS: runs NAME.COM in the login
 *   directory) and prints every message until the other end disconnects
 *   -n  do not offer DAP blocking (needed for FOTOFX)
 *   put writes LOCALFILE as binary records (default SPEC: its base name)
 *   nodes lists the node database of NODE through NML (object 19)
 *
 * The tool is a DECnet node itself (default 23.203), so dnhost must not
 * run with the same address at the same time.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "dnenv.h"
#include "../dap/dap.h"
#include "../nml/nml.h"
#include "../cterm/cterm.h"
#include "../cterm/ctermh.h"
#include "../lat/lat.h"

static const struct { const char *name; const char *addr; } nodes[] = {
	{ "EUGENE", "23.21" }, { "HOWIE", "23.208" }, { "COLT", "23.209" },
	{ "FOTOFX", "23.1001" }, { "MACCI", "23.201" }, { "AMI", "23.202" },
};

static int resolve(const char *s, dn_u16 *out)
{
	unsigned i;

	for (i = 0; i < sizeof nodes / sizeof nodes[0]; i++)
		if (!strcasecmp(s, nodes[i].name))
			s = nodes[i].addr;
	return dn_aton(s, out);
}

static long link_read(void *ctx, unsigned char *buf, long max, long timeout)
{
	return dns_read(ctx, buf, max, (int)timeout);
}

static int link_write(void *ctx, const unsigned char *buf, long n)
{
	return dns_write(ctx, buf, n);
}

static void dap_log(void *ctx, const char *msg)
{
	(void)ctx;
	fprintf(stderr, "dap: %s\n", msg);
}

static int print_entry(void *ud, const struct dap_dirent *e)
{
	int *count = ud;

	if (*count == 0)
		printf("Directory of %s%s\n\n", e->volume, e->directory);
	printf("%-30s %8ld %-20s %s\n", e->name, e->blocks, e->created, e->owner);
	(*count)++;
	return 0;
}

struct out {
	FILE *f;
	long bytes, records;
};

static int write_record(void *ud, const unsigned char *p, long n, int text)
{
	struct out *o = ud;

	if (n > 0 && fwrite(p, 1, n, o->f) != (size_t)n)
		return 1;
	if (text)
		fputc('\n', o->f);
	o->bytes += n;
	o->records++;
	return 0;
}

static int print_node(void *ud, unsigned addr, const char *name, int exec, int state)
{
	static const char *states[] = { "on", "off", "shut", "restricted", "reachable",
	                                "unreachable" };
	char a[16];

	(void)ud;
	snprintf(a, sizeof a, "%u.%u", addr >> 10, addr & 1023);
	printf("%-8s %-9s %s%s\n", name[0] ? name : "-", a,
	       state >= 0 && state <= 5 ? states[state] : "-", exec ? " (executor)" : "");
	return 0;
}

/* nodes NODE [permanent] */
static int do_nodes(dn_u16 node, const char *db, const char *user, const char *pass,
                    int verbose)
{
	struct dn_connreq req;
	struct nml n;
	struct dns *s;
	int reason, count;

	memset(&req, 0, sizeof req);
	req.node = node;
	req.objnum = NML_OBJECT;
	req.srcname = "DNTOOL";
	req.user = user;
	req.password = pass;
	req.data = nml_version;
	req.datalen = sizeof nml_version;
	s = dns_connect(&req, 30000, &reason);
	if (!s) {
		fprintf(stderr, "connect to NML on %s failed: %s (%d)\n", dn_ntoa(node),
		        dap_reason_text(reason), reason);
		return 1;
	}
	memset(&n, 0, sizeof n);
	n.read = link_read;
	n.write = link_write;
	n.log = dap_log;
	n.ctx = s;
	n.verbose = verbose;
	count = nml_known_nodes(&n, db && !strcmp(db, "permanent"), print_node, NULL);
	if (count < 0)
		fprintf(stderr, "%s\n", n.error);
	else
		printf("\n%d nodes.\n", count);
	dns_close(s);
	return count < 0;
}

/* task NODE NAME */
static int do_task(dn_u16 node, const char *name, const char *user, const char *pass)
{
	struct dn_connreq req;
	unsigned char buf[2048];
	struct dns *s;
	long n;
	int reason;

	memset(&req, 0, sizeof req);
	req.node = node;
	req.objname = name;
	req.srcname = "DNTOOL";
	req.user = user;
	req.password = pass;
	s = dns_connect(&req, 30000, &reason);
	if (!s) {
		fprintf(stderr, "connect to task %s on %s failed: %s (%d)\n", name, dn_ntoa(node),
		        dap_reason_text(reason), reason);
		return 1;
	}
	while ((n = dns_read(s, buf, sizeof buf, 30000)) > 0)
		printf("<- %.*s\n", (int)n, buf);
	printf(n == 0 ? "(timeout)\n" : "(disconnected, reason %d)\n", dns_reason(s));
	dns_close(s);
	return 0;
}

static double now(void);

/* login NODE: CTERM session on the terminal */
static int cterm_net_write(void *ctx, const unsigned char *buf, long n)
{
	return dns_write(ctx, buf, n);
}

static void cterm_term_write(void *ctx, const char *buf, long n)
{
	(void)ctx;
	if (write(1, buf, n) < 0)
		return;
}

static void cterm_log(void *ctx, const char *msg)
{
	(void)ctx;
	fprintf(stderr, "[%s]\r\n", msg);
}

static int do_login(dn_u16 node, const char *termtype, int verbose)
{
	struct dn_connreq req;
	struct termios old, raw;
	struct cterm c;
	unsigned char buf[2048];
	struct dns *s;
	int reason, tty = isatty(0), rc = 0;
	long n;

	memset(&req, 0, sizeof req);
	req.node = node;
	req.objnum = CTERM_OBJECT;
	req.srcname = "DNTOOL";
	s = dns_connect(&req, 30000, &reason);
	if (!s) {
		fprintf(stderr, "connect to %s failed: %s (%d)\n", dn_ntoa(node),
		        dap_reason_text(reason), reason);
		return 1;
	}
	memset(&c, 0, sizeof c);
	c.net_write = cterm_net_write;
	c.term_write = cterm_term_write;
	c.log = cterm_log;
	c.ctx = s;
	c.verbose = verbose;
	strcpy(c.revision, "dntool");
	if (termtype)
		snprintf(c.termtype, sizeof c.termtype, "%s", termtype);
	cterm_init(&c);

	if (tty) {
		tcgetattr(0, &old);
		raw = old;
		raw.c_iflag &= ~(BRKINT | INLCR | ICRNL | IXON);
		raw.c_iflag |= IGNBRK;
		raw.c_oflag &= ~OPOST;
		raw.c_lflag &= ~(ICANON | ISIG | ECHO | ECHONL | IEXTEN);
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;
		tcsetattr(0, TCSANOW, &raw);
		fprintf(stderr, "Connected to %s. Escape character is '^]'.\r\n", dn_ntoa(node));
	}
	for (;;) {
		struct pollfd pfd = { 0, POLLIN, 0 };
		double t = now();

		dnenv_pump(20);
		while ((n = dns_read(s, buf, sizeof buf, 0)) > 0)
			if (cterm_net(&c, buf, n) < 0)
				break;
		if (n < 0 || c.unbound) {
			if (!c.unbound)
				rc = 1;
			break;
		}
		if (poll(&pfd, 1, 0) > 0) {
			char kb[256];
			long k = read(0, kb, sizeof kb), i;
			if (k <= 0)
				break;
			for (i = 0; i < k && kb[i] != 0x1d; i++)
				;
			cterm_keys(&c, kb, i);
			if (i < k)
				break;  /* Ctrl-] */
		}
		cterm_tick(&c, (long)((now() - t) * 1000));
		if (dnenv_stopped())
			break;
	}
	if (tty)
		tcsetattr(0, TCSANOW, &old);
	fprintf(stderr, "\r\n%s\n", c.unbound ? "Session ended by the host." :
	        rc ? "Link lost." : "Session closed.");
	dns_close(s);
	return rc;
}

/* serve: CTERM host side with a test loop */
struct serve {
	struct ctermh h;
	struct dns *s;
	const char *password;
	int state;              /* 0 password, 1 lines, 2 keys, 3 done */
	int tries;
	int got_input;
};

static int serve_net_write(void *ctx, const unsigned char *buf, long n)
{
	return dns_write(((struct serve *)ctx)->s, buf, n);
}

static void serve_out(struct serve *v, const char *s)
{
	ctermh_write(&v->h, s, (long)strlen(s));
}

static void serve_prompt(struct serve *v)
{
	static const char pw[] = "Password: ", ln[] = "dntool> ";

	if (v->state == 0)
		ctermh_read(&v->h, CTH_SECRET, pw, sizeof pw - 1, NULL, 0);
	else if (v->state == 1)
		ctermh_read(&v->h, CTH_LINE, ln, sizeof ln - 1, NULL, 0);
	else if (v->state == 2)
		ctermh_read(&v->h, CTH_CHAR, NULL, 0, NULL, 0);
}

static void serve_input(void *ctx, const char *buf, long n, int term)
{
	struct serve *v = ctx;
	char out[CTERMH_MAXLINE * 4 + 64];
	long i, o;

	v->got_input = 1;
	switch (v->state) {
	case 0:
		if ((size_t)n == strlen(v->password) && !memcmp(buf, v->password, n)) {
			snprintf(out, sizeof out, "Welcome. Terminal %s, %d x %d.\n"
			         "Lines are echoed; \"keys\" shows key codes, \"exit\" ends.\n",
			         v->h.termtype[0] ? v->h.termtype : "?", v->h.width, v->h.height);
			serve_out(v, out);
			v->state = 1;
		} else if (++v->tries >= 3) {
			serve_out(v, "Access denied.\n");
			v->state = 3;
		} else
			serve_out(v, "Wrong password.\n");
		break;
	case 1:
		if (n == 4 && !strncasecmp(buf, "exit", 4)) {
			serve_out(v, "Bye.\n");
			v->state = 3;
		} else if (n == 4 && !strncasecmp(buf, "keys", 4)) {
			serve_out(v, "Type keys, q ends.\n");
			v->state = 2;
		} else {
			o = snprintf(out, sizeof out, "[%ld bytes, terminator %02x] ", n, term);
			memcpy(out + o, buf, n);
			out[o + n] = '\n';
			ctermh_write(&v->h, out, o + n + 1);
		}
		break;
	case 2:
		o = 0;
		for (i = 0; i < n; i++)
			o += snprintf(out + o, sizeof out - o, "%02x ", (unsigned char)buf[i]);
		out[o++] = '\n';
		ctermh_write(&v->h, out, o);
		if (n == 1 && buf[0] == 'q')
			v->state = 1;
		break;
	}
}

static void serve_interrupt(void *ctx, int ch)
{
	struct serve *v = ctx;
	char out[32];

	snprintf(out, sizeof out, "*** interrupt ^%c\n", ch + '@');
	serve_out(v, out);
}

static int do_serve(const char *password, int verbose)
{
	struct dn_conninfo ci;
	unsigned char buf[2048];
	struct serve v;
	long n;

	if (!password || !*password) {
		fprintf(stderr, "serve needs a password (-p)\n");
		return 1;
	}
	fprintf(stderr, "waiting for CTERM sessions (object 42)\n");
	while (!dnenv_stopped()) {
		double t;

		memset(&v, 0, sizeof v);
		v.s = dns_accept(CTERM_OBJECT, -1, &ci);
		if (!v.s)
			break;
		fprintf(stderr, "session from %s (%s)\n", dn_ntoa(ci.node), ci.srcname);
		v.password = password;
		v.h.net_write = serve_net_write;
		v.h.input = serve_input;
		v.h.interrupt = serve_interrupt;
		v.h.log = cterm_log;
		v.h.ctx = &v;
		v.h.verbose = verbose;
		ctermh_init(&v.h);
		ctermh_start(&v.h);
		t = now();
		while (!v.h.ready && now() - t < 10 && !dnenv_stopped()) {
			n = dns_read(v.s, buf, sizeof buf, 200);
			if (n < 0 || (n > 0 && ctermh_net(&v.h, buf, n) < 0))
				break;
		}
		if (v.h.ready) {
			/* the answer to READ-CHARACTERISTICS, for the welcome */
			while ((n = dns_read(v.s, buf, sizeof buf, 500)) > 0)
				if (ctermh_net(&v.h, buf, n) < 0 || v.h.width)
					break;
			serve_out(&v, "\nDECnet test host (dntool serve)\n\n");
			serve_prompt(&v);
			while (v.state != 3 && !dnenv_stopped()) {
				n = dns_read(v.s, buf, sizeof buf, 200);
				if (n < 0 || (n > 0 && ctermh_net(&v.h, buf, n) < 0))
					break;
				if (v.got_input) {
					v.got_input = 0;
					serve_prompt(&v);
				}
			}
			if (v.state == 3)
				ctermh_unbind(&v.h);
		}
		fprintf(stderr, "session ended\n");
		dns_close(v.s);
	}
	return 0;
}

/* lat [SERVICE]: list the LAT services, or a terminal session to one */
static int lat_sid = -1, lat_up, lat_down, lat_reason;

static void lat_send(void *ctx, const unsigned char *dst, const unsigned char *payload,
                     unsigned len)
{
	(void)ctx;
	dnenv_send(LAT_ETHERTYPE, dst, payload, len);
}

static void lat_data(void *ctx, int sid, const unsigned char *buf, unsigned len)
{
	(void)ctx;
	(void)sid;
	if (write(1, buf, len) < 0)
		return;
}

static void lat_event(void *ctx, int sid, int ev, int reason)
{
	(void)ctx;
	if (sid != lat_sid)
		return;
	if (ev == LAT_EV_UP)
		lat_up = 1;
	else {
		lat_down = 1;
		lat_reason = reason;
	}
}

static void lat_logmsg(void *ctx, const char *msg)
{
	(void)ctx;
	fprintf(stderr, "[lat: %s]\r\n", msg);
}

static void lat_start(int verbose)
{
	struct lat_env e;

	memset(&e, 0, sizeof e);
	e.send_frame = lat_send;
	e.data = lat_data;
	e.event = lat_event;
	e.log = lat_logmsg;
	e.verbose = verbose;
	dnenv_mac(e.mac);
	strcpy(e.node, "DNTOOL");
	strcpy(e.ident, "dntool LAT test (Linux)");
	lat_init(&e, (unsigned)time(NULL));
	dnenv_other(LAT_ETHERTYPE, lat_input);
}

/* run the event loop for ms milliseconds (or until *flag) */
static void lat_wait(long ms, const int *flag)
{
	double t0 = now(), t = t0;

	while ((now() - t0) * 1000 < ms && !(flag && *flag) && !dnenv_stopped()) {
		dnenv_pump(10);
		lat_tick((unsigned)((now() - t) * 1000));
		t = now();
	}
}

static int do_lat_list(int verbose, int secs)
{
	const struct lat_service *v;
	int i;

	lat_start(verbose);
	fprintf(stderr, "listening %d s for service announcements...\n", secs);
	lat_wait(secs * 1000L, NULL);
	for (i = 0; (v = lat_service(i)) != NULL; i++)
		printf("%-16s %-16s %3d  %02x-%02x-%02x-%02x-%02x-%02x  %s\n", v->name, v->node,
		       v->rating, v->mac[0], v->mac[1], v->mac[2], v->mac[3], v->mac[4],
		       v->mac[5], v->ident);
	return i ? 0 : 1;
}

static int do_lat(const char *service, int verbose)
{
	struct termios old, raw;
	int tty = isatty(0), rc = 0, found = 0;
	double t0;

	lat_start(verbose);
	/* the services are known once their nodes have announced them */
	t0 = now();
	while (!lat_find_service(service) && now() - t0 < 70 && !dnenv_stopped()) {
		if (!found++)
			fprintf(stderr, "waiting for an announcement of %s...\n", service);
		lat_wait(500, NULL);
	}
	lat_sid = lat_connect(service, "dntool");
	if (lat_sid < 0) {
		fprintf(stderr, "%s: %s\n", service, lat_reason_text(-lat_sid));
		return 1;
	}
	lat_wait(10000, &lat_up);
	if (!lat_up) {
		fprintf(stderr, "%s: %s\n", service,
		        lat_down ? lat_reason_text(lat_reason) : "no answer");
		lat_disconnect(lat_sid);
		return 1;
	}
	if (tty) {
		tcgetattr(0, &old);
		raw = old;
		raw.c_iflag &= ~(BRKINT | INLCR | ICRNL | IXON);
		raw.c_iflag |= IGNBRK;
		raw.c_oflag &= ~OPOST;
		raw.c_lflag &= ~(ICANON | ISIG | ECHO | ECHONL | IEXTEN);
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;
		tcsetattr(0, TCSANOW, &raw);
	}
	fprintf(stderr, "Connected to %s. Escape character is '^]'.\r\n", service);
	while (!lat_down && !dnenv_stopped()) {
		struct pollfd pfd = { 0, POLLIN, 0 };
		double t = now();

		dnenv_pump(10);
		if (lat_room(lat_sid) >= 256 && poll(&pfd, 1, 0) > 0) {
			unsigned char kb[256];
			long k = read(0, kb, sizeof kb), i;

			if (k <= 0)
				break;
			for (i = 0; i < k && kb[i] != 0x1d; i++)
				;
			if (i)
				lat_write(lat_sid, kb, (unsigned)i);
			if (i < k)
				break;  /* Ctrl-] */
		}
		lat_tick((unsigned)((now() - t) * 1000));
	}
	if (tty)
		tcsetattr(0, TCSANOW, &old);
	if (!lat_down) {
		lat_disconnect(lat_sid);
		lat_wait(2000, &lat_down);
		fprintf(stderr, "\r\nSession closed.\n");
	} else {
		fprintf(stderr, "\r\nSession ended: %s.\n", lat_reason_text(lat_reason));
		rc = lat_reason == LAT_R_LOST || lat_reason == LAT_R_CIRCUIT;
	}
	lat_wait(500, NULL);            /* the circuit stop goes out */
	return rc;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s [-a area.node] [-i iface] [-v] [-u user] [-p password] [-n] [-t termtype]\n"
	        "          dir NODE SPEC | get NODE SPEC [LOCALFILE] | nodes NODE [permanent]\n"
	        "          put NODE LOCALFILE [SPEC] | task NODE NAME | login NODE | serve\n"
	        "          lat [SERVICE]\n", prog);
	exit(1);
}

int main(int argc, char **argv)
{
	const char *ifname = NULL, *user = NULL, *pass = NULL, *termtype = NULL, *cmd, *spec;
	dn_u16 addr = (23 << 10) | 203, node;
	int c, verbose = 0, blocking = 1, reason, rc = 1;
	struct dn_connreq req;
	struct dns *s;
	struct dap *d;
	double t0;

	while ((c = getopt(argc, argv, "a:i:vu:p:nt:")) != -1) {
		switch (c) {
		case 'a': if (dn_aton(optarg, &addr)) usage(argv[0]); break;
		case 'i': ifname = optarg; break;
		case 'v': verbose++; break;
		case 'u': user = optarg; break;
		case 'p': pass = optarg; break;
		case 'n': blocking = 0; break;
		case 't': termtype = optarg; break;
		default: usage(argv[0]);
		}
	}
	if (argc - optind == 1 && !strcmp(argv[optind], "serve")) {
		if (dnenv_open(ifname, addr, verbose))
			return 1;
		rc = do_serve(pass, verbose);
		dnenv_close();
		return rc;
	}
	if (argc - optind >= 1 && !strcmp(argv[optind], "lat")) {
		if (dnenv_open(ifname, addr, verbose))
			return 1;
		rc = argc - optind > 1 ? do_lat(argv[optind + 1], verbose)
		                       : do_lat_list(verbose, 25);
		dnenv_close();
		return rc;
	}
	if (argc - optind < 2)
		usage(argv[0]);
	cmd = argv[optind];
	if (resolve(argv[optind + 1], &node)) {
		fprintf(stderr, "unknown node %s\n", argv[optind + 1]);
		return 1;
	}
	if (!strcmp(cmd, "task")) {
		if (argc - optind < 3)
			usage(argv[0]);
		if (dnenv_open(ifname, addr, verbose))
			return 1;
		rc = do_task(node, argv[optind + 2], user, pass);
		dnenv_close();
		return rc;
	}
	if (!strcmp(cmd, "login")) {
		if (dnenv_open(ifname, addr, verbose))
			return 1;
		rc = do_login(node, termtype, verbose);
		dnenv_close();
		return rc;
	}
	if (!strcmp(cmd, "nodes")) {
		if (dnenv_open(ifname, addr, verbose))
			return 1;
		rc = do_nodes(node, argc - optind > 2 ? argv[optind + 2] : NULL, user, pass, verbose);
		dnenv_close();
		return rc;
	}
	if (argc - optind < 3)
		usage(argv[0]);
	spec = argv[optind + 2];
	if (strcmp(cmd, "dir") && strcmp(cmd, "get") && strcmp(cmd, "put"))
		usage(argv[0]);

	if (dnenv_open(ifname, addr, verbose))
		return 1;

	memset(&req, 0, sizeof req);
	req.node = node;
	req.objnum = DAP_OBJECT;
	req.srcname = "DNTOOL";
	req.user = user;
	req.password = pass;
	t0 = now();
	s = dns_connect(&req, 30000, &reason);
	if (!s) {
		fprintf(stderr, "connect to %s failed: %s (%d)\n", dn_ntoa(node),
		        dap_reason_text(reason), reason);
		dnenv_close();
		return 1;
	}

	d = calloc(1, sizeof *d);
	d->read = link_read;
	d->write = link_write;
	d->log = dap_log;
	d->ctx = s;
	d->verbose = verbose;
	if (dap_config(d, blocking)) {
		fprintf(stderr, "%s\n", d->error);
	} else if (!strcmp(cmd, "dir")) {
		int count = 0;
		if (dap_directory(d, spec, print_entry, &count) == 0) {
			printf("\nTotal of %d file%s.\n", count, count == 1 ? "" : "s");
			rc = 0;
		} else {
			fprintf(stderr, "%s\n", d->error);
		}
	} else if (!strcmp(cmd, "put")) {
		const char *remote = argc - optind > 3 ? argv[optind + 3] : NULL;
		unsigned char buf[DAP_MAXREC];
		char name[256];
		long bytes = 0, records = 0;
		size_t n;
		FILE *f;

		if (!remote) {
			remote = strrchr(spec, '/');
			remote = remote ? remote + 1 : spec;
		}
		f = fopen(spec, "rb");
		if (!f) {
			perror(spec);
		} else if (dap_open_write(d, remote, 0, name, sizeof name)) {
			fprintf(stderr, "%s\n", d->error);
			fclose(f);
		} else {
			rc = 0;
			while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
				if (dap_write_record(d, buf, (long)n)) {
					fprintf(stderr, "%s\n", d->error);
					rc = 1;
					break;
				}
				bytes += n;
				records++;
			}
			fclose(f);
			if (dap_close_write(d)) {
				fprintf(stderr, "%s\n", d->error);
				rc = 1;
			}
			if (!rc) {
				double t = now() - t0;
				printf("%s -> %s: %ld bytes, %ld records in %.1f s (%.0f bytes/s)\n",
				       spec, name[0] ? name : remote, bytes, records, t,
				       t > 0 ? bytes / t : 0);
			}
		}
	} else {
		const char *local = argc - optind > 3 ? argv[optind + 3] : NULL;
		struct out o;
		long size;

		if (!local) {
			const char *p = spec, *q;
			for (q = spec; *q; q++)
				if (*q == ']' || *q == ':' || *q == '/' || *q == '>')
					p = q + 1;
			local = p;
			if (strchr(local, ';')) {       /* drop the VMS version */
				static char name[256];
				snprintf(name, sizeof name, "%.*s", (int)(strchr(local, ';') - local), local);
				local = name;
			}
		}
		memset(&o, 0, sizeof o);
		o.f = fopen(local, "wb");
		if (!o.f) {
			perror(local);
		} else {
			if (dap_get(d, spec, write_record, &o, &size) == 0) {
				double t = now() - t0;
				printf("%s -> %s: %ld bytes, %ld records in %.1f s (%.0f bytes/s)\n",
				       spec, local, o.bytes, o.records, t, t > 0 ? o.bytes / t : 0);
				rc = 0;
			} else {
				fprintf(stderr, "%s\n", d->error);
			}
			fclose(o.f);
		}
	}
	dns_close(s);
	free(d);
	dnenv_close();
	return rc;
}

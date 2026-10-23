/*
 * nml.c - portable NML client (see nml.h).
 *
 * Copyright (C) 2026  Torsten
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nml.h"

#define TIMEOUT 30000L

#define NICE_READ_INFO    20
#define NICE_ENT_NODE     0
#define NICE_FMT_KNOWN    0xff      /* entity format -1 */
#define NICE_OPT_PERM     0x80

/* return codes */
#define RC_SUCCESS        1
#define RC_MORE           2         /* several responses follow */
#define RC_DONE           (-128)    /* end of the responses */

const unsigned char nml_version[3] = { 4, 0, 0 };

static void logf_(struct nml *n, const char *fmt, ...)
{
	char buf[200];
	va_list ap;

	if (!n->verbose || !n->log)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	n->log(n->ctx, buf);
}

static int fail(struct nml *n, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(n->error, sizeof n->error, fmt, ap);
	va_end(ap);
	logf_(n, "error: %s", n->error);
	return -1;
}

static const char *rc_text(int rc)
{
	switch (rc) {
	case -1: return "unrecognized function or option";
	case -2: return "invalid message format";
	case -3: return "privilege violation";
	case -5: return "management program error";
	case -8: return "unrecognized component";
	case -9: return "invalid identification";
	case -12: return "component in wrong state";
	case -15: return "parameter value too long";
	default: return "server error";
	}
}

/* Length of one NICE data item (data type byte included) at p, 0 if it
   runs past e. Coded multiple fields recurse into their parts. */
static long item_len(const unsigned char *p, const unsigned char *e)
{
	int dt;
	long len;

	if (p >= e)
		return 0;
	dt = p[0];
	if (dt & 0x80) {
		if (dt & 0x40) {                /* CM: n fields, each typed */
			int i, fields = dt & 0x3f;
			len = 1;
			for (i = 0; i < fields; i++) {
				long l = item_len(p + len, e);
				if (!l)
					return 0;
				len += l;
			}
		} else {
			len = 1 + (dt & 0x3f);  /* C-n: n bytes */
		}
	} else if ((dt & 0x40) || (dt & 0x0f) == 0) {
		len = p + 1 < e ? 2 + p[1] : 0;     /* AI or binary image: count + bytes */
	} else {
		len = 1 + (dt & 0x0f);          /* binary number of n bytes */
	}
	return len && p + len <= e ? len : 0;
}

/* Node state from the parameters after the entity, NML_STATE_UNKNOWN if
   it is not among them. */
static int node_state(const unsigned char *p, const unsigned char *e)
{
	while (p + 3 <= e) {
		unsigned type = (p[0] | (p[1] << 8)) & 0x0fff;
		long l = item_len(p + 2, e);

		if (!l)
			break;
		if (type == 0 && (p[2] & 0xc0) == 0x80 && l == 2)   /* STATE, C-1 */
			return p[3];
		p += 2 + l;
	}
	return NML_STATE_UNKNOWN;
}

int nml_known_nodes(struct nml *n, int permanent,
                    int (*cb)(void *ud, unsigned addr, const char *name, int executor,
                              int state),
                    void *ud)
{
	unsigned char req[3];
	int count = 0, stop = 0;

	n->code = 0;
	req[0] = NICE_READ_INFO;
	req[1] = (unsigned char)((permanent ? NICE_OPT_PERM : 0) | NICE_ENT_NODE);  /* summary */
	req[2] = NICE_FMT_KNOWN;
	logf_(n, "-> read information, known nodes, %s", permanent ? "permanent" : "volatile");
	if (n->write(n->ctx, req, sizeof req) != 0)
		return fail(n, "link closed while sending");

	for (;;) {
		long len = n->read(n->ctx, n->rx, sizeof n->rx, TIMEOUT);
		const unsigned char *p = n->rx, *e = n->rx + len;
		int rc;

		if (len == 0)
			return fail(n, "timeout waiting for the server");
		if (len < 0)
			return fail(n, "link closed by the server");
		rc = (signed char)p[0];
		n->code = rc;
		if (rc == RC_DONE) {
			logf_(n, "<- done, %d nodes", count);
			return count;
		}
		if (rc == RC_MORE && len == 1)
			continue;
		if (rc != RC_SUCCESS && rc != RC_MORE) {
			char msg[80] = "";
			if (len >= 4) {                 /* detail, then an error text */
				int ml = p[3];
				if (4 + ml <= len) {
					memcpy(msg, p + 4, ml < 79 ? ml : 79);
					msg[ml < 79 ? ml : 79] = 0;
				}
			}
			return fail(n, "NML error %d: %s%s%s", rc, rc_text(rc), msg[0] ? ", " : "", msg);
		}
		/* success: detail(2), error text, node entity: address(2), name */
		if (len < 4 || 4 + p[3] + 3 > len) {
			logf_(n, "<- short response (%ld bytes), skipped", len);
			continue;
		}
		p += 4 + p[3];
		if (!stop) {
			unsigned addr = p[0] | (p[1] << 8);
			int nl = p[2] & 0x7f, exec = (p[2] & 0x80) != 0, state;
			char name[8];

			p += 3;
			if (p + nl > e || nl > 6)
				nl = 0;
			memcpy(name, p, nl);
			name[nl] = 0;
			state = node_state(p + nl, e);
			count++;
			logf_(n, "<- node %u.%u %s%s, state %d", addr >> 10, addr & 1023, name,
			      exec ? " (executor)" : "", state);
			if (cb(ud, addr, name, exec, state))
				stop = 1;               /* read on until RC_DONE */
		}
	}
}

/*
 * dap.c - portable DAP client (see dap.h).
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dap.h"

/* DAP message types */
enum { CONFIG = 1, ATTRIB = 2, ACCESS = 3, CONTROL = 4, CONTINUE = 5, ACK = 6,
       ACCOMP = 7, DATA = 8, STATUS = 9, KEYDEF = 10, ALLOC = 11, SUMMARY = 12,
       DATE = 13, PROTECT = 14, NAME = 15, ACL = 16 };

/* NAME message name types */
#define NT_FILESPEC  1
#define NT_FILENAME  2
#define NT_DIRECTORY 4
#define NT_VOLUME    8

#define TIMEOUT 30000L

struct msg {
	int type;
	const unsigned char *body;
	long len;
};

static const char *type_name(int t)
{
	static const char *names[] = { "?", "CONFIG", "ATTRIB", "ACCESS", "CONTROL",
		"CONTINUE", "ACK", "ACCOMP", "DATA", "STATUS", "KEYDEF", "ALLOC",
		"SUMMARY", "DATE", "PROTECT", "NAME", "ACL" };
	return (t > 0 && t <= ACL) ? names[t] : "?";
}

static void logf_(struct dap *d, const char *fmt, ...)
{
	char buf[200];
	va_list ap;

	if (!d->verbose || !d->log)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	d->log(d->ctx, buf);
}

static int fail(struct dap *d, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(d->error, sizeof d->error, fmt, ap);
	va_end(ap);
	logf_(d, "error: %s", d->error);
	return -1;
}

const char *dap_reason_text(int r)
{
	switch (r) {
	case 0:  return "normal disconnect";
	case 1:  return "remote node has no resources";
	case 2:  return "unknown node name";
	case 3:  return "remote node shutting down";
	case 4:  return "no such object on the remote node";
	case 6:  return "object too busy";
	case 8:  return "aborted by third party";
	case 9:  return "aborted by the remote end";
	case 32: case 33: return "too many connections";
	case 34: return "access denied (user name or password)";
	case 36: return "invalid account";
	case 38: return "no answer (timeout)";
	case 39: return "node unreachable";
	case 41: return "no link";
	default: return "connection failed";
	}
}

/* ---- framing -------------------------------------------------------------- */

static int next(struct dap *d, struct msg *m, long timeout)
{
	const unsigned char *b;
	int flags;
	long pos, len;

	if (d->rxpos >= d->rxlen) {
		d->rxlen = d->read(d->ctx, d->rx, sizeof d->rx, timeout);
		d->rxpos = 0;
		if (d->rxlen == 0)
			return fail(d, "timeout waiting for the server");
		if (d->rxlen < 0) {
			d->rxlen = 0;
			return fail(d, "link closed by the server");
		}
	}
	b = d->rx + d->rxpos;
	if (d->rxlen - d->rxpos < 2) {
		d->rxpos = d->rxlen;
		return fail(d, "short DAP message");
	}
	m->type = b[0];
	flags = b[1];
	pos = 2;
	if (flags & 0x01)                       /* STREAMID */
		pos++;
	if (flags & 0x02) {                     /* LENGTH */
		len = b[pos++];
		if (flags & 0x04)               /* LEN256 */
			len |= (long)b[pos++] << 8;
	} else {
		len = d->rxlen - d->rxpos - pos;  /* rest of the NSP message */
	}
	if (flags & 0x08) {                     /* BITCNT */
		pos++;
		len--;
	}
	if (flags & 0x20)
		return fail(d, "DAP SYSPEC field not supported");
	if (d->rxpos + pos + len > d->rxlen) {
		d->rxpos = d->rxlen;
		return fail(d, "DAP length beyond message");
	}
	m->body = b + pos;
	m->len = len;
	d->rxpos += pos + len;
	logf_(d, "<- %s (%ld bytes)", type_name(m->type), len);
	return 0;
}

static int send_msg(struct dap *d, int type, const unsigned char *body, long len)
{
	unsigned char buf[DAP_MAXREC + 16];
	long n = 0;

	if (len > (long)sizeof buf - 4)
		return fail(d, "DAP message too long");
	buf[n++] = (unsigned char)type;
	if (len < 256) {
		buf[n++] = 0x02;
		buf[n++] = (unsigned char)len;
	} else {
		buf[n++] = 0x06;
		buf[n++] = (unsigned char)(len & 0xff);
		buf[n++] = (unsigned char)(len >> 8);
	}
	memcpy(buf + n, body, len);
	n += len;
	logf_(d, "-> %s (%ld bytes)", type_name(type), len);
	if (d->write(d->ctx, buf, n) != 0)
		return fail(d, "link closed while sending");
	return 0;
}

/* ---- field helpers ----------------------------------------------------------- */

/* length of an extensible field */
static long ex_len(const unsigned char *p, long max)
{
	long i = 0;

	while (i < max && (p[i] & 0x80))
		i++;
	return i < max ? i + 1 : max;
}

static unsigned long ex_val(const unsigned char *p, long n)
{
	unsigned long v = 0;
	long i;
	int bit = 0;

	for (i = 0; i < n && bit < 28; i++, bit += 7)
		v |= (unsigned long)(p[i] & 0x7f) << bit;
	return v;
}

/* image field: count byte + little endian value */
static unsigned long img_val(const unsigned char **pp, const unsigned char *e)
{
	const unsigned char *p = *pp;
	unsigned long v = 0;
	int n, i;

	if (p >= e)
		return 0;
	n = *p++;
	for (i = 0; i < n && p + i < e; i++)
		if (i < 4)
			v |= (unsigned long)p[i] << (8 * i);
	*pp = p + n;
	return v;
}

static void img_str(const unsigned char **pp, const unsigned char *e, char *out, int size)
{
	const unsigned char *p = *pp;
	int n, c;

	if (p >= e) {
		out[0] = 0;
		return;
	}
	n = *p++;
	if (p + n > e)
		n = (int)(e - p);
	c = n < size - 1 ? n : size - 1;
	memcpy(out, p, c);
	out[c] = 0;
	*pp = p + n;
}

static int status_code(const struct msg *m)
{
	return m->len >= 2 ? (m->body[0] | (m->body[1] << 8)) : -1;
}

/* ATTRIB fields we use. Order and sizes as in dnprogs
   dap_attrib_message::read. */
struct attrib {
	int rfm, rat;
	long bytes, blocks;
};

static void parse_attrib(const struct msg *m, struct attrib *a)
{
	const unsigned char *p = m->body, *e = m->body + m->len;
	long ml = ex_len(p, e - p);
	unsigned long menu = ex_val(p, ml), ebk = 0, ffb = 0, alq = 0;

	a->rfm = -1;
	a->rat = 0;
	a->bytes = -1;
	a->blocks = -1;
	p += ml;
	if (menu & (1UL << 0)) p += ex_len(p, e - p);   /* DATATYPE */
	if (menu & (1UL << 1)) p++;                      /* ORG */
	if (menu & (1UL << 2)) a->rfm = *p++;            /* RFM */
	if (menu & (1UL << 3)) {                         /* RAT */
		a->rat = p[0] & 0x7f;
		p += ex_len(p, e - p);
	}
	if (menu & (1UL << 4)) p += 2;                   /* BLS */
	if (menu & (1UL << 5)) p += 2;                   /* MRS */
	if (menu & (1UL << 6)) alq = img_val(&p, e);     /* ALQ */
	if (menu & (1UL << 7)) p++;                      /* BKS */
	if (menu & (1UL << 8)) p++;                      /* FSZ */
	if (menu & (1UL << 9)) img_val(&p, e);           /* MRN */
	if (menu & (1UL << 10)) img_val(&p, e);          /* RUNSYS */
	if (menu & (1UL << 11)) p += 2;                  /* DEQ */
	if (menu & (1UL << 12)) p += ex_len(p, e - p);  /* FOP */
	if (menu & (1UL << 13)) p++;                     /* BSZ */
	if (menu & (1UL << 14)) p += ex_len(p, e - p);  /* DEV */
	if (menu & (1UL << 15)) p += ex_len(p, e - p);  /* SDC */
	if (menu & (1UL << 16)) p += 2;                  /* LRL */
	if (menu & (1UL << 17)) img_val(&p, e);          /* HBK */
	if (menu & (1UL << 18)) ebk = img_val(&p, e);    /* EBK */
	if ((menu & (1UL << 19)) && p + 2 <= e)          /* FFB */
		ffb = p[0] | (p[1] << 8);
	if (p > e)
		return;
	if (menu & (1UL << 6))
		a->blocks = (long)alq;
	if ((menu & (1UL << 18)) && ebk > 0)
		a->bytes = (long)(ebk - 1) * 512 + (long)ffb;
}

/* ---- CONFIG -------------------------------------------------------------------- */

int dap_config(struct dap *d, int blocking)
{
	unsigned char cfg[] = {
		CONFIG, 0x00,                   /* no length field */
		0x00, 0x10,                     /* BUFSIZ 4096 */
		0x07,                           /* OSTYPE VAX/VMS (as dnprogs does) */
		0x03,                           /* FILESYS RMS-32 */
		0x07, 0x02, 0x00, 0x05, 0x00,   /* VERSION 7.2.0.5.0 */
		0xa2, 0xf9, 0xf9, 0xf4, 0xaa, 0x2c  /* SYSCAP (dnprogs) */
	};
	struct msg m;

	/* SYSCAP bit 18 "blocking of DAP messages up to response": FOTOFX
	   then never answers a failed ACCESS; VMS is much faster with it */
	if (!blocking)
		cfg[13] &= ~0x10;
	d->rxlen = d->rxpos = 0;
	d->status = 0;
	logf_(d, "-> CONFIG");
	if (d->write(d->ctx, cfg, sizeof cfg) != 0)
		return fail(d, "link closed while sending");
	if (next(d, &m, TIMEOUT))
		return -1;
	if (m.type != CONFIG || m.len < 9)
		return fail(d, "protocol error: CONFIG expected, got %s", type_name(m.type));
	d->bufsiz = m.body[0] | (m.body[1] << 8);
	d->ostype = m.body[2];
	d->filesys = m.body[3];
	d->version = m.body[4];
	d->eco = m.body[5];
	logf_(d, "server: ostype %d filesys %d DAP %d.%d bufsiz %u",
	      d->ostype, d->filesys, d->version, d->eco, d->bufsiz);
	return 0;
}

/* ---- ACCESS -------------------------------------------------------------------- */

static int send_access_fac(struct dap *d, int func, int opt, const char *spec, int fac,
                           const unsigned char *display, int displen)
{
	unsigned char body[300];
	long n = 0, sl = (long)strlen(spec);

	if (sl > 255)
		return fail(d, "file name too long");
	body[n++] = (unsigned char)func;        /* ACCFUNC */
	body[n++] = (unsigned char)opt;         /* ACCOPT */
	body[n++] = (unsigned char)sl;          /* FILESPEC */
	memcpy(body + n, spec, sl);
	n += sl;
	body[n++] = (unsigned char)fac;         /* FAC */
	body[n++] = 0x02;                       /* SHR = FB$GET */
	memcpy(body + n, display, displen);     /* DISPLAY */
	n += displen;
	return send_msg(d, ACCESS, body, n);
}

static int send_access(struct dap *d, int func, int opt, const char *spec,
                       const unsigned char *display, int displen)
{
	return send_access_fac(d, func, opt, spec, 0x02, display, displen);   /* FB$GET */
}

static int open_error(struct dap *d, const char *spec, int c)
{
	d->status = c;
	if (c == 0x4032)                        /* open error, RMS$_FNF */
		return fail(d, "%s: file not found", spec);
	if (c == 0x4020)                        /* open error, RMS$_DNF */
		return fail(d, "%s: directory not found", spec);
	if ((c & 0xfff) == 0227)                /* RMS$_SYN (octal like the spec) */
		return fail(d, "%s: name not valid on the server", spec);
	if (((c >> 12) & 0xf) == 4)
		return fail(d, "%s: cannot open (DAP status %04x)", spec, c);
	return fail(d, "%s: server error (DAP status %04x)", spec, c);
}

/* ---- directory ------------------------------------------------------------------ */

int dap_directory(struct dap *d, const char *spec,
                  int (*cb)(void *ud, const struct dap_dirent *e), void *ud)
{
	static const unsigned char display[] = { 0x31 };   /* MAIN|DATE|PROT */
	struct dap_dirent e;
	struct msg m;
	int pending = 0;

	memset(&e, 0, sizeof e);
	e.bytes = e.blocks = -1;
	if (send_access(d, 6, 1, spec, display, sizeof display))   /* DIRECTORY */
		return -1;
	for (;;) {
		if (next(d, &m, TIMEOUT))
			return -1;
		switch (m.type) {
		case NAME: {
			const unsigned char *p = m.body, *end = m.body + m.len;
			long tl = ex_len(p, end - p);
			unsigned long t = ex_val(p, tl);
			char s[200];

			p += tl;
			img_str(&p, end, s, sizeof s);
			if (t & NT_VOLUME) {
				snprintf(e.volume, sizeof e.volume, "%s", s);
			} else if (t & NT_DIRECTORY) {
				snprintf(e.directory, sizeof e.directory, "%s", s);
			} else if (t & NT_FILENAME) {
				strncpy(e.name, s, sizeof e.name - 1);
				e.name[sizeof e.name - 1] = 0;
				e.bytes = e.blocks = -1;
				e.created[0] = e.owner[0] = 0;
				pending = 1;
			}
			break;
		}
		case ATTRIB: {
			struct attrib a;
			parse_attrib(&m, &a);
			e.bytes = a.bytes;
			e.blocks = a.blocks;
			break;
		}
		case DATE: {
			long ml = ex_len(m.body, m.len);
			if ((ex_val(m.body, ml) & 1) && ml + 18 <= m.len) {   /* CDT */
				memcpy(e.created, m.body + ml, 18);
				e.created[18] = 0;
			}
			break;
		}
		case PROTECT: {
			const unsigned char *p = m.body, *end = m.body + m.len;
			long ml = ex_len(p, end - p);
			if (ex_val(p, ml) & 1) {                            /* OWNER */
				p += ml;
				img_str(&p, end, e.owner, sizeof e.owner);
			}
			break;
		}
		case ACK:
			if (pending) {
				pending = 0;
				if (cb(ud, &e))
					return 0;
			}
			break;
		case STATUS: {
			int c = status_code(&m);
			if (c == 0x4030 && pending) {               /* locked: skip it */
				unsigned char skip = 2;
				pending = 0;
				if (send_msg(d, CONTINUE, &skip, 1))
					return -1;
				break;
			}
			return open_error(d, spec, c);
		}
		case ACCOMP:
			return 0;
		default:
			logf_(d, "ignoring %s", type_name(m.type));
		}
	}
}

/* ---- file transfer ---------------------------------------------------------------- */

int dap_open_read(struct dap *d, const char *spec, struct dap_file *f)
{
	/* ATTRIB as dncopy sends it before opening a file for reading (VMS FAL
	   answers ACCESS with a sync error without it): menu DATATYPE|ORG|RFM|
	   MRS|FOP, datatype 1, SEQ, VAR, MRS 0, FOP SQO */
	static const unsigned char attrib[] = { 0xa7, 0x20, 0x01, 0x00, 0x02,
	                                        0x00, 0x00, 0x80, 0x80, 0x10 };
	static const unsigned char display[] = { 0xa1, 0x02 };   /* MAIN|PROT|NAME */
	struct attrib a;
	struct msg m;
	unsigned char body[3];

	a.rfm = -1;
	a.rat = 0;
	a.bytes = a.blocks = -1;
	if (send_msg(d, ATTRIB, attrib, sizeof attrib))
		return -1;
	if (send_access(d, 1, 0, spec, display, sizeof display))   /* OPEN */
		return -1;
	for (;;) {
		if (next(d, &m, TIMEOUT))
			return -1;
		if (m.type == ACK)
			break;
		if (m.type == STATUS)
			return open_error(d, spec, status_code(&m));
		if (m.type == ATTRIB)
			parse_attrib(&m, &a);
	}
	f->size = a.bytes;
	f->rfm = a.rfm;
	f->rat = a.rat;
	/* FB$CR = implied carriage control; stream formats (STM, STMLF,
	   STMCR) already carry the terminator inside the record */
	f->text = (a.rat & 0x02) != 0 && a.rfm >= 1 && a.rfm <= 3;
	logf_(d, "rfm %d rat %02x size %ld: %s", a.rfm, a.rat, a.bytes,
	      f->text ? "text records" : "byte stream");

	body[0] = 2;                            /* CONTROL connect */
	body[1] = 0;
	if (send_msg(d, CONTROL, body, 2) || next(d, &m, TIMEOUT))
		return -1;
	if (m.type == STATUS) {
		d->status = status_code(&m);
		return fail(d, "server error (DAP status %04x)", d->status);
	}
	if (m.type != ACK)
		return fail(d, "protocol error: ACK expected, got %s", type_name(m.type));

	body[0] = 1;                            /* CONTROL get, RAC = SEQFT */
	body[1] = 0x01;
	body[2] = 3;
	return send_msg(d, CONTROL, body, 3);
}

int dap_read_record(struct dap *d, const unsigned char **p, long *n)
{
	struct msg m;
	long i;

	if (next(d, &m, TIMEOUT))
		return -1;
	switch (m.type) {
	case DATA:
		i = 1 + (m.len > 0 ? m.body[0] : 0);   /* skip RECNUM */
		if (i > m.len)
			i = m.len;
		*p = m.body + i;
		*n = m.len - i;
		return 1;
	case STATUS:
		d->status = status_code(&m);
		if ((d->status & 0xfff) == 047)         /* RMS$_EOF */
			return 0;
		return fail(d, "transfer aborted (DAP status %04x)", d->status);
	default:
		return fail(d, "protocol error: unexpected %s", type_name(m.type));
	}
}

int dap_close_file(struct dap *d)
{
	struct msg m;
	unsigned char func = 1;                 /* ACCOMP close */

	if (send_msg(d, ACCOMP, &func, 1))
		return -1;
	/* data still in flight after an abort is skipped until the answer */
	for (;;) {
		if (next(d, &m, TIMEOUT))
			return -1;
		if (m.type == ACCOMP)
			return 0;
		if (m.type == STATUS && (status_code(&m) & 0xfff) != 047) {
			logf_(d, "close: DAP status %04x", status_code(&m));
			return 0;
		}
	}
}

int dap_get(struct dap *d, const char *spec,
            int (*cb)(void *ud, const unsigned char *p, long n, int text), void *ud,
            long *size)
{
	struct dap_file f;
	const unsigned char *p;
	long n;
	int r, rc = -1;

	if (size)
		*size = 0;
	if (dap_open_read(d, spec, &f))
		return -1;
	if (size && f.size > 0)
		*size = f.size;
	while ((r = dap_read_record(d, &p, &n)) > 0) {
		if (cb(ud, p, n, f.text)) {
			fail(d, "transfer cancelled");
			break;
		}
	}
	if (r == 0)
		rc = 0;
	if (r >= 0)
		dap_close_file(d);
	return rc;
}

/* ---- writing ---------------------------------------------------------------------- */

int dap_open_write(struct dap *d, const char *spec, int text, char *name, int namesize)
{
	/* menu DATATYPE|ORG|RFM|RAT|BLS|MRS; SEQ, VAR; text: ASCII with
	   implied carriage control (VMS TYPE shows lines); binary: IMAGE
	   without carriage control, read back byte exact */
	unsigned char attrib[] = { 0x3f, 0x01, 0x00, 0x02, 0x02, 0x00, 0x02, 0x00, 0x00 };
	static const unsigned char alloc[] = { 0x00 };              /* empty menu */
	static const unsigned char display[] = { 0xa1, 0x02 };      /* MAIN|PROT|NAME */
	unsigned char body[3];
	struct msg m;

	if (!text) {
		attrib[1] = 0x02;               /* IMAGE */
		attrib[4] = 0x00;               /* no RAT */
	}
	if (name && namesize > 0)
		name[0] = 0;
	if (send_msg(d, ATTRIB, attrib, sizeof attrib))
		return -1;
	if (d->ostype == 7 && send_msg(d, ALLOC, alloc, sizeof alloc))   /* VMS likes one */
		return -1;
	if (send_access_fac(d, 2, 0, spec, 0x01, display, sizeof display))   /* CREATE, FB$PUT */
		return -1;
	for (;;) {
		if (next(d, &m, TIMEOUT))
			return -1;
		if (m.type == ACK)
			break;
		if (m.type == STATUS)
			return open_error(d, spec, status_code(&m));
		if (m.type == NAME && name && namesize > 0) {
			const unsigned char *p = m.body, *end = m.body + m.len;
			p += ex_len(p, end - p);
			img_str(&p, end, name, namesize);
		}
	}
	logf_(d, "created %s (%s)", name && name[0] ? name : spec, text ? "text" : "binary");

	body[0] = 2;                            /* CONTROL connect */
	body[1] = 0;
	if (send_msg(d, CONTROL, body, 2) || next(d, &m, TIMEOUT))
		return -1;
	if (m.type == STATUS) {
		d->status = status_code(&m);
		return fail(d, "server error (DAP status %04x)", d->status);
	}
	if (m.type != ACK)
		return fail(d, "protocol error: ACK expected, got %s", type_name(m.type));

	body[0] = 4;                            /* CONTROL put, RAC = SEQFT */
	body[1] = 0x01;
	body[2] = 3;
	return send_msg(d, CONTROL, body, 3);
}

int dap_write_record(struct dap *d, const unsigned char *p, long n)
{
	unsigned char body[DAP_MAXREC + 1];

	if (n > DAP_MAXREC)
		return fail(d, "record too long");
	body[0] = 0;                            /* RECNUM: empty image field */
	memcpy(body + 1, p, n);
	return send_msg(d, DATA, body, n + 1);
}

int dap_close_write(struct dap *d)
{
	struct msg m;
	unsigned char func = 1;                 /* ACCOMP close */

	if (send_msg(d, ACCOMP, &func, 1))
		return -1;
	for (;;) {
		if (next(d, &m, TIMEOUT))
			return -1;
		if (m.type == ACCOMP)
			return 0;
		if (m.type == STATUS) {
			d->status = status_code(&m);
			if ((d->status & 0xfff) == 0225 ||      /* SUC */
			    (d->status & 0xfff) == 0226)        /* SPRSED */
				continue;
			return fail(d, "write failed (DAP status %04x)", d->status);
		}
	}
}

/* ERASE or RENAME: the server answers with ACCOMP or STATUS */
static int simple_access(struct dap *d, int func, const char *spec, const char *newspec)
{
	static const unsigned char display[] = { 0x00 };
	struct msg m;

	if (send_access_fac(d, func, 1, spec, 0x00, display, sizeof display))
		return -1;
	if (newspec) {
		unsigned char body[260];
		long sl = (long)strlen(newspec);

		if (sl > 255)
			return fail(d, "file name too long");
		body[0] = NT_FILESPEC;
		body[1] = (unsigned char)sl;
		memcpy(body + 2, newspec, sl);
		if (send_msg(d, NAME, body, sl + 2))
			return -1;
	}
	for (;;) {
		if (next(d, &m, TIMEOUT))
			return -1;
		if (m.type == ACCOMP)
			return 0;
		if (m.type == STATUS)
			return open_error(d, spec, status_code(&m));
	}
}

int dap_erase(struct dap *d, const char *spec)
{
	return simple_access(d, 4, spec, NULL);
}

int dap_rename(struct dap *d, const char *from, const char *to)
{
	return simple_access(d, 3, from, to);
}

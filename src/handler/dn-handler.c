/*
 * dn-handler - DN: file system: files on DECnet nodes via DAP / FAL.
 *
 *   DN:                    the nodes from ENV:DECnet/nodes, as directories
 *   DN:HOWIE               login directory on HOWIE
 *   DN:HOWIE/SUB/FILE.TXT  [.SUB]FILE.TXT (VMS, newest version)
 *   DN:FOTOFX/[MUSTER]/X   a component with '[' is taken as a VMS directory
 *
 * Access data per node from ENV:DECnet/nodes (NAME ADDR [USER name]
 * [PASSWORD word] [NOBLOCK] [PURGE] [SHOW], keywords in any order). PURGE: after a file was written, its older VMS versions
 * are deleted, as an Amiga overwrite would; without it they are kept.
 * SHOW: the node appears as a drawer in DN:; if no node has SHOW, all do.
 * Nodes not shown are still reachable by name (DN:COLT).
 * Files can be read, written (a new file, on VMS a new version), deleted
 * (all versions) and renamed. Writing decides at the first Write() whether
 * the data is text (stored as lines, readable on VMS) or binary (stored
 * byte exact). Directories cannot be created: DAP has no function for it.
 * Protection bits, comments and dates are accepted and ignored, so copies
 * by Workbench and C:Copy do not fail on them.
 * Each directory listing and each open file uses its own DAP connection
 * through decnet.library (dnetd must be running).
 * Debug log: ENV:DECnet/handler-debug holds the log file name (e.g.
 * Transfer:DECnet/dn-handler.log); empty means T:dn-handler.log.
 *
 * Mount with an entry like config/Mountlist (GlobVec -1). AmigaOS 2.04.
 * Entry point is start() in start.c, which must be linked first.
 *
 * Copyright (C) 2026  Torsten
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/decnet.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../dap/dap.h"

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *DECnetBase;
/* libnix's 32 bit multiply/divide for the 68000 call utility.library; there
   is no C startup to open it, so the handler does */
struct Library *UtilityBase;


static const char version[] = "$VER: dn-handler 0.6 (23.10.2026)";

#define VOLNAME     "DECnet Nodes"   /* not "DECnet": that would hide the DECnet: assign */
#define CACHE_TICKS (10 * TICKS_PER_SECOND)
#define NCACHE      8
#define MAXPATH     256
#define SPECLEN     (MAXPATH * 3 + 16)  /* VMS spec: names may grow by $xx */

/* ---- debug log ------------------------------------------------------------ */

static int debug;
static char logname[108] = "T:dn-handler.log";

/* Debugging is on while ENV:DECnet/handler-debug exists. */
static void check_debug(void)
{
	BPTR fh = Open((STRPTR)"ENV:DECnet/handler-debug", MODE_OLDFILE);
	LONG n;

	debug = fh != 0;
	if (!fh)
		return;
	n = Read(fh, logname, sizeof logname - 1);
	Close(fh);
	while (n > 0 && (logname[n - 1] == '\n' || logname[n - 1] == ' '))
		n--;
	logname[n > 0 ? n : 0] = 0;
	if (!logname[0] || !strchr(logname, ':'))
		strcpy(logname, "T:dn-handler.log");
}

static void dlog(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	BPTR fh;

	if (!debug)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf - 1, fmt, ap);
	va_end(ap);
	strcat(buf, "\n");
	if ((fh = Open((STRPTR)logname, MODE_READWRITE)) != 0) {
		Seek(fh, 0, OFFSET_END);
		Write(fh, buf, strlen(buf));
		Close(fh);
	}
}

static void dap_logger(void *ctx, const char *msg)
{
	(void)ctx;
	dlog("dap: %s", msg);
}

/* ---- helpers ---------------------------------------------------------------- */

static int ieq(const char *a, const char *b)
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

static void bstr_to_c(BSTR b, char *out, int size)
{
	UBYTE *p = BADDR(b);
	int n = p ? p[0] : 0;

	if (n > size - 1)
		n = size - 1;
	if (p)
		memcpy(out, p + 1, n);
	out[n] = 0;
}

static void c_to_fib(char *dst, const char *src, int size)
{
	int n = strlen(src);

	if (n > size - 1)
		n = size - 1;
	dst[0] = (char)n;
	memcpy(dst + 1, src, n);
}

static ULONG hash(const char *s)
{
	ULONG h = 5381;

	for (; *s; s++) {
		char c = *s;
		if (c >= 'a' && c <= 'z')
			c -= 32;
		h = h * 33 + (UBYTE)c;
	}
	return h ? h : 1;
}

/* unsigned decimal at *pp; -1 if none */
static LONG num(const char **pp)
{
	const char *p = *pp;
	LONG v = 0;

	if (*p < '0' || *p > '9')
		return -1;
	while (*p >= '0' && *p <= '9')
		v = v * 10 + (*p++ - '0');
	*pp = p;
	return v;
}

/* "area.node" -> address, 0 if not a node address */
static UWORD parse_addr(const char *s)
{
	LONG a, n;

	if ((a = num(&s)) < 1 || a > 63 || *s++ != '.' || (n = num(&s)) < 1 || n > 1023 || *s)
		return 0;
	return (UWORD)(a << 10 | n);
}

/* next blank separated word of *pp into out; FALSE if none */
static BOOL word(const char **pp, char *out, int size)
{
	const char *p = *pp;
	int n = 0;

	while (*p == ' ' || *p == '\t')
		p++;
	if (!*p || *p == '\n' || *p == '\r')
		return FALSE;
	while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
		if (n < size - 1)
			out[n++] = *p;
		p++;
	}
	out[n] = 0;
	*pp = p;
	return TRUE;
}

/* "17-OCT-26 21:44:34" or "17-OCT-2026 21:44:34" -> DateStamp */
static void parse_date(const char *s, struct DateStamp *ds)
{
	static const char months[] = "JANFEBMARAPRMAYJUNJULAUGSEPOCTNOVDEC";
	static const UWORD mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	LONG d, m = -1, y, hh = 0, mm = 0, ss = 0, days, i;
	char mon[4];

	ds->ds_Days = ds->ds_Minute = ds->ds_Tick = 0;
	if ((d = num(&s)) < 1 || *s++ != '-' || !s[0] || !s[1] || !s[2])
		return;
	memcpy(mon, s, 3);
	mon[3] = 0;
	s += 3;
	if (*s++ != '-' || (y = num(&s)) < 0)
		return;
	if (*s == ' ') {
		s++;
		hh = num(&s);
		if (*s == ':') { s++; mm = num(&s); }
		if (*s == ':') { s++; ss = num(&s); }
		if (hh < 0 || mm < 0 || ss < 0)
			hh = mm = ss = 0;
	}
	for (i = 0; i < 12; i++) {
		char x[4];
		memcpy(x, months + 3 * i, 3);
		x[3] = 0;
		if (ieq(mon, x))
			m = i;
	}
	if (m < 0)
		return;
	if (y < 100)
		y += y < 78 ? 2000 : 1900;
	if (y < 1978)
		return;
	days = 0;
	for (i = 1978; i < y; i++)
		days += ((i % 4 == 0 && i % 100 != 0) || i % 400 == 0) ? 366 : 365;
	for (i = 0; i < m; i++)
		days += mdays[i] + (i == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
	days += d - 1;
	ds->ds_Days = days;
	ds->ds_Minute = hh * 60 + mm;
	ds->ds_Tick = ss * TICKS_PER_SECOND;
}

/* ---- node table ------------------------------------------------------------------ */

struct nodecfg {
	char name[24];
	UWORD addr;
	char user[40], pass[40];
	int noblock;
	int purge;                  /* delete older versions after writing */
	int show;                   /* listed in the DN: root */
};

/* Words after the address: USER name, PASSWORD word, NOBLOCK, PURGE,
   SHOW, in any order. The keywords keep a user or password that happens
   to read SHOW from being taken for an option. Unknown words are skipped. */
static void parse_opts(char f[][40], int n, struct nodecfg *cfg)
{
	int k;

	for (k = 2; k < n; k++) {
		if (ieq(f[k], "USER") && k + 1 < n)
			strcpy(cfg->user, f[++k]);
		else if (ieq(f[k], "PASSWORD") && k + 1 < n)
			strcpy(cfg->pass, f[++k]);
		else if (ieq(f[k], "NOBLOCK"))
			cfg->noblock = 1;
		else if (ieq(f[k], "PURGE"))
			cfg->purge = 1;
		else if (ieq(f[k], "SHOW"))
			cfg->show = 1;
	}
}

/* Fill cfg for name (name may also be "23.208"). Returns FALSE if unknown.
   With list != NULL, calls list(name, show) for every node instead. */
static BOOL nodes(const char *name, struct nodecfg *cfg, void (*list)(const char *, int, void *),
                  void *ud)
{
	char line[200], f[10][40];   /* name, address, then USER x PASSWORD y options */
	struct nodecfg tmp;
	BPTR fh;
	BOOL found = FALSE;
	UWORD addr;

	if (name && (addr = parse_addr(name)) != 0) {
		memset(cfg, 0, sizeof *cfg);
		strncpy(cfg->name, name, sizeof cfg->name - 1);
		cfg->addr = addr;
		found = TRUE;               /* still look for access data */
	}
	fh = Open((STRPTR)"ENV:DECnet/nodes", MODE_OLDFILE);
	if (!fh)
		return found;
	while (FGets(fh, (STRPTR)line, sizeof line)) {
		const char *p = line;
		int k;
		if (line[0] == ';' || line[0] == '#')
			continue;
		memset(f, 0, sizeof f);
		for (k = 0; k < 10 && word(&p, f[k], sizeof f[k]); k++)
			;
		if (k < 2 || (addr = parse_addr(f[1])) == 0)
			continue;
		if (list) {
			memset(&tmp, 0, sizeof tmp);
			parse_opts(f, k, &tmp);
			list(f[0], tmp.show, ud);
			continue;
		}
		if (!ieq(f[0], name) && !(found && cfg->addr == addr))
			continue;
		memset(cfg, 0, sizeof *cfg);
		strncpy(cfg->name, f[0], sizeof cfg->name - 1);
		cfg->addr = addr;
		parse_opts(f, k, cfg);
		found = TRUE;
		break;
	}
	Close(fh);
	return found;
}

/* ---- DAP connections -------------------------------------------------------------- */

struct conn {
	struct DnLink *link;
	struct dap *d;
};

static long c_read(void *ctx, unsigned char *buf, long max, long timeout)
{
	return DnRead(((struct conn *)ctx)->link, buf, max, timeout);
}

static int c_write(void *ctx, const unsigned char *buf, long n)
{
	return DnWrite(((struct conn *)ctx)->link, buf, n) == 0 ? 0 : -1;
}

static void conn_close(struct conn *c)
{
	if (c->d)
		FreeVec(c->d);
	if (c->link)
		DnClose(c->link);
	c->d = NULL;
	c->link = NULL;
}

/* Connect to FAL on the node and exchange CONFIG. Returns a DOS error. */
static LONG conn_open(struct conn *c, const struct nodecfg *cfg)
{
	struct DnConnectReq req;

	memset(c, 0, sizeof *c);
	if (!DECnetBase && !(DECnetBase = OpenLibrary((STRPTR)DECNETNAME, DECNET_VERSION)))
		return ERROR_OBJECT_NOT_FOUND;
	memset(&req, 0, sizeof req);
	req.dcr_Node = cfg->addr;
	req.dcr_ObjNum = DAP_OBJECT;
	req.dcr_SrcName = (CONST_STRPTR)"DN-HANDLER";
	req.dcr_User = (CONST_STRPTR)cfg->user;
	req.dcr_Password = (CONST_STRPTR)cfg->pass;
	c->link = DnConnect(&req);
	if (!c->link) {
		dlog("connect %s: reason %ld", cfg->name, req.dcr_Reason);
		return req.dcr_Reason == DNREASON_ACCESS ? ERROR_READ_PROTECTED : ERROR_OBJECT_NOT_FOUND;
	}
	c->d = AllocVec(sizeof(struct dap), MEMF_ANY | MEMF_CLEAR);
	if (!c->d) {
		conn_close(c);
		return ERROR_NO_FREE_STORE;
	}
	c->d->read = c_read;
	c->d->write = c_write;
	c->d->log = dap_logger;
	c->d->verbose = debug;
	c->d->ctx = c;
	if (dap_config(c->d, !cfg->noblock)) {
		dlog("config %s: %s", cfg->name, c->d->error);
		conn_close(c);
		return ERROR_OBJECT_NOT_FOUND;
	}
	return 0;
}

/* ---- paths ------------------------------------------------------------------------- */

/* A path is "NODE/comp/comp"; "" is the root (list of nodes). */

static void path_node(const char *path, char *node)
{
	int n = 0;

	while (path[n] && path[n] != '/' && n < 23) {
		node[n] = path[n];
		n++;
	}
	node[n] = 0;
}

static const char *path_rest(const char *path)
{
	const char *p = strchr(path, '/');
	return p ? p + 1 : "";
}

static const char *path_last(const char *path)
{
	const char *p = strrchr(path, '/');
	return p ? p + 1 : path;
}

/* Resolve name relative to base into out. FALSE if it climbs above root. */
static BOOL path_join(const char *base, const char *name, char *out)
{
	const char *c = strchr(name, ':');

	if (c) {
		name = c + 1;
		base = "";
	}
	strcpy(out, base);
	while (*name) {
		const char *e = strchr(name, '/');
		int len = e ? e - name : (int)strlen(name);
		if (len == 0) {                         /* "/" = parent */
			char *s = strrchr(out, '/');
			if (s)
				*s = 0;
			else if (out[0])
				out[0] = 0;
			else
				return FALSE;
		} else {
			if (strlen(out) + len + 2 > MAXPATH)
				return FALSE;
			if (out[0])
				strcat(out, "/");
			strncat(out, name, len);
		}
		name += len;
		if (*name == '/')
			name++;
	}
	return TRUE;
}

/* Amiga names -> VMS (ODS-2) names. VMS allows A-Z 0-9 $ _ - and one dot
   between name and type; everything else, further dots and '$' itself
   become "$xx" (hex), so "Bild.iff.info" is stored as BILD$2EIFF.INFO and
   listed again as BILD.IFF.INFO. file = 1 keeps the last dot as the type
   separator; directory names have no type. */
static void vms_name(const char *in, int n, int file, char *out)
{
	static const char hex[] = "0123456789ABCDEF";
	const char *lastdot = NULL;
	int i, o = 0;

	if (file)
		for (i = 0; i < n; i++)
			if (in[i] == '.')
				lastdot = in + i;
	for (i = 0; i < n; i++) {
		char c = in[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		    c == '_' || c == '-' || (c == '.' && in + i == lastdot)) {
			out[o++] = c;
		} else {
			out[o++] = '$';
			out[o++] = hex[(UBYTE)c >> 4];
			out[o++] = hex[c & 15];
		}
	}
	out[o] = 0;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

/* Reverse of vms_name() for a listed name (in place). An empty type
   ("README.") loses its dot, as the file was written without one. */
static void amiga_name(char *s)
{
	char *r = s, *w = s;
	int n = strlen(s);

	if (n > 1 && s[n - 1] == '.')
		s[n - 1] = 0;
	while (*r) {
		int h, l;
		if (r[0] == '$' && (h = hexval(r[1])) >= 0 && (l = hexval(r[2])) >= 0 &&
		    (h << 4 | l) >= 32) {
			*w++ = (char)(h << 4 | l);
			r += 3;
		} else {
			*w++ = *r++;
		}
	}
	*w = 0;
}

/* VMS directory spec for the directory components in rest[0..len):
   "" for the login directory, "[.A.B]" relative, "[X.B]" if the first
   component is itself a "[X]" spec. */
static void vms_dir(const char *rest, int len, char *out)
{
	char comp[MAXPATH];
	const char *p = rest, *end = rest + len;
	int first = 1, absolute = 0;

	out[0] = 0;
	while (p < end) {
		const char *e = memchr(p, '/', end - p);
		int n = e ? e - p : end - p;
		memcpy(comp, p, n);
		comp[n] = 0;
		if (first && comp[0] == '[') {
			absolute = 1;
			strcpy(out, comp);
			if (out[strlen(out) - 1] == ']')
				out[strlen(out) - 1] = 0;
		} else {
			char enc[MAXPATH * 3];
			vms_name(comp, n, 0, enc);
			strcat(out, first ? "[." : ".");
			strcat(out, enc);
		}
		first = 0;
		p += n + 1;
	}
	if (out[0])
		strcat(out, "]");
	(void)absolute;
}

/* Remote spec for a file (or the files of a directory if list). */
static void remote_spec(const char *path, int list, char *out)
{
	const char *rest = path_rest(path);
	const char *last = path_last(rest);

	if (list) {
		vms_dir(rest, strlen(rest), out);
		strcat(out, "*.*;0");
		return;
	}
	if (strchr(last, '[') && last == rest) {        /* raw VMS spec */
		strcpy(out, last);
		return;
	}
	vms_dir(rest, last - rest - (last > rest ? 1 : 0), out);
	vms_name(last, strlen(last), 1, out + strlen(out));
}

/* ---- directory listings with a small cache ---------------------------------------- */

struct dent {
	char name[108];
	LONG size;
	int isdir;
	struct DateStamp date;
	int icon;                   /* virtual NAME.info: ICON_x + 1; -1: has a real one */
};

struct dirlist {
	char path[MAXPATH];
	ULONG when;                 /* ticks */
	LONG n, max;
	struct dent *e;
	LONG error;                 /* DOS error if the listing failed */
	int users;
};

static struct dirlist *cache[NCACHE];

/* ---- icons --------------------------------------------------------------------------
   DN: delivers Disk.info (volume) and NODE.info (one per node in the root) from
   files the installer puts in DECnet:Icons, loaded once. VMS has no .info
   files, and without them Workbench would only show the nodes with "Show all
   files". Without DECnet:Icons, Workbench uses its default icons. */

#define ICON_DISK 0
#define ICON_NODE 1
#define ICON_DIR  2
#define ICON_TEXT 3
#define ICON_PIC  4
#define ICON_FILE 5

static struct {
	const char *file;
	UBYTE *data;
	LONG size;
	int tried;
} icons[] = {
	{ "DECnet:Icons/Disk.info" },
	{ "DECnet:Icons/Node.info" },
	{ "DECnet:Icons/Dir.info" },
	{ "DECnet:Icons/Text.info" },
	{ "DECnet:Icons/Picture.info" },
	{ "DECnet:Icons/File.info" },
};

static BOOL icon_load(int k)
{
	BPTR fh;
	LONG n;

	if (icons[k].tried)
		return icons[k].data != NULL;
	icons[k].tried = 1;
	if (!(fh = Open((STRPTR)icons[k].file, MODE_OLDFILE)))
		return FALSE;
	Seek(fh, 0, OFFSET_END);
	n = Seek(fh, 0, OFFSET_BEGINNING);
	if (n > 0 && n < 65536 && (icons[k].data = AllocVec(n, MEMF_ANY)) != NULL) {
		if (Read(fh, icons[k].data, n) == n) {
			icons[k].size = n;
		} else {
			FreeVec(icons[k].data);
			icons[k].data = NULL;
		}
	}
	Close(fh);
	dlog("icon %s: %ld bytes", icons[k].file, icons[k].size);
	return icons[k].data != NULL;
}

static BOOL list_add(struct dirlist *l, const char *name, LONG size, int isdir,
                     const struct DateStamp *ds);

static BOOL ends(const char *s, const char *ext)
{
	int n = strlen(s), m = strlen(ext);

	return n > m && ieq(s + n - m, ext);
}

/* Icon for a listing entry, by type and name extension. */
static int icon_kind(const struct dent *e)
{
	static const char *text[] = {
		".TXT", ".DOC", ".LIS", ".LOG", ".COM", ".MEM", ".RNO", ".HLP", ".MAI",
		".C", ".H", ".MAR", ".FOR", ".PAS", ".BAS", ".BLI", ".COB", ".LSP",
		".PL1", ".CLD", ".OPT", ".MMS", ".SDML", ".TEX", ".HTML", ".HTM",
		".ASC", ".ME", ".1ST", ".README", ".GUIDE", ".REXX", ".INI", ".CFG", NULL
	};
	static const char *pic[] = {
		".IFF", ".ILBM", ".GIF", ".JPG", ".JPEG", ".PNG", ".BMP", ".PCX", NULL
	};
	int i;

	if (e->isdir)
		return ICON_DIR;
	for (i = 0; text[i]; i++)
		if (ends(e->name, text[i]))
			return ICON_TEXT;
	for (i = 0; pic[i]; i++)
		if (ends(e->name, pic[i]))
			return ICON_PIC;
	return ICON_FILE;
}

/*
 * Add a virtual NAME.info for every entry of a listing that has no real
 * one (a .info stored on the node always wins). Root: node icons.
 */
static void add_icons(struct dirlist *l, int root)
{
	LONG i, j, n = l->n;
	char iname[112];

	for (i = 0; i < n; i++) {               /* entries with a real icon */
		int len = strlen(l->e[i].name);
		if (!ends(l->e[i].name, ".info"))
			continue;
		for (j = 0; j < n; j++)
			if ((int)strlen(l->e[j].name) == len - 5) {
				char base[108];
				memcpy(base, l->e[i].name, len - 5);
				base[len - 5] = 0;
				if (ieq(base, l->e[j].name))
					l->e[j].icon = -1;
			}
	}
	for (i = 0; i < n; i++) {
		int k = root ? ICON_NODE : icon_kind(&l->e[i]);

		if (l->e[i].icon || ends(l->e[i].name, ".info") ||
		    strlen(l->e[i].name) > sizeof iname - 6 || !icon_load(k))
			continue;
		strcpy(iname, l->e[i].name);
		strcat(iname, ".info");
		if (list_add(l, iname, icons[k].size, 0, NULL))
			l->e[l->n - 1].icon = k + 1;
	}
}

/* The icon a root-level path stands for ("Disk.info", "HOWIE.info"), or -1. */
static int icon_of(const char *path)
{
	char node[24];
	struct nodecfg cfg;
	int n = strlen(path);

	if (strchr(path, '/') || n < 6 || !ieq(path + n - 5, ".info"))
		return -1;
	if (ieq(path, "Disk.info"))
		return icon_load(ICON_DISK) ? ICON_DISK : -1;
	if (n - 5 >= (int)sizeof node)
		return -1;
	memcpy(node, path, n - 5);
	node[n - 5] = 0;
	if (!nodes(node, &cfg, NULL, NULL) || parse_addr(node))
		return -1;
	return icon_load(ICON_NODE) ? ICON_NODE : -1;
}

static ULONG ticks(void)
{
	struct DateStamp ds;

	DateStamp(&ds);
	return (ULONG)ds.ds_Days * 24 * 60 * 60 * TICKS_PER_SECOND +
	       (ULONG)ds.ds_Minute * 60 * TICKS_PER_SECOND + (ULONG)ds.ds_Tick;
}

static void list_free(struct dirlist *l)
{
	if (l->e)
		FreeVec(l->e);
	FreeVec(l);
}

static BOOL list_add(struct dirlist *l, const char *name, LONG size, int isdir,
                     const struct DateStamp *ds)
{
	struct dent *e;

	if (l->n == l->max) {
		LONG max = l->max ? l->max * 2 : 32;
		struct dent *ne = AllocVec(max * sizeof(struct dent), MEMF_ANY);
		if (!ne)
			return FALSE;
		if (l->e) {
			CopyMem(l->e, ne, l->n * sizeof(struct dent));
			FreeVec(l->e);
		}
		l->e = ne;
		l->max = max;
	}
	e = &l->e[l->n++];
	strncpy(e->name, name, sizeof e->name - 1);
	e->name[sizeof e->name - 1] = 0;
	e->size = size;
	e->isdir = isdir;
	e->icon = 0;
	if (ds)
		e->date = *ds;
	else
		memset(&e->date, 0, sizeof e->date);
	return TRUE;
}

/* size holds the SHOW flag until get_list() has filtered the root */
static void add_node(const char *name, int show, void *ud)
{
	list_add(ud, name, show, 1, NULL);
}

static int add_entry(void *ud, const struct dap_dirent *de)
{
	struct dirlist *l = ud;
	char name[128], *p;
	struct DateStamp ds;
	int isdir = 0;
	LONG size;

	strcpy(name, de->name);
	if ((p = strchr(name, ';')) != NULL)            /* drop the version */
		*p = 0;
	if ((p = strrchr(name, '.')) != NULL && ieq(p, ".DIR")) {
		*p = 0;
		isdir = 1;
	}
	amiga_name(name);
	size = de->bytes >= 0 ? de->bytes : de->blocks >= 0 ? de->blocks * 512 : 0;
	parse_date(de->created, &ds);
	list_add(l, name, isdir ? 0 : size, isdir, &ds);
	return 0;
}

/* Listing of the directory path (cached CACHE_TICKS). NULL on no memory. */
static struct dirlist *get_list(const char *path)
{
	struct dirlist *l;
	struct nodecfg cfg;
	struct conn c;
	char node[24], spec[SPECLEN];
	int i, slot = -1;
	ULONG now = ticks();

	for (i = 0; i < NCACHE; i++) {
		l = cache[i];
		if (l && ieq(l->path, path) && now - l->when < CACHE_TICKS)
			return l;
	}
	l = AllocVec(sizeof *l, MEMF_ANY | MEMF_CLEAR);
	if (!l)
		return NULL;
	strcpy(l->path, path);
	l->when = now;
	if (!path[0]) {
		LONG j, n = 0;

		nodes(NULL, NULL, add_node, l);
		for (j = 0; j < l->n; j++)              /* any SHOW at all? */
			if (l->e[j].size)
				n++;
		if (n > 0) {                            /* then only those */
			n = 0;
			for (j = 0; j < l->n; j++)
				if (l->e[j].size)
					l->e[n++] = l->e[j];
			l->n = n;
		}
		for (j = 0; j < l->n; j++)
			l->e[j].size = 0;
		add_icons(l, 1);
	} else {
		path_node(path, node);
		if (!nodes(node, &cfg, NULL, NULL)) {
			l->error = ERROR_OBJECT_NOT_FOUND;
		} else if ((l->error = conn_open(&c, &cfg)) == 0) {
			remote_spec(path, 1, spec);
			if (dap_directory(c.d, spec, add_entry, l)) {
				dlog("dir %s: %s", spec, c.d->error);
				l->error = ERROR_OBJECT_NOT_FOUND;
			}
			if (!l->error)
				add_icons(l, 0);
			dlog("dir %s: %ld entries", spec, l->n);
			conn_close(&c);
		}
	}
	/* replace an unused or the oldest cache slot */
	for (i = 0; i < NCACHE; i++) {
		if (!cache[i]) {
			slot = i;
			break;
		}
		if (!cache[i]->users && (slot < 0 || cache[i]->when < cache[slot]->when))
			slot = i;
	}
	if (slot >= 0) {
		if (cache[slot] && !cache[slot]->users)
			list_free(cache[slot]);
		cache[slot] = l;
	}
	return l;
}

/* Forget the cached listings of path and of its parent after a change. */
static void list_changed(const char *path)
{
	char parent[MAXPATH], *s;
	int i;

	strcpy(parent, path);
	if ((s = strrchr(parent, '/')) != NULL)
		*s = 0;
	for (i = 0; i < NCACHE; i++)
		if (cache[i] && (ieq(cache[i]->path, path) || ieq(cache[i]->path, parent)))
			cache[i]->when = ticks() - CACHE_TICKS - 1;
}

/* ---- locks ------------------------------------------------------------------------- */

struct mylock {
	struct FileLock fl;         /* what DOS sees */
	char path[MAXPATH];
	int isdir;
	struct dent info;
	struct dirlist *list;       /* held during ExNext */
};

/* Clients send their packets to this port, not to the process port: the
   handler's own DOS calls (ENV:, log) get their replies on pr_MsgPort, and
   a client packet arriving there meanwhile would crash DOS (AN_AsyncPkt). */
static struct MsgPort *port;
static struct DeviceList *volume;

static struct mylock *new_lock(const char *path, int isdir, const struct dent *info)
{
	struct mylock *l = AllocVec(sizeof *l, MEMF_PUBLIC | MEMF_CLEAR);

	if (!l)
		return NULL;
	strcpy(l->path, path);
	l->isdir = isdir;
	if (info)
		l->info = *info;
	else
		strncpy(l->info.name, path[0] ? path_last(path) : VOLNAME, sizeof l->info.name - 1);
	l->info.isdir = isdir;
	l->fl.fl_Key = (LONG)hash(path);
	l->fl.fl_Access = SHARED_LOCK;
	l->fl.fl_Task = port;
	l->fl.fl_Volume = MKBADDR(volume);
	return l;
}

static void free_lock(struct mylock *l)
{
	if (!l)
		return;
	if (l->list)
		l->list->users--;
	FreeVec(l);
}

/* Find what path is. Returns 0 and fills *isdir / *info, or a DOS error. */
static LONG lookup(const char *path, int *isdir, struct dent *info)
{
	char parent[MAXPATH], node[24];
	const char *name;
	struct dirlist *l;
	struct nodecfg cfg;
	LONG i;

	memset(info, 0, sizeof *info);
	if ((i = icon_of(path)) >= 0) {
		strncpy(info->name, path, sizeof info->name - 1);
		info->size = icons[i].size;
		*isdir = info->isdir = 0;
		return 0;
	}
	if (!path[0] || !strchr(path, '/')) {
		*isdir = 1;
		if (path[0]) {                          /* a node */
			path_node(path, node);
			if (!nodes(node, &cfg, NULL, NULL))
				return ERROR_OBJECT_NOT_FOUND;
		}
		strncpy(info->name, path[0] ? path : VOLNAME, sizeof info->name - 1);
		info->isdir = 1;
		return 0;
	}
	strcpy(parent, path);
	*strrchr(parent, '/') = 0;
	name = path_last(path);
	l = get_list(parent);
	if (l && !l->error) {
		for (i = 0; i < l->n; i++)
			if (ieq(l->e[i].name, name)) {
				*info = l->e[i];
				*isdir = info->isdir;
				return 0;
			}
		/* a listing with entries is complete: no need to ask the node
		   (Workbench looks for a .info file for every entry) */
		if (l->n > 0)
			return ERROR_OBJECT_NOT_FOUND;
	}
	/* not in the parent listing (e.g. FOTOFX's root cannot be listed):
	   try it as a directory, then as a single file */
	l = get_list(path);
	if (l && !l->error && l->n > 0) {
		strncpy(info->name, name, sizeof info->name - 1);
		info->isdir = *isdir = 1;
		return 0;
	}
	if (l && l->error == ERROR_READ_PROTECTED)
		return l->error;
	{
		char spec[SPECLEN];
		struct conn c;
		struct dirlist one;
		LONG err;

		path_node(path, node);
		if (!nodes(node, &cfg, NULL, NULL))
			return ERROR_OBJECT_NOT_FOUND;
		if ((err = conn_open(&c, &cfg)) != 0)
			return err;
		memset(&one, 0, sizeof one);
		remote_spec(path, 0, spec);
		if (dap_directory(c.d, spec, add_entry, &one) == 0 && one.n > 0) {
			*info = one.e[0];
			strncpy(info->name, name, sizeof info->name - 1);
			*isdir = info->isdir;
			err = 0;
		} else {
			err = ERROR_OBJECT_NOT_FOUND;
		}
		if (one.e)
			FreeVec(one.e);
		conn_close(&c);
		return err;
	}
}

static void fill_fib(struct FileInfoBlock *fib, const struct dent *e, LONG key)
{
	memset(fib, 0, sizeof *fib);
	fib->fib_DiskKey = key;
	fib->fib_DirEntryType = fib->fib_EntryType = e->isdir ? ST_USERDIR : ST_FILE;
	c_to_fib(fib->fib_FileName, e->name, sizeof fib->fib_FileName);
	/* RWED for drawers, RWD for files (bits are inverted): VMS files are not
	   Amiga programs, and Workbench shows executables as tools */
	fib->fib_Protection = e->isdir ? 0 : FIBF_EXECUTE;
	fib->fib_Size = e->size;
	fib->fib_NumBlocks = (e->size + 511) / 512;
	fib->fib_Date = e->date;
}

/* ---- open files -------------------------------------------------------------------- */

struct myfile {
	char path[MAXPATH];
	struct nodecfg cfg;
	struct conn c;
	struct dap_file f;
	struct dent info;
	LONG pos;                   /* bytes delivered so far */
	UBYTE *buf;                 /* current record (+ newline) */
	LONG buflen, bufpos, bufsize;
	int eof;
	/* writing */
	int writing;
	int created;                /* CREATE sent (at the first Write) */
	int text;                   /* lines, else binary records */
	LONG werr;                  /* first DOS error while writing */
	UBYTE *wbuf;                /* record being collected, DAP_MAXREC */
	LONG wlen;
	/* an icon from DECnet:Icons instead of a remote file */
	const UBYTE *mem;
	LONG memsize;
	UBYTE *memown;              /* mem is a text file read ahead, ours to free */
	int exact;                  /* f.size is the exact stream size */
	int sink;                   /* a .info written by Workbench: discarded */
};

static LONG file_start(struct myfile *f)
{
	char spec[SPECLEN];
	LONG err;

	if ((err = conn_open(&f->c, &f->cfg)) != 0)
		return err;
	remote_spec(f->path, 0, spec);
	if (dap_open_read(f->c.d, spec, &f->f)) {
		dlog("open %s: %s", spec, f->c.d->error);
		conn_close(&f->c);
		return ERROR_OBJECT_NOT_FOUND;
	}
	f->pos = f->buflen = f->bufpos = 0;
	f->eof = 0;
	return 0;
}

static void file_stop(struct myfile *f)
{
	conn_close(&f->c);          /* dropping the link also ends the access */
}

/* Next record into f->buf. FALSE at EOF or on error. */
static BOOL file_fill(struct myfile *f)
{
	const unsigned char *p;
	long n;
	int r;

	if (f->eof || !f->c.d)
		return FALSE;
	r = dap_read_record(f->c.d, &p, &n);
	if (r <= 0) {
		f->eof = 1;
		if (r < 0)
			dlog("read %s: %s", f->path, f->c.d->error);
		return FALSE;
	}
	if (n + 1 > f->bufsize) {
		if (f->buf)
			FreeVec(f->buf);
		f->bufsize = n + 1 > 1024 ? n + 1 : 1024;
		f->buf = AllocVec(f->bufsize, MEMF_ANY);
		if (!f->buf) {
			f->bufsize = 0;
			f->eof = 1;
			return FALSE;
		}
	}
	CopyMem((APTR)p, f->buf, n);
	if (f->f.text)
		f->buf[n++] = '\n';
	f->buflen = n;
	f->bufpos = 0;
	return TRUE;
}

static LONG file_read(struct myfile *f, UBYTE *dst, LONG len)
{
	LONG done = 0;

	if (f->mem) {
		done = f->memsize - f->pos;
		if (done > len)
			done = len;
		if (dst && done > 0)
			CopyMem((APTR)(f->mem + f->pos), dst, done);
		f->pos += done;
		return done;
	}

	while (done < len) {
		LONG n;
		if (f->bufpos >= f->buflen && !file_fill(f))
			break;
		n = f->buflen - f->bufpos;
		if (n > len - done)
			n = len - done;
		if (dst)
			CopyMem(f->buf + f->bufpos, dst + done, n);
		f->bufpos += n;
		done += n;
	}
	f->pos += done;
	return done;
}

/* Sequential access only: backwards means reading again from the start. */
static LONG file_seek(struct myfile *f, LONG target, LONG *err)
{
	LONG old = f->pos;

	if (f->mem) {
		if (target > f->memsize) {
			*err = ERROR_SEEK_ERROR;
			return -1;
		}
		f->pos = target;
		return old;
	}
	if (target < f->pos) {
		file_stop(f);
		if ((*err = file_start(f)) != 0)
			return -1;
	}
	while (f->pos < target)
		if (file_read(f, NULL, target - f->pos > 65536 ? 65536 : target - f->pos) == 0)
			break;
	if (f->pos != target) {
		*err = ERROR_SEEK_ERROR;
		return -1;
	}
	return old;
}

#define MEMFILE_MAX (256 * 1024)

/* A text file from its start into memory, so that its size can be told
   without reading it twice. FALSE if it is larger than MEMFILE_MAX (or
   memory is short); then it has been read to the end. */
static BOOL file_to_mem(struct myfile *f)
{
	UBYTE *m = NULL, *nm;
	LONG cap = 0, n = 0, got;

	for (;;) {
		if (n == cap) {
			if (cap >= MEMFILE_MAX || !(nm = AllocVec(cap ? cap * 2 : 16384, MEMF_ANY)))
				break;
			if (m) {
				CopyMem(m, nm, n);
				FreeVec(m);
			}
			m = nm;
			cap = cap ? cap * 2 : 16384;
		}
		got = file_read(f, m + n, cap - n);
		n += got;
		if (n < cap) {                      /* end of file */
			file_stop(f);
			f->mem = f->memown = m;
			f->memsize = n;
			f->pos = 0;
			return TRUE;
		}
	}
	if (m)
		FreeVec(m);
	while (file_read(f, NULL, 65536) > 0)
		;
	return FALSE;
}

static LONG file_size(struct myfile *f, LONG *err)
{
	LONG here = f->pos, size;

	if (f->mem)
		return f->memsize;

	/* byte exact for UDF, FIX and stream files; VAR and VFC sizes count the
	   record length words, so count the data */
	if (f->exact || (!f->f.text && f->f.size >= 0 && f->f.rfm != 2 && f->f.rfm != 3))
		return f->f.size;
	if (here == 0 && file_to_mem(f))
		return f->memsize;
	while (file_read(f, NULL, 65536) > 0)       /* text: count it */
		;
	size = f->pos;
	file_seek(f, here, err);
	f->f.size = size;
	f->exact = 1;
	return size;
}

/* ---- writing ------------------------------------------------------------------------ */

/* Text if the bytes contain no control characters besides line ends,
   tab, form feed and escape. */
static int looks_text(const UBYTE *p, LONG n)
{
	LONG i;

	for (i = 0; i < n; i++) {
		UBYTE c = p[i];
		if (c < 32 && c != '\n' && c != '\r' && c != '\t' && c != '\f' && c != 27)
			return 0;
	}
	return 1;
}

static LONG dap_error(struct myfile *f, LONG fallback)
{
	int c = f->c.d ? f->c.d->status : 0;

	if (c == 0x4032 || c == 0x4020)                 /* FNF, DNF */
		return ERROR_DIR_NOT_FOUND;
	/* MICCODEs are the RMS error numbers, in octal in the DAP spec */
	switch (c & 0xfff) {
	case 0227:                                      /* SYN */
		return ERROR_INVALID_COMPONENT_NAME;
	case 0125:                                      /* PRV */
	case 0164:                                      /* WLK */
		return ERROR_WRITE_PROTECTED;
	case 065:                                       /* FUL */
	case 052:                                       /* EXT */
		return ERROR_DISK_FULL;
	}
	return fallback;
}

static LONG file_create(struct myfile *f, int text)
{
	char spec[SPECLEN], name[128];

	remote_spec(f->path, 0, spec);
	f->text = text;
	f->created = 1;
	if (dap_open_write(f->c.d, spec, text, name, sizeof name)) {
		dlog("create %s: %s", spec, f->c.d->error);
		return dap_error(f, ERROR_WRITE_PROTECTED);
	}
	dlog("create %s -> %s (%s)", spec, name, text ? "text" : "binary");
	return 0;
}

static LONG put_record(struct myfile *f)
{
	if (dap_write_record(f->c.d, f->wbuf, f->wlen)) {
		dlog("write %s: %s", f->path, f->c.d->error);
		return ERROR_DISK_FULL;
	}
	f->wlen = 0;
	return 0;
}

/* Collect data into records: lines for text, DAP_MAXREC blocks else. */
static LONG file_write(struct myfile *f, const UBYTE *src, LONG len, LONG *err)
{
	LONG i;

	if (f->sink) {
		f->pos += len;
		return len;
	}
	if (f->werr) {
		*err = f->werr;
		return -1;
	}
	if (!f->created && (f->werr = file_create(f, looks_text(src, len))) != 0) {
		*err = f->werr;
		return -1;
	}
	for (i = 0; i < len; i++) {
		if (f->text && src[i] == '\n') {
			f->werr = put_record(f);
		} else {
			f->wbuf[f->wlen++] = src[i];
			if (f->wlen == DAP_MAXREC)
				f->werr = put_record(f);
		}
		if (f->werr) {
			*err = f->werr;
			return -1;
		}
	}
	f->pos += len;
	f->info.size = f->pos;
	return len;
}

/* Finish a written file. Returns a DOS error, 0 if it is complete. */
static LONG file_finish(struct myfile *f)
{
	LONG err = f->werr;

	if (!err && !f->created)
		err = file_create(f, 1);                /* empty file */
	if (!err && f->wlen > 0)
		err = put_record(f);
	if (!err && dap_close_write(f->c.d)) {
		dlog("close %s: %s", f->path, f->c.d->error);
		err = dap_error(f, ERROR_DISK_FULL);
	}
	if (!err && f->cfg.purge) {
		/* ";-1" is the version before the newest one; repeat until the
		   server finds none (at most a few dozen) */
		char spec[SPECLEN];
		int i;

		remote_spec(f->path, 0, spec);
		strcat(spec, ";-1");
		for (i = 0; i < 64 && dap_erase(f->c.d, spec) == 0; i++)
			;
		dlog("purge %s: %d older versions deleted", spec, i);
	}
	list_changed(f->path);
	dlog("closed %s: %ld bytes, error %ld", f->path, f->pos, err);
	return err;
}

/* ---- packets ------------------------------------------------------------------------ */

static struct mylock *lock_of(BPTR b)
{
	return (struct mylock *)BADDR(b);
}

static LONG do_locate(struct DosPacket *pkt, LONG *err)
{
	struct mylock *base = lock_of(pkt->dp_Arg1), *l;
	char name[MAXPATH], path[MAXPATH];
	struct dent info;
	int isdir;

	bstr_to_c(pkt->dp_Arg2, name, sizeof name);
	if (!path_join(base ? base->path : "", name, path)) {
		*err = ERROR_OBJECT_NOT_FOUND;
		return 0;
	}
	if ((*err = lookup(path, &isdir, &info)) != 0)
		return 0;
	l = new_lock(path, isdir, &info);
	if (!l) {
		*err = ERROR_NO_FREE_STORE;
		return 0;
	}
	dlog("lock %s -> %s", path, isdir ? "dir" : "file");
	return MKBADDR(l);
}

static LONG do_copydir(struct mylock *src, LONG *err)
{
	struct mylock *l = new_lock(src ? src->path : "", src ? src->isdir : 1,
	                            src ? &src->info : NULL);
	if (!l) {
		*err = ERROR_NO_FREE_STORE;
		return 0;
	}
	return MKBADDR(l);
}

static LONG do_parent(struct mylock *src, LONG *err)
{
	char path[MAXPATH], *s;
	struct mylock *l;

	if (!src || !src->path[0])
		return 0;                                /* root has no parent */
	strcpy(path, src->path);
	if ((s = strrchr(path, '/')) != NULL)
		*s = 0;
	else
		path[0] = 0;
	l = new_lock(path, 1, NULL);
	if (!l) {
		*err = ERROR_NO_FREE_STORE;
		return 0;
	}
	return MKBADDR(l);
}

static LONG do_examine(struct mylock *l, struct FileInfoBlock *fib)
{
	struct dent e;

	if (l)
		e = l->info;
	else {
		memset(&e, 0, sizeof e);
		strcpy(e.name, VOLNAME);
		e.isdir = 1;
	}
	fill_fib(fib, &e, 0);
	if (l && l->list) {
		l->list->users--;
		l->list = NULL;
	}
	return DOSTRUE;
}

static LONG do_exnext(struct mylock *l, struct FileInfoBlock *fib, LONG *err)
{
	LONG i = fib->fib_DiskKey;
	struct dirlist *dl;

	if (!l || !l->isdir) {
		*err = ERROR_OBJECT_WRONG_TYPE;
		return DOSFALSE;
	}
	if (!l->list || i == 0) {
		if (l->list)
			l->list->users--;
		l->list = get_list(l->path);
		if (!l->list) {
			*err = ERROR_NO_FREE_STORE;
			return DOSFALSE;
		}
		l->list->users++;
	}
	dl = l->list;
	if (dl->error) {
		*err = dl->error;
		return DOSFALSE;
	}
	if (i >= dl->n) {
		dl->users--;
		l->list = NULL;
		*err = ERROR_NO_MORE_ENTRIES;
		return DOSFALSE;
	}
	fill_fib(fib, &dl->e[i], i + 1);
	return DOSTRUE;
}

static LONG do_findinput(struct DosPacket *pkt, LONG *err)
{
	struct FileHandle *fh = BADDR(pkt->dp_Arg1);
	struct mylock *base = lock_of(pkt->dp_Arg2);
	char name[MAXPATH], path[MAXPATH], node[24];
	struct myfile *f;
	int isdir, k;

	bstr_to_c(pkt->dp_Arg3, name, sizeof name);
	if (!path_join(base ? base->path : "", name, path)) {
		*err = ERROR_OBJECT_NOT_FOUND;
		return DOSFALSE;
	}
	k = icon_of(path);
	if (k < 0 && !strchr(path, '/')) {
		*err = ERROR_OBJECT_WRONG_TYPE;
		return DOSFALSE;
	}
	f = AllocVec(sizeof *f, MEMF_ANY | MEMF_CLEAR);
	if (!f) {
		*err = ERROR_NO_FREE_STORE;
		return DOSFALSE;
	}
	strcpy(f->path, path);
	if (k >= 0) {
		f->mem = icons[k].data;
		f->memsize = icons[k].size;
		strncpy(f->info.name, path, sizeof f->info.name - 1);
		f->info.size = f->memsize;
		dlog("open %s (icon)", path);
		fh->fh_Arg1 = (LONG)f;
		return DOSTRUE;
	}
	path_node(path, node);
	if (!nodes(node, &f->cfg, NULL, NULL)) {
		FreeVec(f);
		*err = ERROR_OBJECT_NOT_FOUND;
		return DOSFALSE;
	}
	if ((*err = lookup(path, &isdir, &f->info)) != 0 || isdir) {
		FreeVec(f);
		if (!*err)
			*err = ERROR_OBJECT_WRONG_TYPE;
		return DOSFALSE;
	}
	if (f->info.icon > 0) {
		f->mem = icons[f->info.icon - 1].data;
		f->memsize = icons[f->info.icon - 1].size;
		dlog("open %s (icon)", path);
		fh->fh_Arg1 = (LONG)f;
		return DOSTRUE;
	}
	if ((*err = file_start(f)) != 0) {
		FreeVec(f);
		return DOSFALSE;
	}
	dlog("open %s (%s, size %ld)", path, f->f.text ? "text" : "binary", f->f.size);
	fh->fh_Arg1 = (LONG)f;
	return DOSTRUE;
}

static LONG do_end(struct myfile *f, LONG *err)
{
	if (!f)
		return DOSTRUE;
	if (f->writing && !f->sink)
		*err = file_finish(f);
	file_stop(f);
	if (f->buf)
		FreeVec(f->buf);
	if (f->wbuf)
		FreeVec(f->wbuf);
	if (f->memown)
		FreeVec(f->memown);
	FreeVec(f);
	return *err ? DOSFALSE : DOSTRUE;
}

/* MODE_NEWFILE. The file is created at the first Write() (text or binary
   is decided there) or at Close(). */
/* Did a command (Shell, Run, Execute Command, a script) send pkt? Programs
   started from Workbench have no CLI. Workbench itself runs as the
   background CLI LoadWB made, so it is told apart by its command name. */
static BOOL from_shell(struct DosPacket *pkt)
{
	struct Task *t = pkt->dp_Port ? pkt->dp_Port->mp_SigTask : NULL;
	struct CommandLineInterface *cli;
	const char *name;
	char cmd[64];

	if (!t || t->tc_Node.ln_Type != NT_PROCESS || !((struct Process *)t)->pr_CLI)
		return FALSE;
	cli = BADDR(((struct Process *)t)->pr_CLI);
	cmd[0] = 0;
	if (cli->cli_CommandName)
		bstr_to_c(cli->cli_CommandName, cmd, sizeof cmd);
	name = t->tc_Node.ln_Name ? t->tc_Node.ln_Name : "";
	dlog("sender '%s', command '%s'", name, cmd);
	if (strchr(cmd, ':') && !strchr(cmd, '/'))
		strcpy(cmd, strchr(cmd, ':') + 1);
	return !ieq(name, "Workbench") && !ieq(path_last(cmd), "Workbench");
}

static LONG do_findoutput(struct DosPacket *pkt, struct FileHandle *fh, struct mylock *base,
                          BSTR bname, LONG *err)
{
	char name[MAXPATH], path[MAXPATH], node[24];
	struct myfile *f;
	struct dent info;
	int isdir;

	path[0] = 0;
	bstr_to_c(bname, name, sizeof name);
	/* No .info files on the nodes unless the user copies one there in a
	   Shell: icons Workbench writes (snapshot, Information, the icon of a
	   dragged file) are accepted and dropped; DN: shows its own icons. */
	if (path_join(base ? base->path : "", name, path) && ends(path, ".info") &&
	    !from_shell(pkt)) {
		f = AllocVec(sizeof *f, MEMF_ANY | MEMF_CLEAR);
		if (!f) {
			*err = ERROR_NO_FREE_STORE;
			return DOSFALSE;
		}
		strcpy(f->path, path);
		f->writing = f->sink = 1;
		strncpy(f->info.name, path_last(path), sizeof f->info.name - 1);
		DateStamp(&f->info.date);
		dlog("open %s for writing: icon from Workbench, dropped", path);
		fh->fh_Arg1 = (LONG)f;
		return DOSTRUE;
	}
	if (!path_join(base ? base->path : "", name, path) || !strchr(path, '/')) {
		/* the root holds only nodes and our icons (e.g. a snapshot). Mount
		   of OS 2.04 opens "DN:" itself for writing to start the handler:
		   a directory, and an error without a "write protected" requester */
		dlog("write to the root refused: '%s'", name);
		*err = path[0] || !strchr(name, ':') ? ERROR_DISK_WRITE_PROTECTED
		                                    : ERROR_OBJECT_WRONG_TYPE;
		return DOSFALSE;
	}
	if (lookup(path, &isdir, &info) == 0 && isdir) {
		*err = ERROR_OBJECT_WRONG_TYPE;
		return DOSFALSE;
	}
	f = AllocVec(sizeof *f, MEMF_ANY | MEMF_CLEAR);
	if (!f || !(f->wbuf = AllocVec(DAP_MAXREC, MEMF_ANY))) {
		if (f)
			FreeVec(f);
		*err = ERROR_NO_FREE_STORE;
		return DOSFALSE;
	}
	strcpy(f->path, path);
	f->writing = 1;
	strncpy(f->info.name, path_last(path), sizeof f->info.name - 1);
	DateStamp(&f->info.date);
	path_node(path, node);
	if (!nodes(node, &f->cfg, NULL, NULL))
		*err = ERROR_OBJECT_NOT_FOUND;
	else
		*err = conn_open(&f->c, &f->cfg);   /* login errors show up now */
	if (*err) {
		FreeVec(f->wbuf);
		FreeVec(f);
		return DOSFALSE;
	}
	dlog("open %s for writing", path);
	fh->fh_Arg1 = (LONG)f;
	return DOSTRUE;
}

/* VMS spec of the directory file for a directory path ("A/B" -> "[.A]B.DIR;1"). */
static BOOL dir_file_spec(const char *path, char *out)
{
	const char *rest = path_rest(path), *last = path_last(rest);

	if (!*rest || last[0] == '[')
		return FALSE;                   /* node or absolute VMS directory */
	vms_dir(rest, last - rest - (last > rest ? 1 : 0), out);
	vms_name(last, strlen(last), 0, out + strlen(out));
	strcat(out, ".DIR;1");
	return TRUE;
}

/* Runs one ERASE or RENAME on the node of path. */
static LONG remote_op(const char *path, const char *spec, const char *newspec)
{
	struct nodecfg cfg;
	struct myfile tmp;              /* for conn and dap_error() */
	char node[24];
	LONG err;
	int r;

	path_node(path, node);
	if (!nodes(node, &cfg, NULL, NULL))
		return ERROR_OBJECT_NOT_FOUND;
	memset(&tmp, 0, sizeof tmp);
	if ((err = conn_open(&tmp.c, &cfg)) != 0)
		return err;
	r = newspec ? dap_rename(tmp.c.d, spec, newspec) : dap_erase(tmp.c.d, spec);
	if (r) {
		dlog("%s %s: %s", newspec ? "rename" : "erase", spec, tmp.c.d->error);
		err = dap_error(&tmp, ERROR_DELETE_PROTECTED);
		if (tmp.c.d->status == 0x4032 || tmp.c.d->status == 0x4020)
			err = ERROR_OBJECT_NOT_FOUND;
	}
	conn_close(&tmp.c);
	return err;
}

static LONG do_delete(struct mylock *base, BSTR bname, LONG *err)
{
	char name[MAXPATH], path[MAXPATH], spec[SPECLEN];
	struct dent info;
	struct dirlist *l;
	int isdir;

	bstr_to_c(bname, name, sizeof name);
	if (!path_join(base ? base->path : "", name, path) || !strchr(path, '/')) {
		*err = ERROR_OBJECT_WRONG_TYPE;         /* the root or a node */
		return DOSFALSE;
	}
	if ((*err = lookup(path, &isdir, &info)) != 0)
		return DOSFALSE;
	if (info.icon > 0)                      /* a virtual icon: nothing stored */
		return DOSTRUE;
	if (isdir) {
		l = get_list(path);
		if (l && !l->error && l->n > 0) {
			*err = ERROR_DIRECTORY_NOT_EMPTY;
			return DOSFALSE;
		}
		if (!dir_file_spec(path, spec)) {
			*err = ERROR_OBJECT_WRONG_TYPE;
			return DOSFALSE;
		}
	} else {
		remote_spec(path, 0, spec);
		strcat(spec, ";*");                     /* all versions */
	}
	*err = remote_op(path, spec, NULL);
	list_changed(path);
	dlog("delete %s (%s) -> %ld", path, spec, *err);
	return *err ? DOSFALSE : DOSTRUE;
}

static LONG do_rename(struct DosPacket *pkt, LONG *err)
{
	struct mylock *b1 = lock_of(pkt->dp_Arg1), *b2 = lock_of(pkt->dp_Arg3);
	char n1[MAXPATH], n2[MAXPATH], from[MAXPATH], to[MAXPATH], node1[24], node2[24];
	char spec1[SPECLEN], spec2[SPECLEN];
	struct dent info, info2;
	int isdir, isdir2;

	bstr_to_c(pkt->dp_Arg2, n1, sizeof n1);
	bstr_to_c(pkt->dp_Arg4, n2, sizeof n2);
	if (!path_join(b1 ? b1->path : "", n1, from) || !path_join(b2 ? b2->path : "", n2, to) ||
	    !strchr(from, '/') || !strchr(to, '/')) {
		*err = ERROR_OBJECT_WRONG_TYPE;
		return DOSFALSE;
	}
	path_node(from, node1);
	path_node(to, node2);
	if (!ieq(node1, node2)) {
		*err = ERROR_RENAME_ACROSS_DEVICES;
		return DOSFALSE;
	}
	if ((*err = lookup(from, &isdir, &info)) != 0)
		return DOSFALSE;
	if (info.icon > 0)                      /* a virtual icon follows its file */
		return DOSTRUE;
	if (ieq(from, to))                      /* VMS names have no case */
		return DOSTRUE;
	/* a virtual icon at the target gives way to a real one */
	if (lookup(to, &isdir2, &info2) == 0 && info2.icon <= 0) {
		*err = ERROR_OBJECT_EXISTS;
		return DOSFALSE;
	}
	if (isdir) {
		if (!dir_file_spec(from, spec1) || !dir_file_spec(to, spec2)) {
			*err = ERROR_OBJECT_WRONG_TYPE;
			return DOSFALSE;
		}
	} else {
		remote_spec(from, 0, spec1);
		remote_spec(to, 0, spec2);
	}
	*err = remote_op(from, spec1, spec2);
	list_changed(from);
	list_changed(to);
	dlog("rename %s -> %s (%s -> %s) -> %ld", from, to, spec1, spec2, *err);
	return *err ? DOSFALSE : DOSTRUE;
}

static LONG do_seek(struct myfile *f, LONG off, LONG mode, LONG *err)
{
	LONG target;

	if (f->writing) {                       /* only asking for the position */
		if ((mode == OFFSET_CURRENT || mode == OFFSET_END) && off == 0)
			return f->pos;
		if (mode == OFFSET_BEGINNING && off == f->pos)
			return f->pos;
		*err = ERROR_SEEK_ERROR;
		return -1;
	}

	switch (mode) {
	case OFFSET_BEGINNING: target = off; break;
	case OFFSET_CURRENT: target = f->pos + off; break;
	case OFFSET_END: {
		LONG size = file_size(f, err);
		if (size < 0)
			return -1;
		target = size + off;
		break;
	}
	default:
		*err = ERROR_SEEK_ERROR;
		return -1;
	}
	if (target < 0) {
		*err = ERROR_SEEK_ERROR;
		return -1;
	}
	return file_seek(f, target, err);
}

static void fill_info(struct InfoData *id)
{
	memset(id, 0, sizeof *id);
	/* DAP cannot ask a node for its free space, and Workbench shows "x %
	   full" in every volume window and refuses to copy without an answer:
	   claim an empty 512 MB disk, so nothing refuses to write for lack of
	   room */
	id->id_DiskState = ID_VALIDATED;
	id->id_NumBlocks = 0x100000;
	id->id_NumBlocksUsed = 0;
	id->id_BytesPerBlock = 512;
	id->id_DiskType = ID_DOS_DISK;
	id->id_VolumeNode = MKBADDR(volume);
	id->id_InUse = DOSTRUE;
}

static void add_volume(void)
{
	struct DosList *dl = MakeDosEntry((STRPTR)VOLNAME, DLT_VOLUME);

	if (!dl)
		return;
	dl->dol_Task = port;
	dl->dol_misc.dol_volume.dol_DiskType = ID_DOS_DISK;
	DateStamp(&dl->dol_misc.dol_volume.dol_VolumeDate);
	LockDosList(LDF_VOLUMES | LDF_WRITE);
	if (AddDosEntry(dl))
		volume = (struct DeviceList *)dl;
	else
		FreeDosEntry(dl);
	UnLockDosList(LDF_VOLUMES | LDF_WRITE);
}

/* called by start() in start.c */
void handler_main(void)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	struct DosPacket *pkt;
	struct Message *msg;
	struct DeviceNode *dev;

	WaitPort(&me->pr_MsgPort);
	msg = GetMsg(&me->pr_MsgPort);
	pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
	port = CreateMsgPort();
	DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);
	UtilityBase = OpenLibrary((STRPTR)"utility.library", 37);
	if (!DOSBase || !UtilityBase || !port) {
		pkt->dp_Res1 = DOSFALSE;
		pkt->dp_Res2 = ERROR_INVALID_RESIDENT_LIBRARY;
		ReplyMsg(msg);
		return;
	}
	/* Only exec calls before the reply: Mount may hold the DOS list while
	   it waits for us, so DOS file access here can deadlock. */
	dev = BADDR(pkt->dp_Arg3);
	dev->dn_Task = port;
	ReplyPkt(pkt, DOSTRUE, 0);
	check_debug();
	add_volume();
	dlog("startup: device node %lx, volume %lx", (ULONG)dev, (ULONG)volume);

	for (;;) {
		LONG res1 = DOSFALSE, res2 = 0;

		WaitPort(port);
		msg = GetMsg(port);
		pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
		check_debug();
		dlog("packet %ld arg1 %lx arg2 %lx arg3 %lx", pkt->dp_Type, pkt->dp_Arg1,
		     pkt->dp_Arg2, pkt->dp_Arg3);
		switch (pkt->dp_Type) {
		case ACTION_LOCATE_OBJECT:
			res1 = do_locate(pkt, &res2);
			break;
		case ACTION_FREE_LOCK:
			free_lock(lock_of(pkt->dp_Arg1));
			res1 = DOSTRUE;
			break;
		case ACTION_COPY_DIR:
			res1 = do_copydir(lock_of(pkt->dp_Arg1), &res2);
			break;
		case ACTION_PARENT:
			res1 = do_parent(lock_of(pkt->dp_Arg1), &res2);
			break;
		case ACTION_SAME_LOCK: {
			struct mylock *a = lock_of(pkt->dp_Arg1), *b = lock_of(pkt->dp_Arg2);
			res1 = (a && b && ieq(a->path, b->path)) ? DOSTRUE : DOSFALSE;
			break;
		}
		case ACTION_EXAMINE_OBJECT:
			res1 = do_examine(lock_of(pkt->dp_Arg1), BADDR(pkt->dp_Arg2));
			break;
		case ACTION_EXAMINE_NEXT:
			res1 = do_exnext(lock_of(pkt->dp_Arg1), BADDR(pkt->dp_Arg2), &res2);
			break;
		case ACTION_FINDINPUT:
			res1 = do_findinput(pkt, &res2);
			break;
		case ACTION_FINDOUTPUT:
			res1 = do_findoutput(pkt, BADDR(pkt->dp_Arg1), lock_of(pkt->dp_Arg2), pkt->dp_Arg3, &res2);
			break;
		case ACTION_FINDUPDATE: {
			/* existing files can only be read; a new one is written */
			char name[MAXPATH], path[MAXPATH];
			struct mylock *base = lock_of(pkt->dp_Arg2);
			struct dent info;
			int isdir;

			bstr_to_c(pkt->dp_Arg3, name, sizeof name);
			if (path_join(base ? base->path : "", name, path) &&
			    !(ends(path, ".info") && !from_shell(pkt)) && lookup(path, &isdir, &info) == 0)
				res1 = do_findinput(pkt, &res2);
			else
				res1 = do_findoutput(pkt, BADDR(pkt->dp_Arg1), base, pkt->dp_Arg3, &res2);
			break;
		}
		case ACTION_DELETE_OBJECT:
			res1 = do_delete(lock_of(pkt->dp_Arg1), pkt->dp_Arg2, &res2);
			break;
		case ACTION_RENAME_OBJECT:
			res1 = do_rename(pkt, &res2);
			break;
		case ACTION_CREATE_DIR:
			res2 = ERROR_ACTION_NOT_KNOWN;  /* DAP cannot create directories */
			break;
		case ACTION_SET_PROTECT:
		case ACTION_SET_COMMENT:
		case ACTION_SET_DATE:
			res1 = DOSTRUE;                 /* accepted, nothing to store them in */
			break;
		case ACTION_READ: {
			struct myfile *f = (struct myfile *)pkt->dp_Arg1;
			if (f->writing) {
				res1 = -1;
				res2 = ERROR_READ_PROTECTED;
			} else {
				res1 = file_read(f, (UBYTE *)pkt->dp_Arg2, pkt->dp_Arg3);
			}
			break;
		}
		case ACTION_WRITE: {
			struct myfile *f = (struct myfile *)pkt->dp_Arg1;
			if (!f->writing) {
				res1 = -1;
				res2 = ERROR_WRITE_PROTECTED;
			} else {
				res1 = file_write(f, (UBYTE *)pkt->dp_Arg2, pkt->dp_Arg3, &res2);
			}
			break;
		}
		case ACTION_END:
			res1 = do_end((struct myfile *)pkt->dp_Arg1, &res2);
			break;
		case ACTION_PARENT_FH:
		case ACTION_COPY_DIR_FH: {
			struct myfile *f = (struct myfile *)pkt->dp_Arg1;
			struct mylock tmp;

			memset(&tmp, 0, sizeof tmp);
			strcpy(tmp.path, f->path);
			tmp.info = f->info;
			res1 = pkt->dp_Type == ACTION_PARENT_FH ? do_parent(&tmp, &res2)
			                                        : do_copydir(&tmp, &res2);
			break;
		}
		case ACTION_SEEK:
			res1 = do_seek((struct myfile *)pkt->dp_Arg1, pkt->dp_Arg2, pkt->dp_Arg3, &res2);
			break;
		case ACTION_EXAMINE_FH: {
			/* the exact size: programs read that many bytes (the text
			   datatype of OS 3.2 fails on less) */
			struct myfile *f = (struct myfile *)pkt->dp_Arg1;
			struct FileInfoBlock *fib = BADDR(pkt->dp_Arg2);
			LONG e = 0, size;

			fill_fib(fib, &f->info, 0);
			if (!f->writing && (size = file_size(f, &e)) >= 0) {
				fib->fib_Size = size;
				fib->fib_NumBlocks = (size + 511) / 512;
			}
			res1 = DOSTRUE;
			break;
		}
		case ACTION_IS_FILESYSTEM:
			res1 = DOSTRUE;
			break;
		case ACTION_DISK_INFO:
			fill_info(BADDR(pkt->dp_Arg1));
			res1 = DOSTRUE;
			break;
		case ACTION_INFO:
			fill_info(BADDR(pkt->dp_Arg2));
			res1 = DOSTRUE;
			break;
		case ACTION_CURRENT_VOLUME:
			res1 = MKBADDR(volume);
			break;
		default:
			res2 = ERROR_ACTION_NOT_KNOWN;
		}
		if (debug && pkt->dp_Type != ACTION_READ)
			dlog("packet %ld -> %ld/%ld", pkt->dp_Type, res1, res2);
		ReplyPkt(pkt, res1, res2);
	}
	(void)version;
}

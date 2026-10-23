/*
 * dap.h - portable DAP (Data Access Protocol) client for FAL (object 17).
 *
 * Runs over any logical link given as a read/write function pair. Derived
 * from DAPGet (Mac FotoFix project); message layouts follow the DAP 5.6
 * specification and dnprogs libdap.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DAP_H
#define DAP_H

#define DAP_OBJECT 17

struct dap {
	/* one complete NSP message: >0 length, 0 timeout, <0 link gone */
	long (*read)(void *ctx, unsigned char *buf, long max, long timeout_ms);
	int  (*write)(void *ctx, const unsigned char *buf, long n);   /* 0 = ok */
	void (*log)(void *ctx, const char *msg);                      /* optional */
	void *ctx;
	int verbose;

	/* filled by dap_config */
	int ostype, filesys, version, eco;
	unsigned bufsiz;

	char error[200];        /* text of the last failure */
	int status;             /* last DAP STATUS code, 0 if none */

	unsigned char rx[8192];
	long rxlen, rxpos;
};

struct dap_dirent {
	char volume[64];
	char directory[128];
	char name[128];
	long bytes;             /* -1 if unknown */
	long blocks;            /* allocated blocks, -1 if unknown */
	char created[24];       /* "21-OCT-2026 20:51:02" or empty */
	char owner[40];
};

/* blocking: 1 = offer SYSCAP bit 18 (fast with VMS, FOTOFX needs 0) */
int dap_config(struct dap *d, int blocking);

/* List files matching spec; cb returns 0 to continue. 0 = ok, -1 = error */
int dap_directory(struct dap *d, const char *spec,
                  int (*cb)(void *ud, const struct dap_dirent *e), void *ud);

/* Fetch a file sequentially. cb gets each record (text = 1 if it is a text
   record without its line terminator, 0 for a raw byte block); returns 0 to
   continue. *size gets the size the server announced (0 if unknown). */
int dap_get(struct dap *d, const char *spec,
            int (*cb)(void *ud, const unsigned char *p, long n, int text), void *ud,
            long *size);

/* Step by step reading, for callers that pull data (DOS handler):
   dap_open_read() opens and starts the transfer, dap_read_record()
   returns 1 with the next record, 0 at end of file, -1 on error;
   dap_close_file() ends the access (needed after EOF or to abort). */
struct dap_file {
	int text;               /* records are text lines without terminator */
	long size;              /* announced size in bytes, -1 if unknown */
	int rfm, rat;
};
int dap_open_read(struct dap *d, const char *spec, struct dap_file *f);
int dap_read_record(struct dap *d, const unsigned char **p, long *n);
int dap_close_file(struct dap *d);

/* Writing: dap_open_write() creates spec (a new version on VMS) as a
   variable length record file, text = records are lines (implied
   carriage control) or 0 = binary records; name gets the name the server
   chose (may be NULL). dap_write_record() sends one record of at most
   DAP_MAXREC bytes; dap_close_write() finishes the file and reports
   errors the server found while writing. */
#define DAP_MAXREC 1024
int dap_open_write(struct dap *d, const char *spec, int text, char *name, int namesize);
int dap_write_record(struct dap *d, const unsigned char *p, long n);
int dap_close_write(struct dap *d);

/* Delete files (spec may name all versions, ";*"), rename one file. */
int dap_erase(struct dap *d, const char *spec);
int dap_rename(struct dap *d, const char *from, const char *to);

const char *dap_reason_text(int nsp_reason);

#endif

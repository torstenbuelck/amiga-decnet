/*
 * nml.h - portable NML (Network Management Listener, object 19) client:
 * reads the node database of another node with NICE "read information",
 * like NCP SHOW / LIST KNOWN NODES. Same link abstraction as dap.h.
 *
 * Message layouts follow the DECnet Network Management Functional
 * Specification 4.0 and dnprogs nml.
 *
 * Copyright (C) 2026  Torsten
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NML_H
#define NML_H

#define NML_OBJECT 19

/* NICE version 4.0.0, sent as connect data (RSX and VMS look at it) */
extern const unsigned char nml_version[3];

struct nml {
	/* one complete NSP message: >0 length, 0 timeout, <0 link gone */
	long (*read)(void *ctx, unsigned char *buf, long max, long timeout_ms);
	int  (*write)(void *ctx, const unsigned char *buf, long n);   /* 0 = ok */
	void (*log)(void *ctx, const char *msg);                      /* optional */
	void *ctx;
	int verbose;

	char error[200];        /* text of the last failure */
	int code;               /* last NICE return code, 0 if none */

	unsigned char rx[1024];
};

/* node states (parameter 0) */
#define NML_STATE_UNKNOWN     (-1)  /* not reported */
#define NML_STATE_ON          0
#define NML_STATE_OFF         1
#define NML_STATE_SHUT        2
#define NML_STATE_RESTRICTED  3
#define NML_STATE_REACHABLE   4
#define NML_STATE_UNREACHABLE 5

/* Calls cb for every node the server knows: address, name ("" if it has
   none), whether it is the server itself and its state as the server sees
   it (NML_STATE_..., the server itself is usually ON). permanent = 1 reads
   the permanent database (LIST), 0 the volatile one (SHOW). cb returns 0
   to continue. Returns the number of nodes, -1 on error. */
int nml_known_nodes(struct nml *n, int permanent,
                    int (*cb)(void *ud, unsigned addr, const char *name, int executor,
                              int state),
                    void *ud);

#endif

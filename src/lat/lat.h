/*
 * lat.h - portable LAT terminal server side (outgoing sessions).
 *
 * The Amiga plays the part of a terminal server: it learns the services
 * from the multicast announcements, opens a virtual circuit to the node
 * offering one and runs terminal sessions in slots on it. It can also
 * be the host: offer a service of its own (lat_offer), announce it and
 * take sessions that terminal servers start (incoming, lat_answer). Like the
 * DECnet core it talks to the outside only through callbacks (frames
 * out, session data and events) and keeps all state in static tables,
 * so the Linux test (dntool lat) and the Amiga latd share it.
 *
 * Message layouts follow latd (Christine Caulfield, GPL-2.0-or-later)
 * and a session of a latd-based terminal server with OpenVMS 7.2,
 * captured 23.10.2026.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LAT_H
#define LAT_H

#define LAT_ETHERTYPE   0x6004
#define LAT_NAMELEN     16      /* longest node, service and port name */
#define LAT_MAXSVC      40      /* services remembered */
#define LAT_MAXCIRC     4       /* virtual circuits (remote nodes) */
#define LAT_MAXSESS     4       /* sessions per circuit */
#define LAT_TXBUF       512     /* keys waiting for credit, per session */

/* session events */
#define LAT_EV_UP       1       /* the service accepted the session */
#define LAT_EV_DOWN     2       /* the session is over, reason given */

#define LAT_ANNOUNCE_S  20      /* our service announcements, seconds */

/* reasons for LAT_EV_DOWN (beyond the slot reasons 0-15 of the
   protocol, see lat_reason_text) */
#define LAT_R_USER      1       /* we disconnected */
#define LAT_R_NOSVC     7       /* no such service */
#define LAT_R_NOANSWER  32      /* the node did not answer the start */
#define LAT_R_LOST      33      /* retransmission limit, circuit gone */
#define LAT_R_CIRCUIT   34      /* the node stopped the circuit */

struct lat_env {
	/* one frame without the Ethernet header (the environment adds
	   it with ethertype 0x6004 and pads it) */
	void (*send_frame)(void *ctx, const unsigned char *dst,
	                   const unsigned char *payload, unsigned len);
	/* output of a session for the terminal */
	void (*data)(void *ctx, int sid, const unsigned char *buf, unsigned len);
	/* LAT_EV_UP / LAT_EV_DOWN with a reason */
	void (*event)(void *ctx, int sid, int ev, int reason);
	/* host role: a terminal server starts a session to our service;
	   answer it with lat_answer (until then nothing is sent to it).
	   node: the terminal server, port: its port name. Optional */
	void (*incoming)(void *ctx, int sid, const char *node, const char *port);
	void (*log)(void *ctx, const char *msg);        /* optional */
	void *ctx;
	int verbose;
	unsigned char mac[6];           /* our address */
	char node[LAT_NAMELEN + 1];     /* our node name (Start message) */
	char ident[40];                 /* our description */
};

struct lat_service {
	char name[LAT_NAMELEN + 1];
	char node[LAT_NAMELEN + 1];
	char ident[64];
	unsigned char mac[6];
	int rating;
	unsigned long age_ms;           /* since the last announcement */
};

void lat_init(const struct lat_env *env, unsigned seed);
/* a frame with ethertype 0x6004 (src, dst: Ethernet addresses) */
void lat_input(const unsigned char *src, const unsigned char *dst,
               const unsigned char *msg, unsigned len);
/* call every few ms; ms = time since the last call */
void lat_tick(unsigned ms);

/* known services: i = 0, 1, ... until it returns NULL */
const struct lat_service *lat_service(int i);
const struct lat_service *lat_find_service(const char *name);

/* open a session to a service (port: our port name, shown on VMS as
   "LAT Server/Port: NODE/port"); >= 0: session id, the outcome comes
   as LAT_EV_UP or LAT_EV_DOWN; < 0: -reason (LAT_R_NOSVC, or 5:
   no free circuit or session) */
int  lat_connect(const char *service, const char *port);
/* keys for the session; returns how many were taken (the rest waits
   for room) */
int  lat_write(int sid, const unsigned char *buf, unsigned len);
/* room for lat_write */
unsigned lat_room(int sid);
/* send a BREAK to the host */
int  lat_break(int sid);
/* end a session; LAT_EV_DOWN follows */
void lat_disconnect(int sid);
/* stop every circuit at once (LAT_EV_DOWN for what is left) */
void lat_shutdown(void);

/* host role: offer the service name (announced every LAT_ANNOUNCE_S
   seconds); NULL stops offering, sessions already running go on */
void lat_offer(const char *name, const char *ident);
/* accept (reason 0) or reject (a slot reason: 5 resources, 14 access
   denied, ...) an incoming session; a rejected one is gone at once */
int  lat_answer(int sid, int reason);

const char *lat_reason_text(int reason);

#endif

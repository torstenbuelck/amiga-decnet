/*
 * dn.h - portable DECnet Phase IV endnode core (routing, NSP, session).
 *
 * The core has no OS dependencies. The environment supplies frame output,
 * memory and logging through struct dn_env, feeds received frames into
 * dn_input() and calls dn_tick() regularly (every 100 ms or so).
 * Single threaded: all calls must come from one task.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DN_H
#define DN_H

typedef unsigned char  dn_u8;
typedef unsigned short dn_u16;
typedef unsigned long  dn_u32;

#define DN_ETHERTYPE        0x6003
#define DN_MAXLINKS         16
#define DN_SEGSIZE          1459    /* our max NSP data per segment */
#define DN_BLKSIZE          1498    /* Ethernet routing layer block size */

/* NSP disconnect / reject reasons (Session Control) */
#define DN_R_NORMAL         0
#define DN_R_RESOURCES      1
#define DN_R_NOOBJECT       4
#define DN_R_ACCESS         34
#define DN_R_UNREACHABLE    38
#define DN_R_NOLINK         41
#define DN_R_DISCCOMPLETE   42

#define DN_LOG_ERR   0
#define DN_LOG_INFO  1
#define DN_LOG_DEBUG 2

struct dn_env {
	/* Send one Ethernet frame of type 0x6003. payload starts with the
	   routing layer length field. */
	void (*send_frame)(void *ctx, const dn_u8 *dstmac, const dn_u8 *payload,
	                   unsigned len);
	void *(*alloc)(void *ctx, unsigned size);
	void (*free)(void *ctx, void *p);
	void (*log)(void *ctx, int level, const char *msg);
	void *ctx;
	int loglevel;
};

/* Received connect request, as passed to an object's connect handler. */
struct dn_conninfo {
	dn_u16 node;            /* remote node address */
	dn_u8  dstobj;          /* object number we were called as (0 = by name) */
	char   dstname[17];     /* object name if called by name */
	dn_u8  srcobj;
	char   srcname[17];     /* remote task / user name descriptor */
	char   user[40];        /* access control, may be empty */
	char   password[40];
	char   account[40];
	dn_u8  data[16];        /* optional user data */
	dn_u8  datalen;
};

struct dn_link;

struct dn_object {
	dn_u8 number;           /* 0 = match by name */
	const char *name;
	/* Call dn_accept()/dn_reject() now or later; while the answer is
	   outstanding the remote gets a connect ack. */
	void (*connect)(struct dn_link *l, const struct dn_conninfo *ci);
	/* One complete NSP message. */
	void (*data)(struct dn_link *l, const dn_u8 *buf, unsigned len);
	void (*disconnect)(struct dn_link *l, dn_u16 reason);
	void *user;
	/* outgoing links: connect confirmed, with the remote's connect data */
	void (*connected)(struct dn_link *l, const dn_u8 *data, unsigned len);
};

/* Outgoing connect request. Object by number, or by name if objnum is 0.
   Strings may be NULL or empty. */
struct dn_connreq {
	dn_u16 node;
	dn_u8 objnum;
	const char *objname;
	const char *srcname;    /* our end user name, default "DECNET" */
	const char *user;
	const char *password;
	const char *account;
	const dn_u8 *data;
	unsigned datalen;
};

/* setup / runtime */
int  dn_init(const struct dn_env *env, dn_u16 address);
/* where local link addresses start (default 0x2102); a different value
   at every start keeps a restarted node from reusing the address of a
   link the other end still believes open, which makes it drop the new
   connect as a duplicate */
void dn_seed_links(dn_u16 seed);
void dn_input(const dn_u8 *srcmac, const dn_u8 *dstmac, const dn_u8 *payload,
              unsigned len);
void dn_tick(unsigned ms);
void dn_shutdown(void);
int  dn_register_object(struct dn_object *obj);
void dn_unregister_object(struct dn_object *obj);
/* by number, or by name (any case) if number is 0 */
struct dn_object *dn_find_object(dn_u8 number, const char *name);

/* links */
struct dn_link *dn_connect(const struct dn_connreq *req, struct dn_object *handler);
int  dn_accept(struct dn_link *l, const dn_u8 *data, unsigned len);
int  dn_reject(struct dn_link *l, dn_u16 reason, const dn_u8 *data, unsigned len);
int  dn_send(struct dn_link *l, const dn_u8 *buf, unsigned len);
int  dn_disconnect(struct dn_link *l, dn_u16 reason, const dn_u8 *data, unsigned len);
void dn_set_user(struct dn_link *l, void *p);
void *dn_get_user(struct dn_link *l);
dn_u16 dn_link_node(struct dn_link *l);
unsigned dn_pending(struct dn_link *l);   /* unacknowledged segments */

/* helpers */
void dn_node_mac(dn_u8 *mac, dn_u16 addr);
const char *dn_ntoa(dn_u16 addr);   /* static buffer */
dn_u16 dn_designated_router(void);
extern const dn_u8 dn_all_routers[6];
extern const dn_u8 dn_all_endnodes[6];

/* built-in objects */
void dn_mirror_init(void);

#endif

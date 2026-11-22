/*
 * dnenv.h - run the portable DECnet core on Linux (AF_PACKET) and offer
 * simple blocking calls on top of it, the same model decnet.library will
 * give Amiga programs.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DNENV_H
#define DNENV_H

#include "../core/dn.h"

/* NULL ifname: br0 if it exists, else eth0 */
int  dnenv_open(const char *ifname, dn_u16 addr, int verbose);
void dnenv_pump(int timeout_ms);    /* one round of the event loop */
void dnenv_close(void);
int  dnenv_stopped(void);           /* SIGINT/SIGTERM seen */
int  dn_aton(const char *s, dn_u16 *out);   /* "23.208" -> address */
/* router to use while no router hello is heard; call before dnenv_open */
void dnenv_set_router(dn_u16 addr);
/* frames of another ethertype (LAT) to input; sending them; our MAC */
void dnenv_other(unsigned type, void (*input)(const unsigned char *src,
                 const unsigned char *dst, const unsigned char *payload, unsigned len));
void dnenv_send(unsigned type, const unsigned char *dst, const unsigned char *payload,
                unsigned len);
void dnenv_mac(unsigned char *mac);

/* blocking logical link */
struct dns;
struct dns *dns_connect(const struct dn_connreq *req, int timeout_ms, int *reason);
/* one complete NSP message: >0 length, 0 timeout, -1 link gone */
/* wait for a connect to object objnum (timeout <0: forever) and accept it;
   further connects are rejected while nobody waits */
struct dns *dns_accept(dn_u8 objnum, int timeout_ms, struct dn_conninfo *ci);
long dns_read(struct dns *s, unsigned char *buf, long max, int timeout_ms);
int  dns_write(struct dns *s, const unsigned char *buf, long n);
void dns_close(struct dns *s);
int  dns_reason(struct dns *s);     /* disconnect reason once gone */

#endif

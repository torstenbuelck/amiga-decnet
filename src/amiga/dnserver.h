/*
 * dnserver.h - dnetd's decnet.library server.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DNSERVER_H
#define DNSERVER_H

ULONG srv_init(void);           /* signal mask of the public port, 0 = failed */
void  srv_handle(void);
void  srv_tick(unsigned ms);
void  srv_poll(void);         /* after incoming frames: release waiting writes */
void  srv_shutdown(void);
/* a frame (dst src type payload) of the tap's ethertype */
void  srv_tap_input(const UBYTE *frame, ULONG len);

/* dnetd.c, for the tap */
int   dnetd_tap_enable(UWORD type, const UBYTE *mcast);
void  dnetd_tap_send(const UBYTE *dst, const UBYTE *payload, ULONG len);
const UBYTE *dnetd_address(void);

#endif

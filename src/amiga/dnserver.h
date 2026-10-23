/*
 * dnserver.h - dnetd's decnet.library server.
 *
 * Copyright (C) 2026  Torsten
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DNSERVER_H
#define DNSERVER_H

ULONG srv_init(void);           /* signal mask of the public port, 0 = failed */
void  srv_handle(void);
void  srv_tick(unsigned ms);
void  srv_poll(void);         /* after incoming frames: release waiting writes */
void  srv_shutdown(void);

#endif

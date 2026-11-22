/*
 * dngwamiga.h - dnetd's transport through the decnet-gateway tunnel
 * (UDP over the Amiga's TCP/IP stack, bsdsocket.library), instead of a
 * SANA-II device. Protocol code: src/dngw.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DNGWAMIGA_H
#define DNGWAMIGA_H

#include <exec/types.h>

typedef void (*gwt_log_fn)(const char *fmt, ...);

/* Opens bsdsocket.library (waits up to 30 s for the TCP/IP stack), reads
   the DNGW1 client line from linefile, keeps the counter epoch in
   linefile + "Epoch" (written before the first packet goes out) and sends
   the first keepalive. want = the address the node must have (area << 10
   | node, 0 = any): checked before anything is sent. Sets *area and *node
   from the line. 0 = ok. */
int  gwt_open(const char *linefile, UWORD want, UWORD *area, UWORD *node, gwt_log_fn log);
void gwt_close(void);

/* one Ethernet frame (dst, src, type, payload), at most 1514 bytes */
void gwt_send(const UBYTE *frame, ULONG len);

/* Waits for *sigs (like Wait) or a packet from the gateway; on return
   *sigs holds the signals that came. TRUE if packets may be waiting. */
BOOL gwt_wait(ULONG *sigs);

/* Next frame from the gateway: TRUE and *frame, *len (len 0 = keepalive);
   FALSE when none is waiting. The frame stays valid until the next call. */
BOOL gwt_recv(UBYTE **frame, ULONG *len);

/* every timer tick: keepalive after 15 s without sending, the gateway
   lost after 50 s without a packet, then its name resolved every 30 s */
void gwt_tick(void);

const char *gwt_host(void);

#endif

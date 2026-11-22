/*
 * dngw.h - client side of the DECnet gateway tunnel (decnet-gateway by
 * Hans Hübner, protocol version 1): Ethernet frames of the DEC protocols
 * carried over UDP, sealed with ChaCha20-Poly1305 (RFC 8439).
 *
 * Portable C (C89 plus <stdint.h>), no OS calls: the caller owns the UDP
 * socket, the clock and the epoch state file. Runs on a 68000.
 *
 * Packet: key id (4, big-endian) | version 1 | direction (0 = client to
 * gateway, 1 = gateway to client) | 0 0 | counter (8, big-endian) |
 * sealed frame | 16-byte tag. Nonce = header bytes 4..15, AAD = header.
 * An empty frame is a keepalive.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DNGW_H
#define DNGW_H

#include <stdint.h>

#define DNGW_PORT       26003
#define DNGW_HDR        16
#define DNGW_TAG        16
#define DNGW_MAXFRAME   1514
#define DNGW_MAXPKT     (DNGW_HDR + DNGW_MAXFRAME + DNGW_TAG)
#define DNGW_KEEPALIVE_S 15     /* send one after this long without sending */
#define DNGW_LOST_S      50     /* gateway lost after this long without a packet */

struct dngw {
	uint32_t keyid;
	uint8_t  key[32];
	uint64_t tx;            /* next counter to send */
	uint64_t start;         /* first counter of this run: accept nothing below */
	uint64_t rx_max;        /* highest counter accepted from the gateway */
	int      rx_any;        /* rx_max is valid */
	uint8_t  window[128];   /* bit i: counter rx_max - i seen (i < 1024) */
};

/* The client line: "DNGW1 <host> <port> <key-id> <area.node> <64 hex>".
   host gets at most hostlen-1 characters. 0 if the line is malformed. */
int dngw_parse_line(const char *line, char *host, unsigned hostlen,
                    unsigned *port, uint32_t *keyid, unsigned *area,
                    unsigned *node, uint8_t key[32]);

/* Starts a run. now = Unix time in seconds (0 if unknown); saved = the
   epoch kept from the last run, have_saved = 0 if there is none. Returns
   the epoch this run uses: the caller must store it BEFORE the first
   packet goes out, so that the next run starts above it. */
uint32_t dngw_start(struct dngw *g, uint32_t keyid, const uint8_t key[32],
                    uint32_t now, uint32_t saved, int have_saved);

/* The epoch of the next counter (changes only after 2^32 packets). */
uint32_t dngw_epoch(const struct dngw *g);

/* Seals frame (len 0 = keepalive, else 14..1514 bytes) into out, which
   needs len + 32 bytes. Returns the packet length. */
unsigned dngw_seal(struct dngw *g, const uint8_t *frame, unsigned len, uint8_t *out);

/* Checks and opens a packet from the gateway in place. Returns 1 and sets
   frame and len (len 0 = keepalive) if it is authentic and fresh, else 0
   (drop it silently). */
int dngw_open(struct dngw *g, uint8_t *pkt, unsigned pktlen,
              uint8_t **frame, unsigned *len);

/* RFC 8439 AEAD, for the test vectors. out may equal in. */
void dngw_aead_seal(const uint8_t key[32], const uint8_t nonce[12],
                    const uint8_t *aad, unsigned aadlen,
                    const uint8_t *in, unsigned len, uint8_t *out, uint8_t tag[16]);
int  dngw_aead_open(const uint8_t key[32], const uint8_t nonce[12],
                    const uint8_t *aad, unsigned aadlen,
                    const uint8_t *in, unsigned len, uint8_t *out, const uint8_t tag[16]);

#endif

/*
 * dngw.c - client side of the DECnet gateway tunnel, see dngw.h.
 * ChaCha20 and Poly1305 written from RFC 8439 (Poly1305 with five 26-bit
 * limbs).
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>

#include "dngw.h"

/* ---- byte order --------------------------------------------------------- */

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* ---- ChaCha20 (RFC 8439 2.3) ------------------------------------------- */

#define ROTL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d) \
	a += b; d ^= a; d = ROTL(d, 16); \
	c += d; b ^= c; b = ROTL(b, 12); \
	a += b; d ^= a; d = ROTL(d, 8);  \
	c += d; b ^= c; b = ROTL(b, 7)

static void chacha_block(const uint8_t key[32], uint32_t counter,
                         const uint8_t nonce[12], uint8_t out[64])
{
	uint32_t s[16], x[16];
	int i;

	s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
	for (i = 0; i < 8; i++)
		s[4 + i] = le32(key + 4 * i);
	s[12] = counter;
	s[13] = le32(nonce);
	s[14] = le32(nonce + 4);
	s[15] = le32(nonce + 8);
	memcpy(x, s, sizeof x);
	for (i = 0; i < 10; i++) {
		QR(x[0], x[4], x[8],  x[12]);
		QR(x[1], x[5], x[9],  x[13]);
		QR(x[2], x[6], x[10], x[14]);
		QR(x[3], x[7], x[11], x[15]);
		QR(x[0], x[5], x[10], x[15]);
		QR(x[1], x[6], x[11], x[12]);
		QR(x[2], x[7], x[8],  x[13]);
		QR(x[3], x[4], x[9],  x[14]);
	}
	for (i = 0; i < 16; i++)
		put_le32(out + 4 * i, x[i] + s[i]);
}

/* XOR with the key stream from block counter 1 on; out may equal in */
static void chacha_xor(const uint8_t key[32], const uint8_t nonce[12],
                       const uint8_t *in, unsigned len, uint8_t *out)
{
	uint8_t ks[64];
	uint32_t ctr = 1;
	unsigned i, n;

	while (len) {
		chacha_block(key, ctr++, nonce, ks);
		n = len < 64 ? len : 64;
		for (i = 0; i < n; i++)
			out[i] = in[i] ^ ks[i];
		in += n;
		out += n;
		len -= n;
	}
}

/* ---- Poly1305 (RFC 8439 2.5) -------------------------------------------- */

struct poly {
	uint32_t r[5], s[4], h[5], pad[4];
};

static void poly_init(struct poly *p, const uint8_t key[32])
{
	p->r[0] = le32(key) & 0x3ffffff;
	p->r[1] = (le32(key + 3) >> 2) & 0x3ffff03;
	p->r[2] = (le32(key + 6) >> 4) & 0x3ffc0ff;
	p->r[3] = (le32(key + 9) >> 6) & 0x3f03fff;
	p->r[4] = (le32(key + 12) >> 8) & 0x00fffff;
	p->s[0] = p->r[1] * 5;
	p->s[1] = p->r[2] * 5;
	p->s[2] = p->r[3] * 5;
	p->s[3] = p->r[4] * 5;
	memset(p->h, 0, sizeof p->h);
	p->pad[0] = le32(key + 16);
	p->pad[1] = le32(key + 20);
	p->pad[2] = le32(key + 24);
	p->pad[3] = le32(key + 28);
}

/* one full 16-byte block (with the 2^128 bit) */
static void poly_block(struct poly *p, const uint8_t *m)
{
	uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
	uint32_t s1 = p->s[0], s2 = p->s[1], s3 = p->s[2], s4 = p->s[3];
	uint32_t h0, h1, h2, h3, h4, c;
	uint64_t d0, d1, d2, d3, d4;

	h0 = p->h[0] + (le32(m) & 0x3ffffff);
	h1 = p->h[1] + ((le32(m + 3) >> 2) & 0x3ffffff);
	h2 = p->h[2] + ((le32(m + 6) >> 4) & 0x3ffffff);
	h3 = p->h[3] + ((le32(m + 9) >> 6) & 0x3ffffff);
	h4 = p->h[4] + ((le32(m + 12) >> 8) | (1UL << 24));

	d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 +
	     (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
	d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 +
	     (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
	d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 +
	     (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
	d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 +
	     (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
	d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 +
	     (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

	c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
	d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
	d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
	d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
	d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
	h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
	h1 += c;

	p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

/* data, zero-padded to a multiple of 16 (the AEAD's pad16) */
static void poly_pad16(struct poly *p, const uint8_t *m, unsigned len)
{
	uint8_t last[16];

	for (; len >= 16; m += 16, len -= 16)
		poly_block(p, m);
	if (len) {
		memset(last, 0, sizeof last);
		memcpy(last, m, len);
		poly_block(p, last);
	}
}

static void poly_finish(struct poly *p, uint8_t tag[16])
{
	uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
	uint32_t g0, g1, g2, g3, g4, c, mask;
	uint64_t f;

	c = h1 >> 26; h1 &= 0x3ffffff;
	h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
	h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
	h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
	h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
	h1 += c;

	/* h - p = h + 5 - 2^130 */
	g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
	g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
	g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
	g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
	g4 = h4 + c - (1UL << 26);

	/* take g if h >= p, i.e. if g4 did not borrow */
	mask = (g4 >> 31) - 1;
	g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
	mask = ~mask;
	h0 = (h0 & mask) | g0;
	h1 = (h1 & mask) | g1;
	h2 = (h2 & mask) | g2;
	h3 = (h3 & mask) | g3;
	h4 = (h4 & mask) | g4;

	h0 = h0 | (h1 << 26);
	h1 = (h1 >> 6) | (h2 << 20);
	h2 = (h2 >> 12) | (h3 << 14);
	h3 = (h3 >> 18) | (h4 << 8);

	f = (uint64_t)h0 + p->pad[0];             put_le32(tag, (uint32_t)f);
	f = (uint64_t)h1 + p->pad[1] + (f >> 32); put_le32(tag + 4, (uint32_t)f);
	f = (uint64_t)h2 + p->pad[2] + (f >> 32); put_le32(tag + 8, (uint32_t)f);
	f = (uint64_t)h3 + p->pad[3] + (f >> 32); put_le32(tag + 12, (uint32_t)f);
}

/* ---- AEAD (RFC 8439 2.8) ------------------------------------------------- */

static void aead_tag(const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *aad, unsigned aadlen,
                     const uint8_t *ct, unsigned len, uint8_t tag[16])
{
	uint8_t otk[64], lens[16];
	struct poly p;

	chacha_block(key, 0, nonce, otk);
	poly_init(&p, otk);
	poly_pad16(&p, aad, aadlen);
	poly_pad16(&p, ct, len);
	memset(lens, 0, sizeof lens);
	put_le32(lens, aadlen);
	put_le32(lens + 8, len);
	poly_block(&p, lens);
	poly_finish(&p, tag);
	memset(otk, 0, sizeof otk);
}

void dngw_aead_seal(const uint8_t key[32], const uint8_t nonce[12],
                    const uint8_t *aad, unsigned aadlen,
                    const uint8_t *in, unsigned len, uint8_t *out, uint8_t tag[16])
{
	chacha_xor(key, nonce, in, len, out);
	aead_tag(key, nonce, aad, aadlen, out, len, tag);
}

int dngw_aead_open(const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t *aad, unsigned aadlen,
                   const uint8_t *in, unsigned len, uint8_t *out, const uint8_t tag[16])
{
	uint8_t t[16], diff = 0;
	int i;

	aead_tag(key, nonce, aad, aadlen, in, len, t);
	for (i = 0; i < 16; i++)
		diff |= t[i] ^ tag[i];
	if (diff)
		return 0;
	chacha_xor(key, nonce, in, len, out);
	return 1;
}

/* ---- the client line ------------------------------------------------------ */

static const char *word(const char *s, const char **end)
{
	while (*s == ' ' || *s == '\t')
		s++;
	*end = s;
	while (**end && **end != ' ' && **end != '\t' && **end != '\r' && **end != '\n')
		(*end)++;
	return s;
}

static int number(const char *s, const char *e, uint32_t *v)
{
	uint32_t n = 0;

	if (s == e || e - s > 10)
		return 0;
	for (; s < e; s++) {
		if (*s < '0' || *s > '9')
			return 0;
		if (n > 429496729UL || (n == 429496729UL && *s > '5'))
			return 0;
		n = n * 10 + (uint32_t)(*s - '0');
	}
	*v = n;
	return 1;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

int dngw_parse_line(const char *line, char *host, unsigned hostlen,
                    unsigned *port, uint32_t *keyid, unsigned *area,
                    unsigned *node, uint8_t key[32])
{
	const char *s, *e, *dot;
	uint32_t v, a, n;
	int i, hi, lo;

	s = word(line, &e);
	if (e - s != 5 || memcmp(s, "DNGW1", 5))
		return 0;
	s = word(e, &e);                            /* host */
	if (s == e || (unsigned)(e - s) >= hostlen)
		return 0;
	memcpy(host, s, (size_t)(e - s));
	host[e - s] = 0;
	s = word(e, &e);                            /* port */
	if (!number(s, e, &v) || v < 1 || v > 65535)
		return 0;
	*port = (unsigned)v;
	s = word(e, &e);                            /* key id */
	if (!number(s, e, &v) || v == 0)
		return 0;
	*keyid = v;
	s = word(e, &e);                            /* area.node */
	for (dot = s; dot < e && *dot != '.'; dot++)
		;
	if (dot == e || !number(s, dot, &a) || !number(dot + 1, e, &n) ||
	    a < 1 || a > 63 || n < 1 || n > 1023)
		return 0;
	*area = (unsigned)a;
	*node = (unsigned)n;
	s = word(e, &e);                            /* key */
	if (e - s != 64)
		return 0;
	for (i = 0; i < 32; i++) {
		hi = hexval(s[2 * i]);
		lo = hexval(s[2 * i + 1]);
		if (hi < 0 || lo < 0)
			return 0;
		key[i] = (uint8_t)(hi << 4 | lo);
	}
	s = word(e, &e);
	return s == e;                              /* nothing after the key */
}

/* ---- packets ---------------------------------------------------------------- */

uint32_t dngw_start(struct dngw *g, uint32_t keyid, const uint8_t key[32],
                    uint32_t now, uint32_t saved, int have_saved)
{
	uint32_t epoch = now;

	if (have_saved && saved + 1 > epoch)
		epoch = saved + 1;
	memset(g, 0, sizeof *g);
	g->keyid = keyid;
	memcpy(g->key, key, 32);
	g->tx = (uint64_t)epoch << 32;
	g->start = g->tx;
	return epoch;
}

uint32_t dngw_epoch(const struct dngw *g)
{
	return (uint32_t)(g->tx >> 32);
}

static void header(uint8_t *h, uint32_t keyid, int dir, uint64_t ctr)
{
	put_be32(h, keyid);
	h[4] = 1;
	h[5] = (uint8_t)dir;
	h[6] = 0;
	h[7] = 0;
	put_be32(h + 8, (uint32_t)(ctr >> 32));
	put_be32(h + 12, (uint32_t)ctr);
}

unsigned dngw_seal(struct dngw *g, const uint8_t *frame, unsigned len, uint8_t *out)
{
	header(out, g->keyid, 0, g->tx);
	g->tx++;
	dngw_aead_seal(g->key, out + 4, out, DNGW_HDR, frame, len, out + DNGW_HDR,
	               out + DNGW_HDR + len);
	return DNGW_HDR + len + DNGW_TAG;
}

/* replay window: bit i of window = counter rx_max - i has been seen */
static int window_bit(const struct dngw *g, unsigned i)
{
	return (g->window[i >> 3] >> (i & 7)) & 1;
}

static int window_ok(const struct dngw *g, uint64_t ctr)
{
	if (ctr < g->start)
		return 0;
	if (!g->rx_any || ctr > g->rx_max)
		return 1;
	if (g->rx_max - ctr >= 1024)
		return 0;
	return !window_bit(g, (unsigned)(g->rx_max - ctr));
}

static void window_mark(struct dngw *g, uint64_t ctr)
{
	uint64_t shift;
	unsigned bytes, bits, i;

	if (!g->rx_any) {
		g->rx_any = 1;
		g->rx_max = ctr;
		memset(g->window, 0, sizeof g->window);
	} else if (ctr > g->rx_max) {
		shift = ctr - g->rx_max;
		g->rx_max = ctr;
		if (shift >= 1024) {
			memset(g->window, 0, sizeof g->window);
		} else {
			/* move every bit i to i + shift */
			bytes = (unsigned)shift >> 3;
			bits = (unsigned)shift & 7;
			for (i = 128; i-- > 0;) {
				unsigned v = 0;

				if (i >= bytes) {
					v = (unsigned)g->window[i - bytes] << bits;
					if (bits && i > bytes)
						v |= g->window[i - bytes - 1] >> (8 - bits);
				}
				g->window[i] = (uint8_t)v;
			}
		}
	}
	i = (unsigned)(g->rx_max - ctr);
	g->window[i >> 3] |= (uint8_t)(1 << (i & 7));
}

int dngw_open(struct dngw *g, uint8_t *pkt, unsigned pktlen,
              uint8_t **frame, unsigned *len)
{
	uint64_t ctr;
	unsigned n;

	if (pktlen < DNGW_HDR + DNGW_TAG || pktlen > DNGW_MAXPKT)
		return 0;
	if (be32(pkt) != g->keyid || pkt[4] != 1 || pkt[5] != 1 || pkt[6] || pkt[7])
		return 0;
	ctr = ((uint64_t)be32(pkt + 8) << 32) | be32(pkt + 12);
	if (!window_ok(g, ctr))
		return 0;
	n = pktlen - DNGW_HDR - DNGW_TAG;
	if (!dngw_aead_open(g->key, pkt + 4, pkt, DNGW_HDR, pkt + DNGW_HDR, n,
	                    pkt + DNGW_HDR, pkt + DNGW_HDR + n))
		return 0;
	window_mark(g, ctr);
	*frame = pkt + DNGW_HDR;
	*len = n;
	return 1;
}

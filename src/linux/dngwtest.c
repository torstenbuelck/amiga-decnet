/*
 * dngwtest.c - test vectors for the gateway tunnel code (src/dngw):
 * RFC 8439 2.8.2 and the vectors of the decnet-gateway README (5.9),
 * plus the replay window and the client line parser.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <string.h>

#include "../dngw/dngw.h"

static int fails;

static void hex2bin(const char *h, uint8_t *b, unsigned n)
{
	unsigned i, v;

	for (i = 0; i < n; i++) {
		sscanf(h + 2 * i, "%2x", &v);
		b[i] = (uint8_t)v;
	}
}

static void check(const char *what, const uint8_t *got, const char *want_hex)
{
	uint8_t want[256];
	unsigned n = (unsigned)strlen(want_hex) / 2;
	int ok;

	hex2bin(want_hex, want, n);
	ok = !memcmp(got, want, n);
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		fails++;
}

static void expect(const char *what, int cond)
{
	printf("%s %s\n", cond ? "PASS" : "FAIL", what);
	if (!cond)
		fails++;
}

/* a packet from the gateway (direction 1) with counter ctr */
static unsigned gw_packet(const uint8_t key[32], uint32_t keyid, uint64_t ctr,
                          const uint8_t *frame, unsigned len, uint8_t *out)
{
	out[0] = (uint8_t)(keyid >> 24); out[1] = (uint8_t)(keyid >> 16);
	out[2] = (uint8_t)(keyid >> 8); out[3] = (uint8_t)keyid;
	out[4] = 1; out[5] = 1; out[6] = 0; out[7] = 0;
	out[8] = (uint8_t)(ctr >> 56); out[9] = (uint8_t)(ctr >> 48);
	out[10] = (uint8_t)(ctr >> 40); out[11] = (uint8_t)(ctr >> 32);
	out[12] = (uint8_t)(ctr >> 24); out[13] = (uint8_t)(ctr >> 16);
	out[14] = (uint8_t)(ctr >> 8); out[15] = (uint8_t)ctr;
	dngw_aead_seal(key, out + 4, out, 16, frame, len, out + 16, out + 16 + len);
	return 32 + len;
}

static int gw_try(struct dngw *g, const uint8_t key[32], uint64_t ctr)
{
	uint8_t pkt[64], *f;
	unsigned n, len;

	n = gw_packet(key, 7, ctr, NULL, 0, pkt);
	return dngw_open(g, pkt, n, &f, &len) && len == 0;
}

int main(void)
{
	uint8_t key[32], nonce[12], aad[12], out[256], tag[16], pkt[DNGW_MAXPKT], *f;
	const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you "
	                 "only one tip for the future, sunscreen would be it.";
	struct dngw g;
	unsigned i, n, len, port, area, node;
	uint32_t keyid, ep;
	char host[64];

	/* RFC 8439 2.8.2 */
	for (i = 0; i < 32; i++)
		key[i] = (uint8_t)(0x80 + i);
	hex2bin("070000004041424344454647", nonce, 12);
	hex2bin("50515253c0c1c2c3c4c5c6c7", aad, 12);
	dngw_aead_seal(key, nonce, aad, 12, (const uint8_t *)pt, 114, out, tag);
	check("RFC 8439 2.8.2 ciphertext",
	      out, "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
	           "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
	           "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
	           "3ff4def08e4b7a9de576d26586cec64b6116");
	check("RFC 8439 2.8.2 tag", tag, "1ae10b594f09e26a7e902ecbd0600691");
	expect("RFC 8439 2.8.2 opens", dngw_aead_open(key, nonce, aad, 12, out, 114, out, tag) &&
	       !memcmp(out, pt, 114));
	tag[0] ^= 1;
	expect("RFC 8439 2.8.2 bad tag refused", !dngw_aead_open(key, nonce, aad, 12, out, 114, out, tag));

	/* README 5.9: a frame from a client */
	for (i = 0; i < 32; i++)
		key[i] = (uint8_t)i;
	dngw_start(&g, 7, key, 0, 0, 0);
	g.tx = 0x0102030405060708ULL;
	hex2bin("ab0000030000aa0004002a5c60030102", out, 16);
	n = dngw_seal(&g, out, 16, pkt);
	expect("client frame: length 48", n == 48);
	check("client frame: packet", pkt,
	      "00000007010000000102030405060708"
	      "013b072df6d67be5f9e35d1c2d0bf9e3"
	      "d4a2138535475e51c66aa5148a2a2822");
	expect("counter counts up", g.tx == 0x0102030405060709ULL);

	/* README 5.9: a keepalive from the gateway */
	dngw_start(&g, 7, key, 0x6a0e5f80UL, 0, 0);
	hex2bin("00000007010100006a0e5f800000000091c18898051921c6b5c77635fc4970a1", pkt, 32);
	expect("gateway keepalive opens", dngw_open(&g, pkt, 32, &f, &len) && len == 0);
	hex2bin("00000007010100006a0e5f800000000091c18898051921c6b5c77635fc4970a1", pkt, 32);
	expect("same keepalive again refused (replay)", !dngw_open(&g, pkt, 32, &f, &len));

	/* epochs and the window */
	ep = dngw_start(&g, 7, key, 1000, 0, 0);
	expect("start epoch = now", ep == 1000);
	ep = dngw_start(&g, 7, key, 1000, 2000, 1);
	expect("start epoch after a saved later one", ep == 2001);
	ep = dngw_start(&g, 7, key, 5000, 2000, 1);
	expect("start epoch = now when later than saved", ep == 5000);
	expect("counter below own start refused", !gw_try(&g, key, (4999ULL << 32) + 5));
	expect("first counter in epoch", gw_try(&g, key, 5000ULL << 32));
	expect("higher", gw_try(&g, key, (5000ULL << 32) + 10));
	expect("lower, inside window, new", gw_try(&g, key, (5000ULL << 32) + 3));
	expect("lower, inside window, seen", !gw_try(&g, key, (5000ULL << 32) + 3));
	expect("jump by 1000", gw_try(&g, key, (5000ULL << 32) + 1010));
	expect("old one still remembered", !gw_try(&g, key, (5000ULL << 32) + 10));
	expect("new one inside window", gw_try(&g, key, (5000ULL << 32) + 11));
	expect("1008 below, inside the window, new", gw_try(&g, key, (5000ULL << 32) + 2));
	expect("jump past the window", gw_try(&g, key, (5000ULL << 32) + 5000));
	expect("1023 below accepted", gw_try(&g, key, (5000ULL << 32) + 5000 - 1023));
	expect("1024 below refused", !gw_try(&g, key, (5000ULL << 32) + 5000 - 1024));
	n = gw_packet(key, 8, (5000ULL << 32) + 6000, NULL, 0, pkt);
	expect("other key id refused", !dngw_open(&g, pkt, n, &f, &len));
	n = gw_packet(key, 7, (5000ULL << 32) + 6000, NULL, 0, pkt);
	pkt[5] = 0;
	expect("wrong direction refused", !dngw_open(&g, pkt, n, &f, &len));
	n = gw_packet(key, 7, (5000ULL << 32) + 6000, (const uint8_t *)"abcdefghijklmnop", 16, pkt);
	pkt[20] ^= 0x40;
	expect("changed ciphertext refused", !dngw_open(&g, pkt, n, &f, &len));
	expect("refused packet did not move the window", gw_try(&g, key, (5000ULL << 32) + 6000));

	/* the client line */
	expect("line parses",
	       dngw_parse_line("DNGW1 gateway.example.org 26003 3 23.42 "
	                       "000102030405060708090a0b0c0d0e0f101112131415161718191A1B1C1D1E1F\n",
	                       host, sizeof host, &port, &keyid, &area, &node, key) &&
	       !strcmp(host, "gateway.example.org") && port == 26003 && keyid == 3 &&
	       area == 23 && node == 42 && key[31] == 0x1f);
	expect("bad id refused", !dngw_parse_line("DNGW2 h 1 1 1.1 "
	       "0000000000000000000000000000000000000000000000000000000000000000",
	       host, sizeof host, &port, &keyid, &area, &node, key));
	expect("short key refused", !dngw_parse_line("DNGW1 h 1 1 1.1 00",
	       host, sizeof host, &port, &keyid, &area, &node, key));
	expect("key id 0 refused", !dngw_parse_line("DNGW1 h 1 0 1.1 "
	       "0000000000000000000000000000000000000000000000000000000000000000",
	       host, sizeof host, &port, &keyid, &area, &node, key));
	expect("area 64 refused", !dngw_parse_line("DNGW1 h 1 1 64.1 "
	       "0000000000000000000000000000000000000000000000000000000000000000",
	       host, sizeof host, &port, &keyid, &area, &node, key));

	printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails != 0;
}

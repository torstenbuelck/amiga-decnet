/*
 * nsptest - host test of the NSP receive path: segments out of order,
 * a lost segment (NAK), duplicates.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/dn.h"

#define ME    ((1 << 10) | 1)
#define PEER  ((1 << 10) | 2)
#define REM   0x4000

static unsigned char last[1600];
static unsigned lastlen, nframes;
static char got[256];
static int fails;

static void send_frame(void *ctx, const dn_u8 *dst, const dn_u8 *p, unsigned len)
{
	(void)ctx; (void)dst;
	memcpy(last, p, len);
	lastlen = len;
	nframes++;
}
static void *xalloc(void *ctx, unsigned n) { (void)ctx; return malloc(n); }
static void xfree(void *ctx, void *p) { (void)ctx; free(p); }
static void logmsg(void *ctx, int level, const char *m) { (void)ctx; (void)level; printf("  log: %s\n", m); }

static void on_data(struct dn_link *l, const dn_u8 *buf, unsigned len)
{
	(void)l;
	strncat(got, (const char *)buf, len);
}
static struct dn_object obj = { 0, "TEST", 0, on_data, 0, 0, 0 };

/* NSP message from PEER, short data header */
static void input(const dn_u8 *nsp, unsigned len)
{
	dn_u8 f[1600];
	unsigned mlen = 6 + len;

	f[0] = mlen & 0xff; f[1] = mlen >> 8;
	f[2] = 0x22;                            /* short data, intra-Ethernet */
	f[3] = ME & 0xff; f[4] = ME >> 8;
	f[5] = PEER & 0xff; f[6] = PEER >> 8;
	f[7] = 0;
	memcpy(f + 8, nsp, len);
	dn_input((const dn_u8 *)"\xaa\0\4\0\2\4", (const dn_u8 *)"\xaa\0\4\0\1\4", f, 8 + len);
}

static dn_u16 locaddr;

static void seg(int num, const char *text)
{
	dn_u8 m[64];
	unsigned n = strlen(text);

	m[0] = 0x60;
	m[1] = locaddr & 0xff; m[2] = locaddr >> 8;
	m[3] = REM & 0xff; m[4] = REM >> 8;
	m[5] = num & 0xff; m[6] = num >> 8;
	memcpy(m + 7, text, n);
	input(m, 7 + n);
}

/* NSP part of the last frame we sent (short or long header) */
static const dn_u8 *last_nsp(void)
{
	const dn_u8 *m = last + 2;
	return (m[0] & 7) == 6 ? m + 21 : m + 6;
}

static void expect(const char *what, const char *want)
{
	if (strcmp(got, want) != 0) {
		printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
		fails++;
	} else {
		printf("ok   %s: \"%s\"\n", what, got);
	}
}

static void expect_ack(const char *what, unsigned want)
{
	const dn_u8 *m = last_nsp();
	unsigned v = m[5] | m[6] << 8;

	if (m[0] != 0x04 || v != want) {
		printf("FAIL %s: last NSP %02x %04x, want 04 %04x\n", what, m[0], v, want);
		fails++;
	} else {
		printf("ok   %s: %04x\n", what, v);
	}
}

int main(void)
{
	struct dn_env env = { send_frame, xalloc, xfree, logmsg, 0, DN_LOG_DEBUG };
	struct dn_connreq req;
	dn_u8 cc[10];
	int i;

	dn_init(&env, ME);
	memset(&req, 0, sizeof req);
	req.node = PEER;
	req.objnum = 99;
	if (!dn_connect(&req, &obj)) {
		printf("FAIL dn_connect\n");
		return 1;
	}
	locaddr = last_nsp()[3] | last_nsp()[4] << 8;

	cc[0] = 0x28;
	cc[1] = locaddr & 0xff; cc[2] = locaddr >> 8;
	cc[3] = REM & 0xff; cc[4] = REM >> 8;
	cc[5] = 0x01; cc[6] = 0x03; cc[7] = 0xb3; cc[8] = 0x05; cc[9] = 0;
	input(cc, 10);

	seg(1, "a");
	seg(3, "c");
	expect("1, then 3 ahead", "a");
	expect_ack("ack after 3", 0x8001);
	seg(2, "b");
	expect("2 fills the gap", "abc");
	expect_ack("ack after 2", 0x8003);

	seg(5, "e");
	seg(6, "f");
	nframes = 0;
	for (i = 0; i < 3; i++)
		dn_tick(100);
	expect_ack("NAK for missing 4", 0x9003);
	seg(4, "d");
	expect("4 retransmitted", "abcdef");
	expect_ack("ack after 4", 0x8006);

	seg(5, "X");
	expect("duplicate 5 ignored", "abcdef");
	expect_ack("re-ack on duplicate", 0x8006);
	nframes = 0;
	for (i = 0; i < 20; i++)
		dn_tick(100);
	if (nframes) {
		printf("FAIL no gap, yet %u frames sent by ticks\n", nframes);
		fails++;
	} else {
		printf("ok   no NAK without a gap\n");
	}

	/* far ahead, beyond the window: not kept, but NAKed */
	seg(200, "Z");
	seg(7, "g");
	expect("segment far ahead dropped", "abcdefg");

	/* cache full of late segments (FOTOFX resends its last burst first):
	   the segments that close the gap must still be kept */
	for (i = 10; i < 50; i++) {
		char t[2] = { (char)('A' + i % 26), 0 };
		seg(i, t);
	}
	seg(9, "i");
	seg(8, "h");
	expect_ack("gap fillers kept although the cache was full", 0x8000 | 40);

	dn_shutdown();
	printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails != 0;
}

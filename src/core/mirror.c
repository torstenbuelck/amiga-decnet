/*
 * mirror.c - built-in MIRROR object (25), the loopback partner of
 * NCP LOOP NODE. Accepts every connect, announces the maximum loop data
 * length in the connect data and returns each message with its first byte
 * (function code 0 = loop) replaced by 1 (success).
 *
 * Copyright (C) 2026  Torsten
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "dn_priv.h"

#define MIRROR_MAXLEN 4096

static unsigned char mirrorbuf[MIRROR_MAXLEN];

static void mirror_connect(struct dn_link *l, const struct dn_conninfo *ci)
{
	dn_u8 data[2];

	(void)ci;
	PUT16(data, MIRROR_MAXLEN);
	dn_accept(l, data, 2);
}

static void mirror_data(struct dn_link *l, const dn_u8 *buf, unsigned len)
{
	unsigned i;

	if (len == 0 || len > MIRROR_MAXLEN)
		return;
	for (i = 0; i < len; i++)
		mirrorbuf[i] = buf[i];
	mirrorbuf[0] = 1;
	dn_send(l, mirrorbuf, len);
}

static struct dn_object mirror = {
	25, "MIRROR", mirror_connect, mirror_data, 0, 0, 0
};

void dn_mirror_init(void)
{
	dn_register_object(&mirror);
}

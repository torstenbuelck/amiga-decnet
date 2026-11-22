/*
 * dnhost.c - run the portable DECnet core on Linux as a node offering the
 * built-in objects (MIRROR). Needs root / CAP_NET_RAW.
 *
 * Usage: dnhost [-i iface] [-a area.node] [-r router] [-v]
 *   default: -i br0 (falls back to eth0) -a 23.269
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <unistd.h>

#include "dnenv.h"

int main(int argc, char **argv)
{
	const char *ifname = NULL;
	dn_u16 addr = (23 << 10) | 269, router;
	int c, verbose = 1;

	while ((c = getopt(argc, argv, "i:a:r:v")) != -1) {
		switch (c) {
		case 'i': ifname = optarg; break;
		case 'a':
			if (dn_aton(optarg, &addr)) {
				fprintf(stderr, "bad address %s\n", optarg);
				return 1;
			}
			break;
		case 'r':
			if (dn_aton(optarg, &router)) {
				fprintf(stderr, "bad router %s\n", optarg);
				return 1;
			}
			dnenv_set_router(router);
			break;
		case 'v': verbose++; break;
		default:
			fprintf(stderr, "usage: %s [-i iface] [-a area.node] [-r router] [-v]\n", argv[0]);
			return 1;
		}
	}
	setvbuf(stderr, NULL, _IOLBF, 0);
	if (dnenv_open(ifname, addr, verbose))
		return 1;
	dn_mirror_init();
	fprintf(stderr, "dnhost: node %s\n", dn_ntoa(addr));
	while (!dnenv_stopped())
		dnenv_pump(100);
	dnenv_close();
	fprintf(stderr, "dnhost: stopped\n");
	return 0;
}

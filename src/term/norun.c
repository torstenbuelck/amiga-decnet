/*
 * norun.c - first code of dnterm.device: running the device as a
 * program just returns. A file of its own, because dnterm.c gets inline
 * stubs from the system headers in front of its own code.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
long DevNoRun(void)
{
	return -1;
}

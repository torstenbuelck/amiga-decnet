/*
 * start.c - entry point of dn-handler. Linked first (no C startup), so this
 * must stay the only code in this file.
 *
 * Copyright (C) 2026  Torsten
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>

extern struct ExecBase *SysBase;
void handler_main(void);

void start(void)
{
	SysBase = *(struct ExecBase **)4;
	handler_main();
}

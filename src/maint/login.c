/*
 * login.c - password dialogue before the Shell starts.
 *
 * "Password:" as a secret read (no echo), MAINT_TRIES attempts with a
 * delay after each wrong one, then the session ends.
 *
 * Framework only: the bodies are still empty.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#include "dnmaint.h"

/* banner and first password prompt */
int login_start(struct msession *s)
{
	/* TODO */
	return -1;
}

/* a password line arrived; 1 = logged in, 0 = asked again, <0 = give up */
int login_input(struct msession *s, const char *buf, long n, int term)
{
	/* TODO */
	return -1;
}

/* compare with the configured password */
BOOL login_check(const struct mcfg *cfg, const char *buf, long n)
{
	/* TODO */
	return FALSE;
}

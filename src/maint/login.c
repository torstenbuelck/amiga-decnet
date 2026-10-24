/*
 * login.c - password dialogue before the Shell starts.
 *
 * "Password:" as a secret read (no echo), MAINT_TRIES attempts with a
 * delay after each wrong one, then the session ends.
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

static void say(struct msession *s, const char *text)
{
	ctermh_write(&s->cth, text, (long)strlen(text));
}

static int prompt(struct msession *s)
{
	static const char pw[] = "Password: ";

	return ctermh_read(&s->cth, CTH_SECRET, pw, sizeof pw - 1, NULL, 0);
}

/* banner and first password prompt */
int login_start(struct msession *s)
{
	s->tries = 0;
	/* nothing to log in to: refuse before anything is asked */
	if (!s->cfg || !s->cfg->password[0]) {
		say(s, "\nNo maintenance password set on this Amiga.\n");
		return -1;
	}
	say(s, "\nAmiga maintenance login (dnmaint)\n\n");
	return prompt(s);
}

/* a password line arrived; 1 = logged in, 0 = asked again, <0 = give up */
int login_input(struct msession *s, const char *buf, long n, int term)
{
	(void)term;                     /* every terminator ends an attempt */
	say(s, "\n");                   /* the secret read did not echo it */
	if (login_check(s->cfg, buf, n)) {
		say(s, "Welcome.\n\n");
		return 1;
	}
	/* slow down guessing; the session clock just stands still */
	Delay(MAINT_FAILDELAY);
	if (++s->tries >= MAINT_TRIES) {
		say(s, "Access denied.\n");
		return -1;
	}
	say(s, "Wrong password.\n");
	return prompt(s) ? -1 : 0;
}

/* compare with the configured password; no password set never matches.
   Looks at every character, so the time does not tell how much was right */
BOOL login_check(const struct mcfg *cfg, const char *buf, long n)
{
	long len, i;
	UBYTE diff;

	if (!cfg || !cfg->password[0] || n <= 0)
		return FALSE;
	len = (long)strlen(cfg->password);
	diff = (UBYTE)(len != n);
	for (i = 0; i < n; i++)
		diff |= (UBYTE)buf[i] ^ (UBYTE)cfg->password[i < len ? i : 0];
	return diff == 0;
}

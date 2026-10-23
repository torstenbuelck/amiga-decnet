/*
 * s2buff.h - SANA-II buffer management hooks and bounded AbortIO, shared
 * by dnetd and dncheck.
 *
 * Some drivers (seen with a2065.device on a real A3000) never give back an
 * aborted CMD_READ. A plain WaitIO() then hangs for good, and if the
 * program leaves the request with the driver and exits, the driver still
 * calls the copy hooks when a frame arrives at last. Hooks in the
 * program's own code would then run in unloaded memory (guru). So the
 * hooks are a small stub in allocated memory that calls exec's CopyMem
 * (in ROM); a program with a stuck request leaks the stub, the request,
 * its buffer and the port instead of freeing them.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef S2BUFF_H
#define S2BUFF_H

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <devices/sana2.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>

/* CopyToBuff and CopyFromBuff alike: copy d0 bytes from a1 to a0, return
   TRUE. CopyMem takes source in a0, destination in a1. */
static const UWORD s2buff_code[] = {
	0x2f0e,                 /* move.l  a6,-(sp)         */
	0xc149,                 /* exg     a0,a1            */
	0x2c78, 0x0004,         /* movea.l 4.w,a6           */
	0x4eae, 0xfd90,         /* jsr     -624(a6) CopyMem */
	0x2c5f,                 /* movea.l (sp)+,a6         */
	0x7001,                 /* moveq   #1,d0            */
	0x4e75                  /* rts                      */
};

struct s2buff {
	struct TagItem tags[3];
	UWORD code[sizeof s2buff_code / sizeof s2buff_code[0]];
};

/* Returns the tag list for ios2_BufferManagement, or NULL. */
static struct TagItem *s2buff_create(void)
{
	struct s2buff *b = AllocVec(sizeof *b, MEMF_PUBLIC);

	if (!b)
		return NULL;
	CopyMem((APTR)s2buff_code, b->code, sizeof b->code);
	CacheClearU();
	b->tags[0].ti_Tag = S2_CopyToBuff;
	b->tags[0].ti_Data = (ULONG)b->code;
	b->tags[1].ti_Tag = S2_CopyFromBuff;
	b->tags[1].ti_Data = (ULONG)b->code;
	b->tags[2].ti_Tag = TAG_DONE;
	b->tags[2].ti_Data = 0;
	return b->tags;
}

/* Only when no request is left with the driver. */
static void s2buff_free(struct TagItem *tags)
{
	FreeVec(tags);
}

/* Aborts r and waits for it at most about 2 s. Returns TRUE when r is back
   (and removed from its reply port); FALSE when the driver keeps it. */
static BOOL s2_abort(struct IORequest *r)
{
	int ticks;

	if (!CheckIO(r)) {
		AbortIO(r);
		for (ticks = 0; ticks < 100 && !CheckIO(r); ticks += 5)
			Delay(5);
		if (!CheckIO(r))
			return FALSE;
	}
	WaitIO(r);
	return TRUE;
}

/* After a request got stuck: a late reply must not signal a task that has
   gone. The port itself stays allocated. */
static void s2_orphan_port(struct MsgPort *p)
{
	p->mp_Flags = PA_IGNORE;
}

#endif

/*
 * dnserver.c - dnetd's public port "DECnet": serves decnet.library calls.
 *
 * Each library call arrives as a DnMsg. Connects and reads may stay
 * pending (until the link is up, data arrives, the link goes away or the
 * caller's timeout expires), and so may a write while more than
 * WRITE_WINDOW segments are unacknowledged; everything else is replied
 * at once.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>

#include <proto/exec.h>

#include <string.h>

#include "../core/dn.h"
#include "dnmsg.h"
#include "dnserver.h"

#define MAXSRV 16
/* A write waits while more segments than this are unacknowledged, so a
   writer cannot pile up a whole file in dnetd's memory. */
#define WRITE_WINDOW 16

struct qmsg {
	struct qmsg *next;
	ULONG len;
	UBYTE data[1];
};

struct srv {
	int used;
	ULONG id;
	struct dn_link *link;
	int state;                  /* 0 connecting, 1 up, 2 gone */
	LONG reason;
	struct qmsg *head, *tail;
	struct DnMsg *pconnect;     /* pending DNC_CONNECT */
	ULONG connect_deadline;
	struct DnMsg *pread;        /* pending DNC_READ */
	ULONG read_deadline;
	struct DnMsg *pwrite;       /* DNC_WRITE waiting for acknowledgements */
	ULONG write_deadline;
};

static struct MsgPort *pubport;
static struct srv srvs[MAXSRV];
static ULONG generation;
static ULONG now;               /* ms, advanced by srv_tick */

static void reply(struct DnMsg *m, LONG result, LONG reason)
{
	m->dm_Result = result;
	m->dm_Reason = reason;
	ReplyMsg(&m->dm_Msg);
}

static struct srv *find_srv(ULONG id)
{
	struct srv *s = &srvs[id & 0xff];

	if ((id & 0xff) >= MAXSRV || !s->used || s->id != id)
		return NULL;
	return s;
}

static void free_queue(struct srv *s)
{
	struct qmsg *q, *n;

	for (q = s->head; q; q = n) {
		n = q->next;
		FreeVec(q);
	}
	s->head = s->tail = NULL;
}

/* Satisfy a pending read from the queue, or fail it if the link is gone. */
static void serve_read(struct srv *s)
{
	struct DnMsg *m = s->pread;
	struct qmsg *q;
	ULONG n;

	if (!m)
		return;
	q = s->head;
	if (q) {
		s->head = q->next;
		if (!s->head)
			s->tail = NULL;
		n = q->len < (ULONG)m->dm_Len ? q->len : (ULONG)m->dm_Len;
		CopyMem(q->data, m->dm_Buf, n);
		FreeVec(q);
		s->pread = NULL;
		reply(m, (LONG)n, 0);
	} else if (s->state == 2) {
		s->pread = NULL;
		reply(m, DNR_CLOSED, s->reason);
	}
}

/* ---- core callbacks ------------------------------------------------------- */

static void cb_connected(struct dn_link *l, const dn_u8 *data, unsigned len)
{
	struct srv *s = dn_get_user(l);

	(void)data;
	(void)len;
	if (!s)
		return;
	s->state = 1;
	if (s->pconnect) {
		s->pconnect->dm_Link = s->id;
		reply(s->pconnect, 0, 0);
		s->pconnect = NULL;
	}
}

static void cb_data(struct dn_link *l, const dn_u8 *buf, unsigned len)
{
	struct srv *s = dn_get_user(l);
	struct qmsg *q;

	if (!s)
		return;
	q = AllocVec(sizeof(struct qmsg) + len, MEMF_ANY);
	if (!q)
		return;
	q->next = NULL;
	q->len = len;
	CopyMem((APTR)buf, q->data, len);
	if (s->tail)
		s->tail->next = q;
	else
		s->head = q;
	s->tail = q;
	serve_read(s);
}

static void cb_disconnect(struct dn_link *l, dn_u16 reason)
{
	struct srv *s = dn_get_user(l);

	if (!s)
		return;
	s->state = 2;
	s->reason = reason;
	s->link = NULL;
	if (s->pconnect) {
		/* connect failed: nobody will ever see this id */
		reply(s->pconnect, -1, reason);
		s->pconnect = NULL;
		free_queue(s);
		s->used = 0;
		return;
	}
	if (s->pwrite) {
		reply(s->pwrite, -1, reason);
		s->pwrite = NULL;
	}
	serve_read(s);
}

static struct dn_object handler = {
	0, NULL, NULL, cb_data, cb_disconnect, NULL, cb_connected
};

/* ---- requests ----------------------------------------------------------------- */

static void do_connect(struct DnMsg *m)
{
	struct DnConnectReq *r = m->dm_Req;
	struct dn_connreq req;
	struct srv *s = NULL;
	int i;

	for (i = 0; i < MAXSRV; i++)
		if (!srvs[i].used) {
			s = &srvs[i];
			break;
		}
	if (!s) {
		reply(m, -1, DNREASON_RESOURCES);
		return;
	}
	memset(s, 0, sizeof *s);
	s->used = 1;
	s->id = (ULONG)i | (++generation << 8);

	memset(&req, 0, sizeof req);
	req.node = r->dcr_Node;
	req.objnum = r->dcr_ObjNum;
	req.objname = (const char *)r->dcr_ObjName;
	req.srcname = (const char *)r->dcr_SrcName;
	req.user = (const char *)r->dcr_User;
	req.password = (const char *)r->dcr_Password;
	req.account = (const char *)r->dcr_Account;
	if ((r->dcr_Flags & DCRF_DATA) && r->dcr_Data && r->dcr_DataLen <= 16) {
		req.data = r->dcr_Data;
		req.datalen = r->dcr_DataLen;
	}
	s->pconnect = m;
	s->connect_deadline = now + (r->dcr_Timeout > 0 ? (ULONG)r->dcr_Timeout : 30000UL);
	s->link = dn_connect(&req, &handler);
	if (!s->link) {
		s->used = 0;
		reply(m, -1, DNREASON_RESOURCES);
		return;
	}
	dn_set_user(s->link, s);
}

static void do_close(struct srv *s)
{
	if (s->link) {
		dn_set_user(s->link, NULL);
		dn_disconnect(s->link, DNREASON_NORMAL, NULL, 0);
	}
	if (s->pread)
		reply(s->pread, DNR_CLOSED, DNREASON_NOLINK);
	if (s->pwrite)
		reply(s->pwrite, -1, DNREASON_NOLINK);
	free_queue(s);
	s->used = 0;
}

static void handle(struct DnMsg *m)
{
	struct srv *s;

	if (m->dm_Cmd == DNC_CONNECT) {
		do_connect(m);
		return;
	}
	s = find_srv(m->dm_Link);
	if (!s) {
		reply(m, -1, DNREASON_NOLINK);
		return;
	}
	switch (m->dm_Cmd) {
	case DNC_READ:
		if (s->pread) {                 /* one read at a time */
			reply(m, -1, DNREASON_RESOURCES);
			break;
		}
		s->pread = m;
		s->read_deadline = now + (m->dm_Timeout > 0 ? (ULONG)m->dm_Timeout : 0x7fffffffUL);
		serve_read(s);
		break;
	case DNC_WRITE:
		if (s->state != 1 || !s->link || m->dm_Len < 0 ||
		    dn_send(s->link, m->dm_Buf, (unsigned)m->dm_Len) < 0) {
			reply(m, -1, s->state == 2 ? s->reason : DNREASON_NOLINK);
		} else if (dn_pending(s->link) > WRITE_WINDOW && !s->pwrite) {
			s->pwrite = m;          /* replied by srv_poll() */
			s->write_deadline = now + 60000UL;
		} else {
			reply(m, 0, 0);
		}
		break;
	case DNC_CLOSE:
		do_close(s);
		reply(m, 0, 0);
		break;
	case DNC_REASON:
		reply(m, 0, s->reason);
		break;
	default:
		reply(m, -1, DNREASON_RESOURCES);
	}
}

/* ---- dnetd interface ------------------------------------------------------------ */

ULONG srv_init(void)
{
	Forbid();
	if (FindPort((STRPTR)DN_PORTNAME)) {
		Permit();
		return 0;                       /* another dnetd is running */
	}
	pubport = CreateMsgPort();
	if (pubport) {
		pubport->mp_Node.ln_Name = (char *)DN_PORTNAME;
		pubport->mp_Node.ln_Pri = 0;
		AddPort(pubport);
	}
	Permit();
	return pubport ? 1UL << pubport->mp_SigBit : 0;
}

void srv_handle(void)
{
	struct DnMsg *m;

	while ((m = (struct DnMsg *)GetMsg(pubport)) != NULL)
		handle(m);
}

/* Let waiting writers go on once their segments are acknowledged. Call
   after incoming frames. */
void srv_poll(void)
{
	int i;

	for (i = 0; i < MAXSRV; i++) {
		struct srv *s = &srvs[i];
		if (s->used && s->pwrite && s->link && dn_pending(s->link) <= WRITE_WINDOW / 2) {
			struct DnMsg *m = s->pwrite;
			s->pwrite = NULL;
			reply(m, 0, 0);
		}
	}
}

void srv_tick(unsigned ms)
{
	int i;

	now += ms;
	for (i = 0; i < MAXSRV; i++) {
		struct srv *s = &srvs[i];
		if (!s->used)
			continue;
		if (s->pread && (LONG)(now - s->read_deadline) >= 0) {
			struct DnMsg *m = s->pread;
			s->pread = NULL;
			reply(m, DNR_TIMEOUT, 0);
		}
		if (s->pwrite && (LONG)(now - s->write_deadline) >= 0) {
			struct DnMsg *m = s->pwrite;
			s->pwrite = NULL;
			reply(m, -1, DNREASON_TIMEOUT);
		}
		if (s->pconnect && (LONG)(now - s->connect_deadline) >= 0) {
			struct DnMsg *m = s->pconnect;
			s->pconnect = NULL;
			if (s->link) {
				dn_set_user(s->link, NULL);
				dn_disconnect(s->link, DNREASON_NORMAL, NULL, 0);
			}
			free_queue(s);
			s->used = 0;
			reply(m, -1, DNREASON_TIMEOUT);
		}
	}
}

/* Remove the port and fail everything still waiting. Call before
   dn_shutdown(). */
void srv_shutdown(void)
{
	struct DnMsg *m;
	int i;

	if (!pubport)
		return;
	Forbid();
	RemPort(pubport);
	Permit();
	while ((m = (struct DnMsg *)GetMsg(pubport)) != NULL)
		reply(m, -1, DNREASON_SHUTDOWN);
	for (i = 0; i < MAXSRV; i++) {
		struct srv *s = &srvs[i];
		if (!s->used)
			continue;
		if (s->link)
			dn_set_user(s->link, NULL);
		if (s->pconnect)
			reply(s->pconnect, -1, DNREASON_SHUTDOWN);
		if (s->pread)
			reply(s->pread, DNR_CLOSED, DNREASON_SHUTDOWN);
		if (s->pwrite)
			reply(s->pwrite, -1, DNREASON_SHUTDOWN);
		free_queue(s);
		s->used = 0;
	}
	DeleteMsgPort(pubport);
	pubport = NULL;
}

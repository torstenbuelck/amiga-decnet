#ifndef LIBRARIES_DECNET_H
#define LIBRARIES_DECNET_H
/*
 * libraries/decnet.h - decnet.library definitions.
 *
 * decnet.library is the application interface to the DECnet Phase IV
 * endnode dnetd: logical links to objects on other nodes.
 *
 * Copyright (C) 2026  Torsten
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif

#define DECNETNAME    "decnet.library"
#define DECNET_VERSION 1

/* Opaque logical link handle. Use it only from the task that created it. */
struct DnLink;

/* Connect request for DnConnect(). Object by number, or by name if
   dcr_ObjNum is 0. String pointers may be NULL. */
struct DnConnectReq {
	UWORD        dcr_Node;          /* node address, area << 10 | number */
	UBYTE        dcr_ObjNum;
	UBYTE        dcr_Flags;         /* DCRF_..., 0 for the original fields only */
	CONST_STRPTR dcr_ObjName;
	CONST_STRPTR dcr_SrcName;       /* our end user name, default "DECNET" */
	CONST_STRPTR dcr_User;          /* access control */
	CONST_STRPTR dcr_Password;
	CONST_STRPTR dcr_Account;
	LONG         dcr_Timeout;       /* milliseconds, 0 = 30 s */
	LONG         dcr_Reason;        /* out: NSP reason if DnConnect fails */
	/* only read with DCRF_DATA in dcr_Flags (older callers do not have
	   these fields): optional connect data, at most 16 bytes */
	CONST UBYTE *dcr_Data;
	UWORD        dcr_DataLen;
};

#define DCRF_DATA     (1 << 0)          /* dcr_Data / dcr_DataLen are valid */

/* DnRead() results besides the message length */
#define DNR_TIMEOUT   0
#define DNR_CLOSED   (-1)

/* NSP disconnect / reject reasons (dcr_Reason, DnLinkReason()) */
#define DNREASON_NORMAL       0
#define DNREASON_RESOURCES    1
#define DNREASON_UNKNOWNNODE  2
#define DNREASON_SHUTDOWN     3
#define DNREASON_NOOBJECT     4
#define DNREASON_BUSY         6
#define DNREASON_ABORTED      9
#define DNREASON_ACCESS      34
#define DNREASON_ACCOUNT     36
#define DNREASON_TIMEOUT     38
#define DNREASON_UNREACHABLE 39
#define DNREASON_NOLINK      41
#define DNREASON_NODNETD    256         /* dnetd is not running */

#endif

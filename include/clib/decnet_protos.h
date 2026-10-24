/* Automatically generated header (sfdc 1.12)! Do not edit! */

#ifndef CLIB_DECNET_PROTOS_H
#define CLIB_DECNET_PROTOS_H

/*
**   $VER: decnet_protos.h 1.2 $Id: decnet_lib.sfd 1.2 (24.10.2026) $
**
**   C prototypes. For use with 32 bit integers only.
**
**   Copyright (c) 2001 Amiga, Inc.
**       All Rights Reserved
*/

#include <exec/types.h>
#include <libraries/decnet.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* decnet.library - DECnet Phase IV logical links for AmigaOS */
/* Copyright (C) 2026  Torsten Bülck (tuti), GPL-2.0-or-later */
struct DnLink * DnConnect(struct DnConnectReq * req);
LONG DnRead(struct DnLink * link, APTR buffer, LONG length, LONG timeout);
LONG DnWrite(struct DnLink * link, CONST_APTR buffer, LONG length);
VOID DnClose(struct DnLink * link);
LONG DnLinkReason(struct DnLink * link);
LONG DnNodeAddr(CONST_STRPTR name);
struct DnListener * DnListen(LONG objnum, CONST_STRPTR objname);
struct DnLink * DnAccept(struct DnListener * listener, struct DnConnInfo * info, LONG timeout);
LONG DnAnswer(struct DnLink * link, LONG reason, CONST_APTR data, LONG length);
VOID DnUnlisten(struct DnListener * listener);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CLIB_DECNET_PROTOS_H */

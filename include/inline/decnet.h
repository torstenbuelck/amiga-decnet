/* Automatically generated header (sfdc 1.12)! Do not edit! */

#ifndef _INLINE_DECNET_H
#define _INLINE_DECNET_H

#ifndef _SFDC_VARARG_DEFINED
#define _SFDC_VARARG_DEFINED
#ifdef __HAVE_IPTR_ATTR__
typedef APTR _sfdc_vararg __attribute__((iptr));
#else
typedef ULONG _sfdc_vararg;
#endif /* __HAVE_IPTR_ATTR__ */
#endif /* _SFDC_VARARG_DEFINED */

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif /* !__INLINE_MACROS_H */

#ifndef DECNET_BASE_NAME
#define DECNET_BASE_NAME DECnetBase
#endif /* !DECNET_BASE_NAME */

#define DnConnect(___req) \
      LP1(0x1e, struct DnLink *, DnConnect , struct DnConnectReq *, ___req, a0,\
      , DECNET_BASE_NAME)

#define DnRead(___link, ___buffer, ___length, ___timeout) \
      LP4(0x24, LONG, DnRead , struct DnLink *, ___link, a0, APTR, ___buffer, a1, LONG, ___length, d0, LONG, ___timeout, d1,\
      , DECNET_BASE_NAME)

#define DnWrite(___link, ___buffer, ___length) \
      LP3(0x2a, LONG, DnWrite , struct DnLink *, ___link, a0, CONST_APTR, ___buffer, a1, LONG, ___length, d0,\
      , DECNET_BASE_NAME)

#define DnClose(___link) \
      LP1NR(0x30, DnClose , struct DnLink *, ___link, a0,\
      , DECNET_BASE_NAME)

#define DnLinkReason(___link) \
      LP1(0x36, LONG, DnLinkReason , struct DnLink *, ___link, a0,\
      , DECNET_BASE_NAME)

#define DnNodeAddr(___name) \
      LP1(0x3c, LONG, DnNodeAddr , CONST_STRPTR, ___name, a0,\
      , DECNET_BASE_NAME)

#define DnListen(___objnum, ___objname) \
      LP2(0x42, struct DnListener *, DnListen , LONG, ___objnum, d0, CONST_STRPTR, ___objname, a0,\
      , DECNET_BASE_NAME)

#define DnAccept(___listener, ___info, ___timeout) \
      LP3(0x48, struct DnLink *, DnAccept , struct DnListener *, ___listener, a0, struct DnConnInfo *, ___info, a1, LONG, ___timeout, d0,\
      , DECNET_BASE_NAME)

#define DnAnswer(___link, ___reason, ___data, ___length) \
      LP4(0x4e, LONG, DnAnswer , struct DnLink *, ___link, a0, LONG, ___reason, d0, CONST_APTR, ___data, a1, LONG, ___length, d1,\
      , DECNET_BASE_NAME)

#define DnUnlisten(___listener) \
      LP1NR(0x54, DnUnlisten , struct DnListener *, ___listener, a0,\
      , DECNET_BASE_NAME)

#endif /* !_INLINE_DECNET_H */

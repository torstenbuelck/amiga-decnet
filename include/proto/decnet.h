/* Automatically generated header (sfdc 1.12)! Do not edit! */

#ifndef PROTO_DECNET_H
#define PROTO_DECNET_H

#include <clib/decnet_protos.h>

#if defined(_CONST_BASES)
# ifndef __CONSTLIBBASEDECL__
# define __CONSTLIBBASEDECL__ const
# endif /* __CONSTLIBBASEDECL__ */
# ifndef __SEGMENTLIBBASEDECL__
# define __SEGMENTLIBBASEDECL__  __attribute__((__section__(".data")))
# endif /* __SEGMENTLIBBASEDECL__ */
#endif /* _CONST_BASES */
#ifdef __amigaos4__
# include <interfaces/decnet.h>
# ifndef __NOGLOBALIFACE__
   extern struct DECnetIFace *IDECnet;
# endif /* __NOGLOBALIFACE__*/
#endif /* !__amigaos4__ */
#ifndef __NOLIBBASE__
  extern struct Library *
# ifdef __CONSTLIBBASEDECL__
   __CONSTLIBBASEDECL__
# endif /* __CONSTLIBBASEDECL__ */
  DECnetBase
# ifdef __SEGMENTLIBBASEDECL__
 __SEGMENTLIBBASEDECL__
# endif /* __SEGMENTLIBBASEDECL__ */
;
#endif /* !__NOLIBBASE__ */

#ifndef _NO_INLINE
# if defined(__GNUC__)
#  ifdef __AROS__
#   include <defines/decnet.h>
#  else
#   include <inline/decnet.h>
#  endif
# else
#  include <pragmas/decnet_pragmas.h>
# endif
#endif /* _NO_INLINE */

#endif /* !PROTO_DECNET_H */

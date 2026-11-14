#ifndef _INCLUDE_PRAGMA_PCCARD_LIB_H
#define _INCLUDE_PRAGMA_PCCARD_LIB_H

#ifndef CLIB_PCCARD_PROTOS_H
#include <clib/pccard_protos.h>
#endif

#if defined(AZTEC_C) || defined(__MAXON__) || defined(__STORM__)
#pragma amicall(PCCardBase,0x01e,PCCard_GetTupleInfo(a0))
#pragma amicall(PCCardBase,0x024,PCCard_FreeTupleInfo(a0))
#pragma amicall(PCCardBase,0x02a,PCCard_GetFuncETupleInfo(a0,d0))
#endif
#if defined(_DCC) || defined(__SASC)
#pragma  libcall PCCardBase PCCard_GetTupleInfo    01e 801
#pragma  libcall PCCardBase PCCard_FreeTupleInfo   024 801
#pragma  libcall PCCardBase PCCard_GetFuncETupleInfo 02a 0802
#endif

#endif	/*  _INCLUDE_PRAGMA_PCCARD_LIB_H  */

#ifndef _INLINE_PCCARD_H
#define _INLINE_PCCARD_H

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif

#ifndef PCCARD_BASE_NAME
#define PCCARD_BASE_NAME PCCardBase
#endif

#define PCCard_GetTupleInfo(tuple) \
	LP1(0x1e, const struct TagItem *, PCCard_GetTupleInfo, const UBYTE *, tuple, a0, \
	, PCCARD_BASE_NAME)

#define PCCard_FreeTupleInfo(tagList) \
	LP1NR(0x24, PCCard_FreeTupleInfo, const struct TagItem *, tagList, a0, \
	, PCCARD_BASE_NAME)

#define PCCard_GetFuncETupleInfo(tuple, funcID) \
	LP2(0x2a, const struct TagItem *, PCCard_GetFuncETupleInfo, const UBYTE *, tuple, a0, const ULONG, funcID, d0, \
	, PCCARD_BASE_NAME)

#endif /*  _INLINE_PCCARD_H  */

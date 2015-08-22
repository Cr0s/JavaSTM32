#ifndef _UJ_HEAP_H_
#define _UJ_HEAP_H_

#include "common.h"

//init & debug
#define UJ_HEAP_SZ		HEAP_SZ

#define UJ_HEAP_MAX_HANDLES	(UJ_HEAP_SZ / 8)

#if UJ_HEAP_MAX_HANDLES < (1UL << 8)
 #define HANDLE	UInt8
 #define HANDLE_SZ HEAP_ALIGN
#elif UJ_HEAP_MAX_HANDLES < (1UL << 16)
 #define HANDLE	UInt16
 #define HANDLE_SZ (2 > HEAP_ALIGN ? 2 : HEAP_ALIGN)
#elif UJ_HEAP_MAX_HANDLES < (1UL << 32)
 #define HANDLE	UInt32
 #define HANDLE_SZ (4 > HEAP_ALIGN ? 4 : HEAP_ALIGN)
#else
 #error "too many heap handles possible!"
#endif



void ujHeapInit(void);
void ujHeapDebug(void);

HANDLE ujHeapHandleNew(UInt16 sz);
void ujHeapHandleFree(HANDLE handle);
void* ujHeapAllocNonmovable(UInt16 sz);

void* ujHeapHandleLock(HANDLE handle);
void ujHeapHandleRelease(HANDLE handle);
void* ujHeapHandleIsLocked(HANDLE handle);	//return pointer if already locked, else NULL

void ujHeapUnmarkAll(void);
void ujHeapFreeUnmarked(void);
HANDLE ujHeapFirstMarked(UInt8 markVal);	//get first handle with a given mark value
void ujHeapMark(HANDLE handle, UInt8 mark);	//will only increase the mark value
UInt8 ujHeapGetMark(HANDLE handle);


#endif

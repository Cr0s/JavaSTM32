#include "ujHeap.h"
#include "uj.h"
#include "common.h"

#ifdef DEBUG_HEAP
#include <stdio.h>
#define pr fprintf
#define pe(...) do{ perr(__VA_ARGS__); ujHeapDebug(); }while(0)
#else
#define pr(...)
#define pe(...)
#endif



#if UJ_HEAP_SZ <= (1 << 8)
 #define SIZE	UInt8
#elif UJ_HEAP_SZ <= (1 << 16)
 #define SIZE	UInt16
#elif UJ_HEAP_SZ <= (1 << 24)
 #define SIZE	UInt24
#elif UJ_HEAP_SZ <= (1 << 32)
 #define SIZE	UInt32
#else
 #error "heap too big!"
#endif

#define INITIAL_NUM_HANDLES	UJ_HEAP_SZ / 8 / sizeof(SIZE)


static UInt8 _HEAP_ATTRS_ __attribute__ ((aligned (HEAP_ALIGN))) gHeap[UJ_HEAP_SZ];

typedef struct{
	
	HANDLE numHandles;	//handles are at start of heap
	UInt8 data[] __attribute__ ((aligned (HANDLE_SZ)));

}UjHeapHdr;

typedef struct{

	SIZE	size;
	
	UInt8	free : 1;
	UInt8	lock : 1;
	UInt8	mark : 2;
	UInt8	wsze : 4;	//wasted size (already included in "size") XXX: ASSERT(CHUNK_HDR_SZ < (1 << wsze))
	
	UInt8	data[] __attribute__ ((aligned (HEAP_ALIGN)));

}UjHeapChunk;

#define CHUNK_HDR_SZ		(((offsetof(UjHeapChunk, data)) + HEAP_ALIGN - 1) &~ (HEAP_ALIGN - 1))

#ifdef DEBUG_HEAP
	static void perr(const char* err){

		fprintf(stderr, "%s", err);
	}
#endif

static UjHeapChunk* ujHeapPrvGetFirstChunk(void){

	UjHeapHdr* hdr = (UjHeapHdr*)gHeap;
	UInt32 ptr;
	
	ptr = (UInt32)(hdr->data + sizeof(SIZE) * hdr->numHandles);
	ptr = (ptr + HEAP_ALIGN - 1) &~ (HEAP_ALIGN - 1);
	return (UjHeapChunk*)ptr;
}

static UjHeapChunk* ujHeapPrvGetNextChunk(UjHeapChunk* c){

	c = (UjHeapChunk*)(c->data + c->size);
	
	if((UInt8*)c >= gHeap + UJ_HEAP_SZ) c = NULL;
	
	return c;
}

static UjHeapChunk* ujHeapPrvGetPrevChunk(UjHeapChunk* c){	//VERY SLOW!!!!!!!

	UjHeapChunk *a, *t = ujHeapPrvGetFirstChunk();
	
	while(t){
	
		a = ujHeapPrvGetNextChunk(t);
		if(a == c) return t;
		t = a;
	}
	
	return NULL;
}

void ujHeapInit(void){

	UjHeapHdr* hdr = (UjHeapHdr*)gHeap;
	UjHeapChunk* chk;
	SIZE* handleTable = (SIZE*)hdr->data;
	SIZE i;
	
	hdr->numHandles = INITIAL_NUM_HANDLES;
	
	for(i = 0; i < INITIAL_NUM_HANDLES; i++){
	
		handleTable[i] = 0;	
	}
	
	chk = ujHeapPrvGetFirstChunk();
	
	chk->size = UJ_HEAP_SZ - (chk->data - gHeap);
	chk->free = 1;
	chk->lock = 0;
	chk->wsze = 0;
}

#ifdef DEBUG_HEAP

	void ujHeapDebug(void){
	
		UjHeapHdr* hdr = (UjHeapHdr*)gHeap;
		UjHeapChunk* chk;
		SIZE* handleTable = (SIZE*)hdr->data;
		SIZE i;
		
		
		pr(stderr, " %u handles, data begins at 0x%08X (0x%08X)\n", hdr->numHandles, (UInt8*)(handleTable + hdr->numHandles) - hdr->data, (UInt8*)ujHeapPrvGetFirstChunk() - hdr->data);
		
		for(i = 0; i < hdr->numHandles; i++){
		
			if(handleTable[i]){
			
				pr(stderr, "HANDLE[%u] => 0x%08lX\n", i + 1, (unsigned long)handleTable[i]);
			}
		}
		chk = ujHeapPrvGetFirstChunk();
		
		while(chk){
		
			pr(stderr, "CHUNK @ 0x%08lX: datap = 0x%08lX, 0x%lX size (%d waste), %slocked, %sfree %u mark", (unsigned long)((UInt8*)chk - gHeap), (unsigned long)((UInt8*)chk->data - gHeap), (unsigned long)chk->size, chk->free ? 0 : chk->wsze, chk->lock ? "" : "un", chk->free ? "" : "not ", chk->mark);
			if(!chk->free){
			
				SIZE t;
				UInt8 b, v, x = 5;
				
				for(t = 0; t < chk->size - chk->wsze; t++){
					
					v = chk->data[t];
					
					for(b = 0; b < 8; b++, v <<= 1){
					
						x = (x << 1) ^ (((x & 0x80) == (v & 0x80)) ? 0xA2 : 0);
					}
				}
				
				pr(stderr, " crc: %02X", x);
			}
			pr(stderr, "\n");
		
			chk = ujHeapPrvGetNextChunk(chk);
		}
	}

#endif

static void ujHeapPrvFreeChunk(UjHeapChunk* chk, UjHeapChunk* prev){

	UjHeapChunk* n;
	UjHeapChunk* t = NULL;

	//step 1: debug checks
	
	if(chk->lock){
	
		pe(" FREEING a locked chunk\n");
	}
	if(chk->free){
		
		pe(" FREEING a free chunk\n");
	}
	
	
	//step 2: find previous chuunk (if not given to us)
	n = prev ? prev : ujHeapPrvGetPrevChunk(chk);
	
	//step 3: merge with previous chunk, if it is also free. use up previous chunk's wasted space if it's not free
	if(n){
		if(n->free){
		
			n->size += CHUNK_HDR_SZ + chk->size;
			chk = n;
		}
		else if(n->wsze){
		
			t = (UjHeapChunk*)(n->data + n->size - n->wsze);
			t->size = chk->size + n->wsze;
			n->size -= n->wsze;
			n->wsze = 0;
			chk = t;
		}
	}
	
	chk->free = 1;
	chk->lock = 0;
	chk->wsze = 0;
	
	//merge with next chunk if it is free
	n = ujHeapPrvGetNextChunk(chk);
	if(n && n->free){
		
		chk->size += CHUNK_HDR_SZ + n->size;
	}
}

static UjHeapChunk* ujHeapPrvAllocChunk(UInt16 sz){

	UInt8* ret;
	UjHeapChunk* chk = ujHeapPrvGetFirstChunk();
	UjHeapChunk* fit = NULL;

	sz = (sz + HEAP_ALIGN - 1) &~ (HEAP_ALIGN - 1);
	while(chk){
	
		if(chk->free && chk->size >= sz && (!fit || fit->size >= chk->size)) fit = chk;
		chk = ujHeapPrvGetNextChunk(chk);
	}
	
	//fit is now the chunk we'll use to back this allocation
	if(!fit) return NULL;
	
	if(fit->size - sz > CHUNK_HDR_SZ){	//splitting makes sense
	
		chk = fit;
		chk->size = fit->size - sz - CHUNK_HDR_SZ;
		chk->free = 1;
		chk->lock = 0;
		chk->wsze = 0;
		fit = ujHeapPrvGetNextChunk(chk);
		fit->size = sz;
		fit->wsze = 0;
	}
	else{							//we cnanot split, let's at least record wasted size for later use
	
		fit->wsze = fit->size - sz;
	}
	
	fit->free = 0;
	fit->lock = 0;
	
	ret = fit->data;
	while(sz--) *ret++ = 0;

	return fit;
}

//UjHeapChunk* ujHeapPrevGetPrevChunk(UjHeapChunk*)
//UjHeapChunk* ujHeapNextGetPrevChunk(UjHeapChunk*)


static void ujHeapPrvCompact(){

	UjHeapHdr* hdr = (UjHeapHdr*)gHeap;
	SIZE* handleTable = (SIZE*)hdr->data;
	UjHeapChunk *c, *p, *pp, *f, tmp;
	SIZE pos;
	HANDLE i;
	
	while(1){
	
		pp = p = f = NULL;
		
		// the last free chunk preceded but an unlocked non-free chunk (chunk before the chunk before free one is stored in f
		c = ujHeapPrvGetFirstChunk();
		while(c){
		
			if(p && c->free && !p->lock && !p->free && (!pp || !pp->lock)){
			
				//we do not bother moving in cases where a movable chunk immediately follows a non-movable chunk since that does us no good
				f = pp;
			}
			pp = p;
			p = c;
			c = ujHeapPrvGetNextChunk(c);
		}
		if(!f) break;
		
		pp = f;
		p = pp ? ujHeapPrvGetNextChunk(pp) : ujHeapPrvGetFirstChunk();
		c = ujHeapPrvGetNextChunk(p);
		f = (UjHeapChunk*)(((UInt8*)p) + CHUNK_HDR_SZ + c->size + p->wsze);
		
		//now: c is free chunk, p is unlocked nonfree hcunk before p, pp is chunk before p or NULL if p is first, f is where new p will be
		
		//now we copy data backwards to avoid overwrites
		pos = p->size - p->wsze;
		while(pos){
		
			pos--;
			f->data[pos] = p->data[pos];
		}
		
		//now copy chunk header (buffered locally to avoid compiler bugs on struct copy)
		tmp = *p;
		*f = tmp;
		
		//now fix up the header since we no longer have wasted space
		f->size -= f->wsze;
		
		//now update handle table
		pos = (UInt8*)p - gHeap;
		for(i = 0; i < hdr->numHandles; i++){
		
			if(handleTable[i] == pos){
				
				TL(" compact updating chunk %u from 0x%08X to 0x%08X\n", i + 1, handleTable[i], (UInt8*)f - gHeap);
				handleTable[i] = (UInt8*)f - gHeap;
				break;
			}
		}
		
		if(i == hdr->numHandles){
		
			pe("Handle repoint fail");
			//fprintf(stderr," for 0x%08X -> 0x%08X\n", (UInt8*)p - gHeap, (UInt8*)f - gHeap);	
		}
		
		//now create chunk before the new data chunk. note we do not mark it free!
		p->free = 0;
		p->lock = 0;
		p->wsze = 0;
		p->size = (f->data - p->data) - CHUNK_HDR_SZ;
		
		//now "free" this new chunk (effectively merge it with free space around, as needed)
		ujHeapPrvFreeChunk(p, pp);
	}
}

HANDLE ujHeapHandleNew(UInt16 sz){

	UjHeapHdr* hdr = (UjHeapHdr*)gHeap;
	SIZE* handleTable = (SIZE*)hdr->data;
	UjHeapChunk* chk = 0;
	HANDLE i;
	
	TL("Start allocating new handle with size %u\n", sz);

	for(i = 0; i < hdr->numHandles; i++){
		
		if(!handleTable[i]) break;
	}
	
	if(i != hdr->numHandles) chk = ujHeapPrvAllocChunk(sz);
	
	if(i == hdr->numHandles || !chk){	//no handles or no space
		
		ujHeapUnmarkAll();
		i = hdr->numHandles;
		ujGC();
		ujHeapFreeUnmarked();
		ujHeapPrvCompact();
		chk = ujHeapPrvAllocChunk(sz);
	}
	
	if(!chk){
		
		pe(" OUT OF MEMORY\n");
		return 0;
	}
	
	if(i == hdr->numHandles){
		for(i = 0; i < hdr->numHandles; i++){
		
			if(!handleTable[i]) break;
		}
	}
	
	if(i == hdr->numHandles){	//out of handles
	
		pe(" OUT OF HANDLES\n");
		ujHeapPrvFreeChunk(chk, NULL);
		return 0;
	}
	
	//pr(stderr, "HEAP: handle.new (%d) sz=%d\n", i + 1, sz);
	
	handleTable[i] = (UInt8*)chk - gHeap;

	TL("Done allocating new handle with size %u -> (%d, 0x%08X)\n", sz, i + 1, handleTable[i]);

	return i + 1;
}

/*
	it is a very bad idea to call this anytime once vm is running, since it will fragment heap.
	calling it before vm runs is ok, since it will allocate all at end safely.
*/
void* ujHeapAllocNonmovable(UInt16 sz){

	HANDLE h = ujHeapHandleNew(sz);		//use this to allocae since it will triger GC as needed
	UjHeapHdr* hdr = (UjHeapHdr*)gHeap;
	SIZE* handleTable = (SIZE*)hdr->data;
	UjHeapChunk* chk;

	if(!h) return NULL;
	
	chk = (UjHeapChunk*)(gHeap + handleTable[h - 1]);	//free handle table slot since this isnt a real handle
	
	handleTable[h - 1] = 0;
	
	chk->lock = 1;
	return chk->data;
}

void ujHeapHandleFree(HANDLE handle){

	UjHeapHdr* hdr = (UjHeapHdr*)gHeap;
	SIZE* handleTable = (SIZE*)hdr->data;
	UjHeapChunk* chk = (UjHeapChunk*)(gHeap + handleTable[handle - 1]);
	
	TL("Free handle %u\n", handle);

	if(!handle){

		pe("Freeing a NULL handle\n");
	}

	if(!handleTable[handle - 1]){   

                pe("Freeing nonexistent chunk\n");
        }

	//pr(stderr, "HEAP: handle.free (%d)\n", handle);
	
	handleTable[handle - 1] = 0;
	
	ujHeapPrvFreeChunk(chk, NULL);
}

void* ujHeapHandleLock(HANDLE handle){

	UjHeapHdr* hdr = (UjHeapHdr*)gHeap;
	SIZE* handleTable = (SIZE*)hdr->data;
	UjHeapChunk* chk = (UjHeapChunk*)(gHeap + handleTable[handle - 1]);
		
	//pr(stderr, "HEAP: handle.lock (%d)\n", handle);

        if(!handle){

                pe("Locking a NULL handle\n");
        }

	if(!handleTable[handle - 1]){

		pe("Locking nonexistent chunk\n");
	}
	if(chk->lock){
		
		pe("Locking already locked chunk\n");
	}
	if(chk->free){
		
		pe("Locking free chunk\n");
	}
	
	chk->lock = 1;
	
	TL("Do lock handle %d -> 0x%08X (0x%08X)\n", handle, chk->data - gHeap, chk->data);

	return chk->data;
}

void* ujHeapHandleIsLocked(HANDLE handle){

	UjHeapHdr* hdr = (UjHeapHdr*)gHeap;
	SIZE* handleTable = (SIZE*)hdr->data;
	UjHeapChunk* chk = (UjHeapChunk*)(gHeap + handleTable[handle - 1]);

        if(!handle){

                pe("TryLocking a NULL handle\n");
        }

	TL("Try lock handle %d -> 0x%08X (0x%08X)\n", handle, (chk->lock ? chk->data - gHeap: NULL), chk->lock ? chk->data : NULL);
	
	return chk->lock ? chk->data : NULL;
}


void ujHeapHandleRelease(HANDLE handle){

	UjHeapHdr* hdr = (UjHeapHdr*)gHeap;
	SIZE* handleTable = (SIZE*)hdr->data;
	UjHeapChunk* chk = (UjHeapChunk*)(gHeap + handleTable[handle - 1]);
		
	//pr(stderr, "HEAP: handle.rel (%d)\n", handle);

	TL("Relese handle %d\n", handle);

        if(!handle){

                pe("Releasing a NULL handle\n");
        }

	if(!handleTable[handle - 1]){   

                pe("Releasing nonexistent chunk\n");
        }

	if(!chk->lock){
		
		pe("Ulocking unlocked chunk\n");
	}
	if(chk->free){
		
		pe("Unlocking free chunk\n");
	}
	
	chk->lock = 0;
}

void ujHeapUnmarkAll(void){
	
	UjHeapChunk* chk = ujHeapPrvGetFirstChunk();
	
	while(chk){
	
		chk->mark = 0;
		chk = ujHeapPrvGetNextChunk(chk);
	}
}

void ujHeapFreeUnmarked(void){
	
	UjHeapHdr* hdr = (UjHeapHdr*)gHeap;
	SIZE* handleTable = (SIZE*)hdr->data;
	HANDLE i;
#ifdef DEBUG_HEAP
	HANDLE sNumFreed = 0;
	SIZE sBytesFreed = 0;
#endif

	//pr(stderr, "HEAP: GC.start\n");
	
	for(i = 0; i < hdr->numHandles; i++){
	
		if(handleTable[i]){
	
			TL("GC frees handle %d\n", i + 1);
	
			UjHeapChunk* chk = (UjHeapChunk*)(gHeap + handleTable[i]);
			
			if(!chk->mark && !chk->lock){
#ifdef DEBUG_HEAP
				sNumFreed++;
				sBytesFreed += chk->size;
#endif
				ujHeapHandleFree(i + 1);	
			}
		} 
	}
	
	//pr(stderr, "HEAP: GC.stop\n");
#ifdef DEBUG_HEAP

	pr(stderr, "GC collected %u bytes over %d chunks\n", (unsigned)sBytesFreed, sNumFreed);
#endif
}

HANDLE ujHeapFirstMarked(UInt8 markVal){

	UjHeapHdr* hdr = (UjHeapHdr*)gHeap;
	SIZE* handleTable = (SIZE*)hdr->data;
	HANDLE t;
	
	
	for(t = 0; t < hdr->numHandles; t++){
	
		if(handleTable[t] && ((UjHeapChunk*)(gHeap + handleTable[t]))->mark == markVal) return t + 1;
	}
	return 0;
}


void ujHeapMark(HANDLE handle, UInt8 mark){

	UjHeapHdr* hdr = (UjHeapHdr*)gHeap;
	SIZE* handleTable = (SIZE*)hdr->data;
	UjHeapChunk* chk = (UjHeapChunk*)(gHeap + handleTable[handle - 1]);

        if(!handle){

                pe("Marking a NULL handle\n");
        }


	TL(" marking handle %u to level %u\n", handle, mark);
	
	if(chk->mark < mark) chk->mark = mark;
}

UInt8 ujHeapGetMark(HANDLE handle){

	UjHeapHdr* hdr = (UjHeapHdr*)gHeap;
	SIZE* handleTable = (SIZE*)hdr->data;
	
	return ((UjHeapChunk*)(gHeap + handleTable[handle - 1]))->mark;
}

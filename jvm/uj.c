#include "uj.h"
#include "ujHeap.h"
#include "UJC.h"
#include "common.h"

#ifdef UJ_DBG_HELPERS
	#include <stdio.h>
#endif


#if defined(UJ_FTR_SUPPORT_LONG) || defined(UJ_FTR_SUPPORT_DOUBLE)
	#include "long64.h"
#endif

#if defined(UJ_FTR_SUPPORT_DOUBLE)
	#include "double64.h"
#endif

#if defined(UJ_FTR_SUPPORT_FLOAT) || defined(UJ_FTR_SUPPORT_DOUBLE)
	#include <math.h>
	#ifdef MICROCHIP

	__attribute__((__const__)) static char isNaN (const float* f) 
	{
		const int* rep = ((const int*) f) + 1;
		return ((*rep & 0x7F00) == 0x7F00);
	}
	#else
		double floor(double);
		#define floorf floor
	#endif
#endif



/*
	TODO:
		
		* throw real exceptions for VM things like OOM & out of stack
		* Heap compaction (1/2 done?)
		* Heap handle table growth (is this needed?)
		* constant initializer support (is this needed with latest JDK [6+] ?)
		* in class file store a UInt8 hash of name, to speed up findClass (mash searched name, only do string compare if hashes match)
		* keep array types so as to support appropriate throwing of "ArrayStoreException"
*/


//java stuff

#define FLAG_DONT_SEARCH_SUBCLASSES	0x8000	//our flag


//types of constants
#define JAVA_CONST_TYPE_STRING		1	//2 bytes + string
#define JAVA_CONST_TYPE_INT			3	//4 bytes
#define JAVA_CONST_TYPE_FLOAT		4	//4 bytes
#define JAVA_CONST_TYPE_LONG		5	//8 bytes
#define JAVA_CONST_TYPE_DOUBLE		6	//8 bytes
#define JAVA_CONST_TYPE_CLASS		7	//2 bytes (index back into constant pool pointing to class name string)
#define JAVA_CONST_TYPE_STR_REF		8	//2 bytes (index back into constant pool pointing to string)
#define JAVA_CONST_TYPE_FIELD		9	//4 bytes (2x index back into constant pool: class & name&type info)
#define JAVA_CONST_TYPE_METHOD		10	//4 bytes (2x index back into constant pool: class & name&type info)
#define JAVA_CONST_TYPE_INTERFACE	11	//4 bytes (2x index back into constant pool: class & name&type info) (interfce METHOD)
#define JAVA_CONST_TYPE_NAME_TYPE_INFO	12	//4 bytes (2x index back into constant pool: method name string & type info string)


//types for newarray
#define JAVA_ATYPE_FIRST	4	//first atype we have
#define JAVA_ATYPE_BOOL		4
#define JAVA_ATYPE_CHAR		5
#define JAVA_ATYPE_FLOAT	6
#define JAVA_ATYPE_DOUBLE	7
#define JAVA_ATYPE_BYTE		8
#define JAVA_ATYPE_SHORT	9
#define JAVA_ATYPE_INT		10
#define JAVA_ATYPE_LONG		11
#define JAVA_ATYPE_LAST		11


//types for string-based descriptors
#define JAVA_TYPE_BYTE		'B'
#define JAVA_TYPE_CHAR		'C'
#define JAVA_TYPE_DOUBLE	'D'
#define JAVA_TYPE_FLOAT		'F'
#define JAVA_TYPE_INT		'I'
#define JAVA_TYPE_LONG		'J'
#define JAVA_TYPE_SHORT		'S'
#define JAVA_TYPE_BOOL		'Z'
#define JAVA_TYPE_ARRAY		'['
#define JAVA_TYPE_OBJ		'L'
#define JAVA_TYPE_OBJ_END	';'




//our stuff

#define THREAD_RET_INFO_SZ	3	//in units of stack slots

//flags for {get,put}{statis,field}
#define UJ_ACCESS_PUT		1	//these are not random and canot be changed (see instr decoding)
#define UJ_ACCESS_FIELD		2


//method invocation type
#define UJ_INVOKE_VIRTUAL	0
#define UJ_INVOKE_SPECIAL	1
#define UJ_INVOKE_STATIC	2
#define UJ_INVOKE_INTERFACE	3

//special PC values
#define UJ_PC_DONE	0xFFFFFFFFUL	//we fell off the end of the code
#define UJ_PC_BAD	0xFFFFFFFEUL	//invalid PC


static const UInt8 ujPrvAtypeToStrType[] = {JAVA_TYPE_BOOL, JAVA_TYPE_CHAR, JAVA_TYPE_FLOAT,
						JAVA_TYPE_DOUBLE, JAVA_TYPE_BYTE, JAVA_TYPE_SHORT,
						JAVA_TYPE_INT, JAVA_TYPE_LONG};

typedef struct{

	HANDLE holder;
	UInt16 numHolds;

}UjMonitor;

typedef struct UjClass{

	struct UjClass* nextClass;
	
	#ifdef UJ_FTR_SYNCHRONIZATION
		UjMonitor mon;
	#endif
	
	UInt8 reserved	: 4;
	UInt8 native	: 1;
	UInt8 ujc	: 1;
	UInt8 mark	: 2;

#ifdef UJ_OPT_CLASS_SEARCH
	UInt8 clsNameHash;
#endif

	union{
	
		struct{
			
			UInt32 readD;
			UInt24 interfaces;
			UInt24 fields;
			UInt24 methods;
			
		}java;
		
		const UjNativeClass* native;
		
	}info;
	
	struct UjClass* supr;

	UInt16 instDataOfst;		//offset in instance data to class's instance data (before it comes data from superclasses)
	UInt16 instDataSize;		//size of class's instance data (before it comes data from superclasses) in instance data
	
	UInt16 clsDataOfst;		//offset in class data to class's data (before it comes data from superclasses)
	UInt16 clsDataSize;		//size in class data of class's data (before it comes data from superclasses)
	
	UInt8 data[];
}UjClass;

typedef struct UjInstance{	//must begin with UjClass*

	UjClass* cls;
	
	#ifdef UJ_FTR_SYNCHRONIZATION
		UjMonitor mon;
	#endif
	
	UInt8 data[];			//instance data ( array of elements such as: {u8 type, u8 data[]} )

}UjInstance;

//if a chunk in heap is not an object, its "cls" field is NULL, and the next 8 bits explain what it is
#define OBJ_TYPE_ARRAY		0	//array  of something other then objects
#define OBJ_TYPE_OBJ_ARRAY	1	//array of objects/arrays

typedef struct UjArray{		//must begin with UjClass*

	UjClass* cls;	//NULL for arrays. all objects in the heap must have this as the first field!
	UInt8 objType;
	UInt24 length;
	UInt8 data[];
	
}UjArray;

typedef struct UjThread{

	HANDLE nextThread;

	union{
		struct{
			UInt8 hasInst	: 1;	//same as !!instH
			UInt8 syncronized: 1;
		}access;
		UInt8 raw;
	}flags;
	
	UjClass* cls;
	HANDLE instH;
	
	UInt24 pc;		//direct offset into class file!
	
	//these next 3 fields do not need to be pushed on stack since we can get them again from class file by method index :)
	UInt24 methodStartPc;	//direct offset into class file!

	UInt16 spBase;		//we use an empty ascending stack
	UInt16 spLimit;	//also used for "isPtr"
	UInt16 localsBase;
	UInt32 stack[];

}UjThread;


#define STR_EQ_PAR_TYPE_PTR	0	//len, const char*
#define STR_EQ_PAR_TYPE_REF2	1	//@ (t->cls).const(@((t->cls).const(idx) + offset))
#define STR_EQ_PAR_TYPE_REF	2	//@ cls.const(@(cls.const(idx) + offset))
#define STR_EQ_PAR_TYPE_IDX	3	//@ cls.const(strIdx)
#define STR_EQ_PAR_TYPE_ADR	4	//readD @ addr
#define STR_EQ_PAR_TYPE_UJC	5	//cls, (JavaString*)( (*(UInt24*)addr) + 2)

typedef struct{
	
	UInt8 type;
	
	union{
	
		struct{
			UInt16 len;
			const char* str;
		}ptr;
		
		struct{
			UjClass* cls;
			UInt16 strIdx;
		}idx;
		
		struct{
		
			UInt32 readD;
			UInt24 addr;
			
		}adr;
		
		struct{
		
			union{
				UjClass* cls;
				UjThread* t;
			
			}from;
			UInt16 idx;
			UInt8 offset;	//second offset for double deref
		
		}ref;
		
		struct{
		
			UjClass* cls;
			UInt24 addr;
		}ujc;
		
	}data;
	
}UjPrvStrEqualParam;


/************************ START GLOBALS *******************************/

static UjClass* gFirstClass;
static HANDLE gCurThread;
static HANDLE gFirstThread;
static UInt32 gNumInstrs;

/************************ END  GLOBALS *******************************/




static UInt16 ujCstrlen(const char* s){

	UInt16 len = 0;
	
	while(*s++) len++;
	
	return len;	
}

static UInt8 ujThreadPrvRet(UjThread* t, HANDLE threadH);
static UInt8 ujInitBuiltinClasses(UjClass** objectClassP);

static Int32 ujThreadReadBE32_ex(UInt32 readD, UInt24 addr){	//from class file

	Int32 i32 = 0;
	UInt8 t8;
	
	for(t8 = 0; t8 < 4; t8++) i32 = (i32 << 8) | ujReadClassByte(readD, addr++);
	
	return i32;
}

static UInt24 ujThreadReadBE24_ex(UInt32 readD, UInt24 addr){	//from class file

	UInt24 i24 = 0;
	UInt8 t8;
	
	for(t8 = 0; t8 < 3; t8++) i24 = (i24 << 8) | ujReadClassByte(readD, addr++);
	
	return i24;
}

static Int16 ujThreadReadBE16_ex(UInt32 readD, UInt24 addr){	//from class file

	Int16 i16 = 0;
	
	i16 = ujReadClassByte(readD, addr++);
	i16 <<= 8;
	i16 |= ujReadClassByte(readD, addr);
	
	return i16;
}

static _INLINE_ Int32 ujThreadReadBE32(UjThread* t, UInt24 addr){	//from class file

	return ujThreadReadBE32_ex(t->cls->info.java.readD, addr);
}

static _INLINE_ UInt24 ujThreadReadBE24(UjThread* t, UInt24 addr){	//from class file

	return ujThreadReadBE24_ex(t->cls->info.java.readD, addr);
}

static _INLINE_ Int16 ujThreadReadBE16(UjThread* t, UInt24 addr){	//from class file

	return ujThreadReadBE16_ex(t->cls->info.java.readD, addr);
}

static _INLINE_ UInt8 ujThreadPrvFetchClassByte(UjThread* t, UInt24 addr){

	return 	ujReadClassByte(t->cls->info.java.readD, addr);
}

#ifdef UJ_FTR_SUPPORT_CLASS_FORMAT
	static UInt24 ujPrvSkipAttribute(UInt32 readD, UInt24 addr){
	
		return addr + 6 + ujThreadReadBE32_ex(readD, addr + 2);
	}
#endif

static Boolean ujThreadPrvMonEnter(HANDLE h, UjMonitor* mon){

	if(mon->numHolds && (mon->holder != h)) return false;
	mon->numHolds++;
	mon->holder = h;
	return true;
}

static Boolean ujThreadPrvMonExit(HANDLE h, UjMonitor* mon){

	if(mon->numHolds && (mon->holder == h)){
		
		mon->numHolds--;
		return UJ_ERR_NONE;
	}
	return UJ_ERR_MON_STATE_ERR;
}

#ifdef UJ_FTR_SUPPORT_CLASS_FORMAT
	static UInt24 ujThreadPrvFindConst_ex_class(UInt32 readD, UInt16 idx){
	
		UInt8 type;
		UInt24 addr = 10;
		
		while(--idx){
			
			type = ujReadClassByte(readD, addr++);
			switch(type){
			
				case JAVA_CONST_TYPE_STRING:
					
					addr += 2 + ujThreadReadBE16_ex(readD, addr);
					break;
				
				case JAVA_CONST_TYPE_INT:
				case JAVA_CONST_TYPE_FLOAT:
				case JAVA_CONST_TYPE_FIELD:
				case JAVA_CONST_TYPE_METHOD:
				case JAVA_CONST_TYPE_INTERFACE:
				case JAVA_CONST_TYPE_NAME_TYPE_INFO:
				default:	//XXX: is this good?
					
					addr += 4;
					break;
				
				case JAVA_CONST_TYPE_LONG:
				case JAVA_CONST_TYPE_DOUBLE:
					
					addr += 8;
					idx--;
					break;
				
				case JAVA_CONST_TYPE_CLASS:
				case JAVA_CONST_TYPE_STR_REF:
					
					addr += 2;
					break;
			}
		}
		
		return addr;
	}
#endif

#ifdef UJ_FTR_SUPPORT_UJC_FORMAT
	static UInt24 ujThreadPrvFindConst_ex_ujc(UInt32 readD, UInt16 idx){
	
		UInt24 addr = 18 + 2 + (((UInt24)(idx - 1)) << 1) + (idx - 1);
		
		addr = ujThreadReadBE24_ex(readD, addr);	//read constant location
		
		#ifdef UJ_DBG_HELPERS
	
			if(addr == 0){
			
				fprintf(stderr,"ujc using zero const %d\n", idx);	
			}
		#endif
		
		return addr;
	}
#endif

static UInt24 ujThreadPrvFindConst_ex(UjClass* cls, UInt16 idx){

	if(cls->ujc){
		
		#ifdef UJ_FTR_SUPPORT_UJC_FORMAT
			
			return ujThreadPrvFindConst_ex_ujc(cls->info.java.readD, idx);
		#endif
	}
	else{
		#ifdef UJ_FTR_SUPPORT_CLASS_FORMAT
			
			return ujThreadPrvFindConst_ex_class(cls->info.java.readD, idx);
		#endif
	}
	return 0;
}

#define ujThreadPrvFindConst(t, idx)	ujThreadPrvFindConst_ex(t->cls, idx)

static UInt8 ujThreadPrvStrEqualGetChar(UjPrvStrEqualParam* p, UInt16 idx){	//get n-th char

	return (p->type == STR_EQ_PAR_TYPE_PTR) ? p->data.ptr.str[idx] : ujReadClassByte(p->data.adr.readD, p->data.adr.addr + 2 + idx);
}

static UInt16 ujThreadPrvStrEqualGetLen(UjPrvStrEqualParam* p){			//get length

	return (p->type == STR_EQ_PAR_TYPE_PTR) ? p->data.ptr.len : (UInt16)ujThreadReadBE16_ex(p->data.adr.readD, p->data.adr.addr);
}

static void ujThreadPrvStrEqualProcessParam(UjPrvStrEqualParam* p){		//process to one of two types we support

	UInt24 addr;

	switch(p->type){
		
		case STR_EQ_PAR_TYPE_PTR:
			
			break;
		
		case STR_EQ_PAR_TYPE_REF2:
		
			p->data.ref.from.cls = p->data.ref.from.t->cls;
			p->type = STR_EQ_PAR_TYPE_REF;
			//fallthrough intended
			
		case STR_EQ_PAR_TYPE_REF:
		
			addr = ujThreadPrvFindConst_ex(p->data.ref.from.cls, p->data.ref.idx);
			addr = ujThreadReadBE16_ex(p->data.ref.from.cls->info.java.readD, addr + p->data.ref.offset);
		
			p->data.idx.cls = p->data.ref.from.cls;
			p->data.idx.strIdx = addr;
			p->type = STR_EQ_PAR_TYPE_IDX;
			//fallthrough intentional	
			
		case STR_EQ_PAR_TYPE_IDX:
			
			addr = ujThreadPrvFindConst_ex(p->data.idx.cls, p->data.idx.strIdx) + 1;
			p->data.adr.readD = p->data.idx.cls->info.java.readD;
	addr_ready:
			p->data.adr.addr = addr;
			p->type = STR_EQ_PAR_TYPE_ADR;
			//fallthrough intended
			
		case STR_EQ_PAR_TYPE_ADR:
		
			break;

#ifdef UJ_FTR_SUPPORT_UJC_FORMAT		
		case STR_EQ_PAR_TYPE_UJC:
			
			addr = ujThreadReadBE24_ex(p->data.ujc.cls->info.java.readD, p->data.ujc.addr) + 1;
			p->data.adr.readD = p->data.ujc.cls->info.java.readD;
			goto addr_ready;
#endif

	}
}

#ifdef UJ_OPT_CLASS_SEARCH
	static UInt8 ujPrvHashString(UjPrvStrEqualParam* p){

		UInt16 L;
		UInt8 c = 0xCC;

		ujThreadPrvStrEqualProcessParam(p);
		L = ujThreadPrvStrEqualGetLen(p);
	

		while(L){

			L--;
			c = (c << 1) ^ ((c & 0x80) ? 0x41 : 0x00) ^ ujThreadPrvStrEqualGetChar(p, L);
		}

		return c;
	}
#endif

static Boolean ujThreadPrvStrEqualEx(UjPrvStrEqualParam* p1, UjPrvStrEqualParam* p2){

	UInt16 L;
	
	ujThreadPrvStrEqualProcessParam(p1);
	ujThreadPrvStrEqualProcessParam(p2);
	
	L = ujThreadPrvStrEqualGetLen(p1);
	if(ujThreadPrvStrEqualGetLen(p2) != L) return false;	//non-matching lengths indicate non-equal strings
	
	//we match in reverse, since it's easier. shouldn't mater anyways
	while(L){
	
		L--;
		if(ujThreadPrvStrEqualGetChar(p1, L) != ujThreadPrvStrEqualGetChar(p2, L)) return false;	//non-matching chars
	}
	
	//they match
	return true;
}

static Boolean ujThreadPrvStrEqual(UjClass* cls, UInt16 strIdx, UjPrvStrEqualParam* p2){

	UjPrvStrEqualParam p1;
	
	p1.type = STR_EQ_PAR_TYPE_IDX;
	p1.data.idx.cls = cls;
	p1.data.idx.strIdx = strIdx;
	
	return ujThreadPrvStrEqualEx(&p1, p2);
}

static UjClass* ujThreadPrvFindClass(UjPrvStrEqualParam* name){
	
	UjPrvStrEqualParam p;
	UjClass* cls = gFirstClass;
#ifdef UJ_OPT_CLASS_SEARCH
	UInt8 nameCrc = ujPrvHashString(name);
#endif
	
	while(cls){
	
	#ifdef UJ_OPT_CLASS_SEARCH
		if(nameCrc == cls->clsNameHash){	//check hash if we have it
	#else
		if(1){					//always check name if we have no hash
	#endif

			if(cls->native){	//native class
		
				p.type = STR_EQ_PAR_TYPE_PTR;
				p.data.ptr.len = ujCstrlen(p.data.ptr.str = cls->info.native->clsName);
			}
			else if(cls->ujc){	//UJC

			#ifdef UJ_FTR_SUPPORT_UJC_FORMAT
				p.type = STR_EQ_PAR_TYPE_IDX;
				p.data.idx.cls = cls;
				p.data.idx.strIdx = ujThreadReadBE16_ex(cls->info.java.readD, 2);
			#endif
			
			}
			else{			//java class

			#ifdef UJ_FTR_SUPPORT_CLASS_FORMAT
				p.type = STR_EQ_PAR_TYPE_REF;
				p.data.ref.from.cls = cls;
				p.data.ref.idx = ujThreadReadBE16_ex(cls->info.java.readD, cls->info.java.interfaces - 6);
				p.data.ref.offset = 1;
			#endif
			}
		
			if(ujThreadPrvStrEqualEx(&p, name)) return cls;
		}

		cls = cls->nextClass;
	}
	
	return NULL;
}

UInt8 ujRegisterNativeClass(const UjNativeClass* nCls, struct UjClass* super, struct UjClass** clsP){

#ifdef UJ_OPT_CLASS_SEARCH
	UjPrvStrEqualParam p;
#endif
	UjClass* cls;

	cls = ujHeapAllocNonmovable(sizeof(UjClass) + (super ? super->clsDataOfst : 0) + nCls->clsDatSz);
	if(!cls) return UJ_ERR_OUT_OF_MEMORY;

#ifdef UJ_OPT_CLASS_SEARCH
	p.type = STR_EQ_PAR_TYPE_PTR;
	p.data.ptr.len = ujCstrlen(p.data.ptr.str = nCls->clsName);
	cls->clsNameHash = ujPrvHashString(&p);
#endif

	cls->supr = super;
	
	if(super){
		
		cls->instDataOfst = super->instDataOfst + super->instDataSize;
		cls->clsDataOfst = super->clsDataOfst + super->clsDataSize;
	}
	
	cls->instDataSize = nCls->instDatSz;
	cls->clsDataSize = nCls->clsDatSz;
	cls->native = 1;
	
	cls->info.native = nCls;
	
	if(clsP) *clsP = cls;
	
	cls->nextClass = gFirstClass;
	gFirstClass = cls;
	
	return UJ_ERR_NONE;
}

static UInt8 ujPrvJavaTypeToSize(char type){

	switch(type){
		
		case JAVA_TYPE_BYTE:
		case JAVA_TYPE_BOOL:
			
			return 1;
		
		case JAVA_TYPE_CHAR:
		case JAVA_TYPE_SHORT:
			
			return 2;
		
		case JAVA_TYPE_DOUBLE:
		case JAVA_TYPE_LONG:
			
			return 8;
		
		default:
			
			return 4;
	}
}

UInt8 ujLoadClass(UInt32 readD, UjClass** clsP){
	
	UjClass* cls;
	UjClass* supr = NULL;
	UjPrvStrEqualParam p;
	UInt16 n, numInterfaces, clsDatSz = 0, instDatSz = 0;
	UInt24 addr, fields, interfaces;
	Boolean isUjc = false;
	Boolean isClassVar;
	UInt8 type;
	#ifdef UJ_OPT_CLASS_SEARCH
		UInt8 clsNameHash;
	#endif
	
	if(ujThreadReadBE16_ex(readD, 0) == UJC_MAGIC){			//UJC file

	#ifdef UJ_FTR_SUPPORT_UJC_FORMAT
			
		//p.data.adr.addr has addr of superclass name
		n = ujThreadReadBE16_ex(readD, 4);
		if(n){
			
			p.data.adr.addr = ujThreadPrvFindConst_ex_ujc(readD, ujThreadReadBE16_ex(readD, 4));
			supr =(void*)1;
		}
		
		//"interfaces" points to the interfaces list
		interfaces = ujThreadReadBE24_ex(readD, 8);
		
		//"fields" points to fields list
		fields = ujThreadReadBE24_ex(readD, 14);
		
		#ifdef UJ_OPT_CLASS_SEARCH
			
			//"clsNameHash" has name hash
			clsNameHash = ujReadClassByte(readD, 17);
			
		#endif
		
		isUjc = true;
		
		addr = fields;
		n = ujThreadReadBE16_ex(readD, fields - 2);
		//skip fields
		while(n--){
			
			isClassVar = !!(ujThreadReadBE16_ex(readD, addr) & JAVA_ACC_STATIC);		//check flags
			
			type = ujReadClassByte(readD, ujThreadReadBE24_ex(readD, addr + 7) + 3);	//get type descriptor first character
	
			type = ujPrvJavaTypeToSize(type);
			
			if(isClassVar) clsDatSz += type;
			else instDatSz += type;		
			
			addr += 10;
		}
		
		//"addr" points to methods list
		addr = ujThreadReadBE24_ex(readD, 11);
	#else
		return UJ_ERR_INTERNAL;
	#endif
	}
	else if((UInt32)ujThreadReadBE32_ex(readD, 0) == 0xCAFEBABE){	//class file
	
	#ifdef UJ_FTR_SUPPORT_CLASS_FORMAT
	
		UInt16 t;
		
		addr = 10;
		
		//first we skip the constants
		t = ujThreadReadBE16_ex(readD, 8) - 1;	//get number of constant pool entries
		while(t--){				//skip the constants
		
			type = ujReadClassByte(readD, addr++);
			switch(type){
			
				case JAVA_CONST_TYPE_STRING:
					
					addr += 2 + ujThreadReadBE16_ex(readD, addr);
					break;
				
				case JAVA_CONST_TYPE_INT:
				case JAVA_CONST_TYPE_FLOAT:
				case JAVA_CONST_TYPE_FIELD:
				case JAVA_CONST_TYPE_METHOD:
				case JAVA_CONST_TYPE_INTERFACE:
				case JAVA_CONST_TYPE_NAME_TYPE_INFO:
					
					addr += 4;
					break;
				
				case JAVA_CONST_TYPE_CLASS:
				case JAVA_CONST_TYPE_STR_REF:
					
					addr += 2;
					break;
				
				case JAVA_CONST_TYPE_LONG:
				case JAVA_CONST_TYPE_DOUBLE:
					
					addr += 8;
					t--;
					break;
			}
		}
		
		numInterfaces = ujThreadReadBE16_ex(readD, addr + 6);
		addr += 8;
		interfaces = addr;
		addr += ((UInt24)numInterfaces) << 1;	//skip interfaces
		
		t = ujThreadReadBE16_ex(readD, addr);
		addr += 2;
		//skip fields
		while(t--){
			
			n = ujThreadReadBE16_ex(readD, addr);		//get flags
			isClassVar = !!(n & JAVA_ACC_STATIC);
			
			n = ujThreadReadBE16_ex(readD, addr + 4);				//read type destriptor index
			type = ujReadClassByte(readD, ujThreadPrvFindConst_ex_class(readD, n) + 3);	//get type descriptor first character
	
			type = ujPrvJavaTypeToSize(type);
			
			if(isClassVar) clsDatSz += type;
			else instDatSz += type;		
			
			n = ujThreadReadBE16_ex(readD, addr + 6);
			addr += 8;
			while(n--) addr = ujPrvSkipAttribute(readD, addr);
		}
		addr += 2;	//now points to methods
		
		n = ujThreadReadBE16_ex(readD, interfaces - 4);					//super class index
		if(n){
			n = ujThreadReadBE16_ex(readD, ujThreadPrvFindConst_ex_class(readD, n) + 1);	//super class name index
			supr =(void*)1;
		}
		
		#ifdef UJ_OPT_CLASS_SEARCH
	
			p.type = STR_EQ_PAR_TYPE_ADR;
			p.data.adr.readD = readD;
			
			p.data.adr.addr = ujThreadPrvFindConst_ex_class(readD, ujThreadReadBE16_ex(readD, ujThreadPrvFindConst_ex_class(readD, ujThreadReadBE16_ex(readD, interfaces - 6)) + 1)) + 1;
			clsNameHash = ujPrvHashString(&p);
		#endif	
		
		fields = interfaces + (((UInt24)numInterfaces) << 1) + 2;
		
		//p.data.adr.addr has addr of superclass name
		if(supr) p.data.adr.addr = ujThreadPrvFindConst_ex_class(readD, n);
	#else
		return UJ_ERR_INTERNAL;
	#endif
	}
	else return UJ_ERR_INTERNAL;
	
	p.type = STR_EQ_PAR_TYPE_ADR;
	p.data.adr.readD = readD;
	p.data.adr.addr++;
	
	if(supr){	//Object has a superclass?
		
		supr = ujThreadPrvFindClass(&p);
		if(!supr) return UJ_ERR_DEPENDENCY_MISSING;
	}
	
	//now we have enough data to know this class's size -> alloc it
	
	cls = ujHeapAllocNonmovable(sizeof(UjClass) + clsDatSz + (supr ? supr->clsDataOfst + supr->clsDataSize : 0));
	if(!cls) return UJ_ERR_OUT_OF_MEMORY;
	
	cls->native = 0;
	cls->info.java.readD = readD;
	cls->supr = supr;
	cls->ujc = isUjc;
	
	if(supr){
		
		cls->instDataOfst = supr->instDataOfst + supr->instDataSize;
		cls->clsDataOfst = supr->clsDataOfst + supr->clsDataSize;
	}
	else{
	
		cls->instDataOfst = 0;
		cls->clsDataOfst = 0;
	}
	
	cls->info.java.interfaces = interfaces;
	cls->info.java.fields = fields;
	cls->info.java.methods = addr;
	cls->clsDataSize = clsDatSz;
	cls->instDataSize = instDatSz;
	
	#ifdef UJ_OPT_CLASS_SEARCH
		cls->clsNameHash = clsNameHash;
	#endif


	//TODO: setup static const variables. See fields' "ConstantValue" attribute: http://java.sun.com/docs/books/jvms/second_edition/html/ClassFile.doc.html#1405
	
	
	cls->nextClass = gFirstClass;
	gFirstClass = cls;
	if(clsP) *clsP = cls;
	
	

	return UJ_ERR_NONE;
}

HANDLE ujThreadCreate(UInt16 stackSz){

	HANDLE handle;
	UjThread* t;
	
	if(!stackSz) stackSz = UJ_DEFAULT_STACK_SIZE;
	handle = ujHeapHandleNew(sizeof(UjThread) + stackSz + ((stackSz / sizeof(UInt32)) + 7) / 8);
	if(!handle) return 0;
	
	t = ujHeapHandleLock(handle);
	
	t->spBase = 0;
	t->localsBase = 0;
	t->spLimit = stackSz / sizeof(UInt32);
	t->pc = UJ_PC_BAD;
	
	if(!gFirstThread) gCurThread = handle;
	t->nextThread = gFirstThread;
	gFirstThread = handle;
	
	ujHeapHandleRelease(handle);
	
	return handle;
}

static UInt24 ujThreadPrvGetMethodAddr(UjClass** clsP, UjPrvStrEqualParam* name, UjPrvStrEqualParam* type, UInt16 flagsAnd, UInt16 flagsEqEx, UInt16* flagsP){	//return offset into class file where code_attribute begins

	UInt24 addr;
	UjClass* cls = *clsP;
	UjPrvStrEqualParam p;
	UInt16 n, flags, flagsEq = flagsEqEx &~ FLAG_DONT_SEARCH_SUBCLASSES;

	while(cls){
		
		if(cls->native){	//native class
		
			for(n = 0; n < cls->info.native->numMethods; n++){
			
				p.type = STR_EQ_PAR_TYPE_PTR;
				p.data.ptr.len = ujCstrlen(p.data.ptr.str = cls->info.native->methods[n].name);
				if(!ujThreadPrvStrEqualEx(&p, name)) continue;
				
				p.type = STR_EQ_PAR_TYPE_PTR;
				p.data.ptr.len = ujCstrlen(p.data.ptr.str = cls->info.native->methods[n].type);
				if(!ujThreadPrvStrEqualEx(&p, type)) continue;
				
				if(((flags = cls->info.native->methods[n].flags) & flagsAnd) != flagsEq) continue;
				if(flagsP) *flagsP = flags;
				
				*clsP = cls;
				return n;
			}
		}
		else if(cls->ujc){	//UJC
		
		#ifdef UJ_FTR_SUPPORT_UJC_FORMAT
		
			#ifdef UJ_OPT_CLASS_SEARCH
			
				UInt8 nHash = ujPrvHashString(name);
				UInt8 tHash = ujPrvHashString(type);
			#endif
				
				addr = cls->info.java.methods;
				n = ujThreadReadBE16_ex(cls->info.java.readD, addr);
				addr += 2;
				
				while(n--){
				
					flags = ujThreadReadBE16_ex(cls->info.java.readD, addr);
					
				#ifdef UJ_OPT_CLASS_SEARCH
					if((ujReadClassByte(cls->info.java.readD, addr + 2) == nHash) && (ujReadClassByte(cls->info.java.readD, addr + 3) == tHash)){
				#else
					if(1){
				#endif
						if((flags & flagsAnd) == flagsEq){
						
							//potential match - check it
							p.type = STR_EQ_PAR_TYPE_UJC;
							p.data.ujc.cls = cls;
							p.data.ujc.addr = addr + 4;
							
							if(ujThreadPrvStrEqualEx(&p, name)){	//name match - check type
								
								p.type = STR_EQ_PAR_TYPE_UJC;
								p.data.ujc.cls = cls;
								p.data.ujc.addr = addr + 7;
								
								if(ujThreadPrvStrEqualEx(&p, type)){	//type match - we win
									
									if(flagsP) *flagsP = flags;
									*clsP = cls;
									return ujThreadReadBE24_ex(cls->info.java.readD, addr + 10);
								}
							}
						}
					}
					addr += 13;	
				}
		#endif
		}
		else{			//java class
		
		#ifdef UJ_FTR_SUPPORT_CLASS_FORMAT
		
			UInt16 t16;
			
			addr = cls->info.java.methods;
			t16 = ujThreadReadBE16_ex(cls->info.java.readD, addr - 2);
			
			p.type = STR_EQ_PAR_TYPE_PTR;
			p.data.ptr.len = 4;
			p.data.ptr.str = "Code";
			
			while(t16--){
				
				n = ujThreadReadBE16_ex(cls->info.java.readD, addr + 6);
				if(ujThreadPrvStrEqual(cls, ujThreadReadBE16_ex(cls->info.java.readD, addr + 2), name) && ujThreadPrvStrEqual(cls, ujThreadReadBE16_ex(cls->info.java.readD, addr + 4), type) && ((flags = ujThreadReadBE16_ex(cls->info.java.readD, addr)) & flagsAnd) == flagsEq){
				
					addr += 8;
					while(n--){
					
						if(ujThreadPrvStrEqual(cls, ujThreadReadBE16_ex(cls->info.java.readD, addr), &p)){
							
							if(flagsP) *flagsP = flags;
							*clsP = cls;
							return addr;
						}
						addr = ujPrvSkipAttribute(cls->info.java.readD, addr);
					}
				}
				
				//if we get here, the method doesn't match -> skip it
				addr += 8;
				while(n--) addr = ujPrvSkipAttribute(cls->info.java.readD, addr);
			}
		
		#endif
		}
		if(flagsEqEx & FLAG_DONT_SEARCH_SUBCLASSES) break;
		cls = cls->supr;
	}
	return UJ_PC_BAD;
}

UInt32 ujThreadDbgGetPc(HANDLE threadH){
	
	UInt32 ret;
	UjThread* t;
	
	t = ujHeapHandleLock(threadH);
	ret = t->pc;
	ujHeapHandleRelease(threadH);
	
	return ret;
}

static UInt8 ujThreadPrvGoto(UjThread* t, UjClass* cls, HANDLE objHandle, UInt24 addr){

	UInt16 numLocals = 0;
	
	if(cls->native){	//native class
		
		return (cls->info.native->methods[addr].func)(t, cls);
	}
	else{			//java class
	
		if(cls->ujc){	//UJC
		#ifdef UJ_FTR_SUPPORT_UJC_FORMAT	
			t->methodStartPc = addr;
			numLocals = ujThreadReadBE16_ex(cls->info.java.readD, addr - 4);
		#endif
		}
		else{
		#ifdef UJ_FTR_SUPPORT_CLASS_FORMAT
			t->methodStartPc = addr + 14;
			numLocals = ujThreadReadBE16_ex(cls->info.java.readD, addr + 8);
		#endif
		}
		
		t->pc = t->methodStartPc;
		t->flags.access.hasInst = 0;
		if(objHandle){
			
			t->flags.access.hasInst = 1;
			t->instH = objHandle;
		}
		t->cls = cls;
		
		TL("  goto: 1. num locals = %u, sp=%u locals=%u\n", numLocals, t->spBase, t->localsBase); 

		t->localsBase = t->spBase;
		t->spBase += numLocals;	
		
		TL("  goto: 2. num locals = %u, sp=%u locals=%u\n", numLocals, t->spBase, t->localsBase); 

		if(t->spLimit < t->spBase) return UJ_ERR_STACK_SPACE;
		
		return UJ_ERR_NONE;
	}
}

UInt8 ujThreadGoto(HANDLE threadH, UjClass* cls, const char* methodNamePtr, const char* methodTypePtr){

	UjPrvStrEqualParam name, type;
	UjThread* t = ujHeapHandleLock(threadH);
	UInt24 addr;
	UInt16 needFlags = JAVA_ACC_STATIC;
	UInt8 ret;
	
	if(methodNamePtr[0] == '<'){

		//special calls never search subclasses of asked class
		needFlags |= FLAG_DONT_SEARCH_SUBCLASSES;
	}
	else{

		//this lets us call <clinit> properly
		needFlags |= JAVA_ACC_PUBLIC;
	}
	
	name.type = STR_EQ_PAR_TYPE_PTR;
	name.data.ptr.len = ujCstrlen(name.data.ptr.str = methodNamePtr);
	
	type.type = STR_EQ_PAR_TYPE_PTR;
	type.data.ptr.len = ujCstrlen(type.data.ptr.str = methodTypePtr);
	
	addr = ujThreadPrvGetMethodAddr(&cls, &name, &type, JAVA_ACC_STATIC | JAVA_ACC_PUBLIC | JAVA_ACC_NATIVE, needFlags, NULL);
	if(addr == UJ_PC_BAD){
		ret = UJ_ERR_METHOD_NONEXISTENT;
		goto out;
	}
	
	ret = ujThreadPrvGoto(t, cls, 0, addr);
	
out:
	ujHeapHandleRelease(threadH);
	return ret;
}

Boolean ujCanRun(void){

	return !!gFirstThread;
}

static void ujThreadPrvPut32(UInt8* ptr, UInt32 v){			//to pointer
	const UInt8* d = &v;
	
	*ptr++ = *d++;
	*ptr++ = *d++;
	*ptr++ = *d++;
	*ptr = *d;
}

static void ujThreadPrvPut16(UInt8* ptr, UInt16 v){			//to pointer

	const UInt8* d = &v;
	
	*ptr++ = *d++;
	*ptr = *d;
}

static UInt32 ujThreadPrvGet32(const UInt8* ptr){			//from pointer

	UInt32 v;
	UInt8* d = &v;
	
	*d++ = *ptr++;
	*d++ = *ptr++;
	*d++ = *ptr++;
	*d = *ptr;
	
	return v;
}

static UInt16 ujThreadPrvGet16(const UInt8* ptr){			//from pointer

	UInt16 v;
	UInt8* d = &v;
	
	*d++ = *ptr++;
	*d = *ptr;
	
	return v;
}

static const UInt8 gShifts[8] = {1,2,4,8,16,32,64,128};

static _INLINE_ void ujThreadPrvBitSet(UjThread* t, UInt16 offst){

	UInt8* p = (UInt8*)(t->stack + t->spLimit);
	UInt8 v;
	
	p += (offst >> 3);
	v = gShifts[offst & 7];
	
	*p |= v;
}

static _INLINE_ void ujThreadPrvBitClear(UjThread* t, UInt16 offst){

	UInt8* p = (UInt8*)(t->stack + t->spLimit);
	UInt8 v;
	
	p += (offst >> 3);
	v = gShifts[offst & 7];
	
	*p &=~ v;
}

static _INLINE_ Boolean ujThreadPrvBitGet(UjThread* t, UInt16 offst){

	UInt8* p = (UInt8*)(t->stack + t->spLimit);
	UInt8 v;
	
	p += (offst >> 3);
	v = gShifts[offst & 7];
	
	return !!(*p & v);
}

static Boolean ujThreadPrvPush(UjThread* t, UInt32 v, Boolean isRef){

	TL(" stack push %s 0x%08X\n", isRef ? "ref" : "int", v);

	if(t->spBase < t->spLimit){
	
		t->stack[t->spBase] = v;
		if(isRef) ujThreadPrvBitSet(t, t->spBase);
		t->spBase++;
		
		return true;
	}
	return false;
}

Boolean ujThreadPush(UjThread* t, UInt32 v, Boolean isRef){

	return ujThreadPrvPush(t, v, isRef);
}


static UInt32 ujThreadPrvPop(UjThread* t){

	t->spBase--;

	TL(" stack pop %s 0x%08X\n", ujThreadPrvBitGet(t, t->spBase) ? "ref" : "int", t->stack[t->spBase]);

	ujThreadPrvBitClear(t, t->spBase);
	return t->stack[t->spBase];
}

UInt32 ujThreadPop(UjThread* t){

	return ujThreadPrvPop(t);
}

#define DUP_FLAG_DONT_COPY		0x40	//apply to "howMany" to not copy or adjust stack afterwards

static Boolean ujThreadPrvDup(UjThread* t, Int8 howManyEx, Int8 howFarBelow){	//dup correctly, including "isRef" bits


	/*
		The idea here is as follows(stacks shown with top facing right):
		
		initial stack: ABCDEFGH
		
		howMany = 3, howFarBelow = 1
			
			ABCDFGHEFGH	= ABCD FGH E FGH
		
		howMany = 1, howFarBelow = 0
		
			ABCDEFGHH	= ABCDEFG H H
			
		howMany = 2, howFarBelow = 0
			
			ABCDEFGHGH	= ABCDEF GH GH
	
		howMany = 2, howFarBelow = 2
			
			ABCDGHEFGH	= ABCD GH EF GH
	
		howMany = 2, howFarBelow = 4
			
			ABGHCDEFGH	= AB GH CDEF GH
			
		we need to make space for "howMany" items by shifting "howMany + howFarBelow" items up "howMany" spots
	*/

	UInt16 src;
	UInt16 dst;
	UInt16 i;
	UInt8 howMany = howManyEx &~ DUP_FLAG_DONT_COPY;
	
	//step 0: verify stack has space for "howMany" extra elements

	TL(" dup %d %d below%s sp=%u\n", howMany, howFarBelow, (howManyEx & DUP_FLAG_DONT_COPY) ? " (just shift)" : "", t->spBase);

	if(t->spBase + howMany > t->spLimit) return false;
	
	
	//step 1: move "howMany + howFarBelow" elements up "howMany" spots
	
	src = t->spBase;
	dst = t->spBase + howMany;
	for(i = 0; i < (UInt8)(howMany + howFarBelow); i++){
	
		dst--;
		src--;
		if(ujThreadPrvBitGet(t, src)) ujThreadPrvBitSet(t, dst);
		else ujThreadPrvBitClear(t, dst);
		ujThreadPrvBitClear(t, src);
		t->stack[dst] = t->stack[src];
	}
	
	
	//step 2 and 3, if needed
	
	if(!(howManyEx & DUP_FLAG_DONT_COPY)){
		
		//step2: copy elements to new place
		
		dst = t->spBase - (howMany + howFarBelow);
		src = t->spBase;
		
		for(i = 0; i < howMany; i++){
			
			if(ujThreadPrvBitGet(t, src)) ujThreadPrvBitSet(t, dst);
			else ujThreadPrvBitClear(t, dst);
			ujThreadPrvBitClear(t, src);
			t->stack[dst] = t->stack[src];
			dst++;
			src++;
		}
		
		
		//step 3: adjust sp
		
		t->spBase += howMany;
	}
	
	TL(" dup end with sp %u\n", t->spBase);

	return true;
}


static UInt32 ujThreadPrvPeek(UjThread* t, UInt8 slots/* 0 is top of stack*/){	//peek at stack items without popping
	
	TL(" stack peek %u %s -> 0x%08X\n", slots, ujThreadPrvBitGet(t, t->spBase - (slots + 1)) ? "ref" : "int", t->stack[t->spBase - (slots + 1)]);

	return t->stack[t->spBase - (slots + 1)];
}

static UInt32 ujThreadPrvLocalLoad(UjThread* t, UInt16 idx){

	TL(" local load %u %s -> 0x%08X\n", idx, ujThreadPrvBitGet(t, t->localsBase + idx) ? "ref" : "int", t->stack[t->localsBase + idx]);

	return t->stack[t->localsBase + idx];
}

static void ujThreadPrvLocalStore(UjThread* t, UInt16 idx, UInt32 v, Boolean isRef){

	TL(" local store %u %s 0x%08X\n", idx, isRef ? "ref" : "int", v);

	t->stack[t->localsBase + idx] = v;
	if(isRef) ujThreadPrvBitSet(t, t->localsBase + idx);
	else ujThreadPrvBitClear(t, t->localsBase + idx);
}

#define ujThreadPrvPushRef(t, obj)		if(!ujThreadPrvPush(t, (UInt32)obj, 1)) return UJ_ERR_STACK_SPACE
#define ujThreadPrvPushInt(t, i)		if(!ujThreadPrvPush(t, (UInt32)i, 0)) return UJ_ERR_STACK_SPACE
#define ujThreadPrvPushFloat(t, f)		if(!ujThreadPrvPushFloat_(t, f)) return UJ_ERR_STACK_SPACE
#define ujThreadPrvPushDouble(t, d)		if(!ujThreadPrvPushDouble_(t, d)) return UJ_ERR_STACK_SPACE
#define ujThreadPrvPushLong(t, L)		if(!ujThreadPrvPushLong_(t, L)) return UJ_ERR_STACK_SPACE

#define ujThreadPrvPopInt(t)			((Int32)ujThreadPrvPop(t))
#define ujThreadPrvPopRef(t)			((HANDLE)ujThreadPrvPop(t))
#define ujThreadPrvPopArrayref(t)		((HANDLE)ujThreadPrvPop(t))

#define ujThreadPrvLocalLoadInt(t, idx)		((Int32)ujThreadPrvLocalLoad(t, idx))
#define ujThreadPrvLocalLoadRef(t, idx)		((UInt32)ujThreadPrvLocalLoad(t, idx))

#define ujThreadPrvLocalStoreInt(t, idx, i)	ujThreadPrvLocalStore(t, idx, (UInt32)i, 0)
#define ujThreadPrvLocalStoreRef(t, idx, obj)	ujThreadPrvLocalStore(t, idx, (UInt32)obj, 1)



#if defined(UJ_FTR_SUPPORT_DOUBLE)

	static Boolean ujThreadPrvPushDouble_(UjThread* t, Double64 d){
	
		return ujThreadPrvPush(t, d64_getTopWord(d), 0) && ujThreadPrvPush(t, d64_getBottomWord(d), 0);	
	}
	
	static Double64 ujThreadPrvPopDouble(UjThread* t){
	
		UInt32 tmp = ujThreadPrvPop(t);
		
		return d64_fromHalves(ujThreadPrvPop(t), tmp);
	}

#endif

#if defined(UJ_FTR_SUPPORT_FLOAT)

	static Boolean ujThreadPrvPushFloat_(UjThread* t, UjFloat f){
	
		return ujThreadPrvPush(t, *(UInt32*)&f, 0);	
	}
	
	static UjFloat ujThreadPrvPopFloat(UjThread* t){
	
		UInt32 tmp = ujThreadPrvPop(t);
		
		return *(UjFloat*)&tmp;
	}

#endif

#if defined(UJ_FTR_SUPPORT_LONG) || defined(UJ_FTR_SUPPORT_DOUBLE)
	
	static Boolean ujThreadPrvPushLong_(UjThread* t, Int64 L){
		
		return ujThreadPrvPush(t, u64_get_hi(L), 0) && ujThreadPrvPush(t, u64_64_to_32(L), 0);
	}


	static Int64 ujThreadPrvPopLong(UjThread* t){
	
		UInt32 lo = ujThreadPrvPop(t);
		Int64 ret = u64_from_halves(ujThreadPrvPop(t), lo);
		
		return ret;
	}
	
	static Int64 ujThreadPrvLocalLoadLong(UjThread* t, UInt16 idx){
	
		UInt32 hi = ujThreadPrvLocalLoadInt(t, idx);
		Int64 ret = u64_from_halves(hi, ujThreadPrvLocalLoadInt(t, idx + 1));
		
		return ret;
	}
	
	static void ujThreadPrvLocalStoreLong(UjThread* t, UInt16 idx, Int64 L){
		
		ujThreadPrvLocalStore(t, idx, u64_get_hi(L), 0);
		ujThreadPrvLocalStore(t, idx + 1, u64_64_to_32(L), 0);
	}

#endif

static Int32 ujThreadPrvArrayGetLength(HANDLE arrHandle){

	Int32 ret;
	
	ret = ((UjArray*)ujHeapHandleLock(arrHandle)) -> length;
	ujHeapHandleRelease(arrHandle);
	
	return ret;	
}

static Int32 ujThreadPrvArrayGet4B(HANDLE arrHandle, Int32 idx){

	Int32 ret;
	
	
	idx <<= 2;
	ret = ujThreadPrvGet32(((UjArray*)ujHeapHandleLock(arrHandle))->data + idx);
	ujHeapHandleRelease(arrHandle);
	
	return ret;
}

static Int16 ujThreadPrvArrayGet2B(HANDLE arrHandle, Int32 idx){

	Int16 ret;
	
	idx <<= 1;
	ret = ujThreadPrvGet16(((UjArray*)ujHeapHandleLock(arrHandle))->data + idx);
	ujHeapHandleRelease(arrHandle);
	
	return ret;
}

static Int8 ujThreadPrvArrayGet1B(HANDLE arrHandle, Int32 idx){

	Int8 ret;
	
	ret = ((UjArray*)ujHeapHandleLock(arrHandle))->data[idx];
	ujHeapHandleRelease(arrHandle);
	
	return ret;
}

UInt32 ujArrayLen(UInt32 arr){	//external use

	return ujThreadPrvArrayGetLength((HANDLE)arr);
}

UInt32 ujArrayGetByte(UInt32 arr, UInt32 idx){	//external use

	return ujThreadPrvArrayGet1B((HANDLE)arr, idx);
}

UInt32 ujArrayGetShort(UInt32 arr, UInt32 idx){	//external use

	return ujThreadPrvArrayGet2B((HANDLE)arr, idx);
}

UInt32 ujArrayGetInt(UInt32 arr, UInt32 idx){	//external use

	return ujThreadPrvArrayGet4B((HANDLE)arr, idx);
}

void* ujArrayRawAccessStart(UInt32 arr){

	return ((UjArray*)ujHeapHandleLock(arr))->data;
}

void ujArrayRawAccessFinish(UInt32 arr){

	ujHeapHandleRelease(arr);	
}

#if defined(UJ_FTR_SUPPORT_LONG) || defined(UJ_FTR_SUPPORT_DOUBLE)
	static Int64 ujThreadPrvArrayGetLong(HANDLE arrHandle, Int32 idx){
	
		Int64 ret;
		UjArray* arr = (UjArray*)ujHeapHandleLock(arrHandle);
	
		idx <<= 3;
		ret = u64_from_halves(ujThreadPrvGet32(arr->data + idx),  ujThreadPrvGet32(arr->data + idx + 4));
		ujHeapHandleRelease(arrHandle);
	
		return ret;
	}
#endif

#define ujThreadPrvArrayGetInt(arr,idx)		((Int32)ujThreadPrvArrayGet4B(arr, idx))
#define ujThreadPrvArrayGetRef(arr,idx)		((HANDLE)ujThreadPrvArrayGet4B(arr, idx))
#define ujThreadPrvArrayGetByte(arr,idx)	((Int8)ujThreadPrvArrayGet1B(arr, idx))
#define ujThreadPrvArrayGetShort(arr,idx)	((Int16)ujThreadPrvArrayGet2B(arr, idx))
#define ujThreadPrvArrayGetChar(arr,idx)	((UInt16)ujThreadPrvArrayGet2B(arr, idx))

static void ujThreadPrvArraySet4B(HANDLE arrHandle, Int32 idx, UInt32 v){

	idx <<= 2;
	ujThreadPrvPut32(((UjArray*)ujHeapHandleLock(arrHandle))->data + idx, v);
	ujHeapHandleRelease(arrHandle);
}

static void ujThreadPrvArraySet2B(HANDLE arrHandle, Int32 idx, UInt16 v){

	idx <<= 1;
	ujThreadPrvPut16(((UjArray*)ujHeapHandleLock(arrHandle))->data + idx, v);
	ujHeapHandleRelease(arrHandle);
}

static void ujThreadPrvArraySet1B(HANDLE arrHandle, Int32 idx, UInt8 v){

	((UjArray*)ujHeapHandleLock(arrHandle))->data[idx] = v;
	ujHeapHandleRelease(arrHandle);
}

#if defined(UJ_FTR_SUPPORT_LONG) || defined(UJ_FTR_SUPPORT_DOUBLE)
	static void ujThreadPrvArraySetLong(HANDLE arrHandle, Int32 idx, UInt64 val){
	
		UjArray* arr = (UjArray*)ujHeapHandleLock(arrHandle);
	
		idx <<= 3;
		ujThreadPrvPut32(arr->data + idx + 0, u64_get_hi(val));
		ujThreadPrvPut32(arr->data + idx + 4, u64_64_to_32(val));
		
		ujHeapHandleRelease(arrHandle);
	}
#endif

#define ujThreadPrvArraySetInt(arr, idx, i)	ujThreadPrvArraySet4B(arr, idx, (UInt32)i)
#define ujThreadPrvArraySetRef(arr, idx, obj)	ujThreadPrvArraySet4B(arr, idx, (UInt32)obj)
#define ujThreadPrvArraySetChar(arr, idx, ch)	ujThreadPrvArraySet2B(arr, idx, ch)
#define ujThreadPrvArraySetShort(arr, idx, sh)	ujThreadPrvArraySet2B(arr, idx, sh)
#define ujThreadPrvArraySetByte(arr, idx, b)	ujThreadPrvArraySet1B(arr, idx, b)

static UInt8 ujThreadPrvArrayBoundsCheck(HANDLE arrHandle, Int32 idx){

	if(!arrHandle) return UJ_ERR_NULL_POINTER;
	if(idx < 0 || idx >= ujThreadPrvArrayGetLength(arrHandle)) return UJ_ERR_ARRAY_INDEX_OOB;

	return UJ_ERR_NONE;
}

static UInt8 ujThreadPushRetInfo(UjThread* t){	//push all that we need to come back here using a return
	
	//XXX: make sure THREAD_RET_INFO_SZ is correct, or else...
	
	//we push: localsBase, cls/inst, methodStartPc, method, pc (as offset form methodStart pc)  [we combine the last 2 into a single U32)
	//we use top bit of methodStartPc to indicate if we have instance (else we have class)
	UInt32 combined;
	
	
	//we need to push: localsBase(16 bit), cls/inst( <= 32-bit), methodStartPc(24-bit), pc (24-bit), flags(8-bit)
	//how we do it:
	// *  (pc - methodStartPc) is always 16 bit, call it A
	// *  combine A with localsBase into a 32-bit value call it B
	// *  methodStartPc is 24-bit so we use top 8 for flags call this C
	// we thus have to push 3 32-bit words: B, cls/inst, C
	
	TL(" pushing ret info with locals=%u, sp=%u, pc=0x%06X\n", t->localsBase, t->spBase, t->pc);
	
	if(t->flags.access.hasInst){
		
		ujThreadPrvPushRef(t, t->instH);	//needed to keep ref to the obj
	}
	else{
	
		ujThreadPrvPushInt(t, (UInt32)t->cls);
	}
	
	combined = t->localsBase;
	combined <<= 16;
	combined |= (t->pc - t->methodStartPc);
	ujThreadPrvPushInt(t, combined);
	
	combined = t->flags.raw;
	combined <<= 24;
	combined |= t->methodStartPc;
	ujThreadPrvPushInt(t, combined);
	
	TL(" push ret info done with sp=%u\n", t->spBase);

	return UJ_ERR_NONE;
}

static UInt8 ujThreadPrvRet(UjThread* t, HANDLE threadH){	//the opposite of the above

	UjInstance* inst;
	UInt32 combined;
	UInt8 ret;
	
	//no matter where sp is (stack may be non-empty), restore it to original place
	//while doing that, clear bits for "ref" since locals are now gone

	TL(" performing return with locals=%u, sp=%u, pc=0x%06X\n", t->localsBase, t->spBase, t->pc);

	while(t->spBase > t->localsBase){
	
		t->spBase--;
		ujThreadPrvBitClear(t, t->spBase);
	}
	
	if(t->spBase == 0){	//return from top level func

		TL(" return terminates thread %d\n", threadH);
	
		t->pc = UJ_PC_DONE;	//will be destroyed later
	}
	else{			//we have levels to return to
	
		#ifdef UJ_FTR_SYNCHRONIZATION
			if(t->flags.access.syncronized){	//release the monitor
				
				if(t->flags.access.hasInst){
					
					inst = ujHeapHandleLock(t->instH);
					ret = ujThreadPrvMonExit(threadH, &inst->mon);
					ujHeapHandleRelease(t->instH);
				}
				else{
				
					ret = ujThreadPrvMonExit(threadH, &t->cls->mon);
				}
				if(ret != UJ_ERR_NONE) return ret;
			}
		#endif
	
		//now pop off things we need and process as needed
		combined = ujThreadPrvPopInt(t);
		t->methodStartPc = combined & 0x00FFFFFFUL;
		t->flags.raw = combined >> 24;
		
		combined = ujThreadPrvPopInt(t);
		t->pc = t->methodStartPc + (combined & 0x0000FFFFUL);
		t->localsBase = combined >> 16;
		
		combined = ujThreadPrvPopInt(t);
		if(t->flags.access.hasInst){
		
			t->instH = (HANDLE)combined;
			inst = ujHeapHandleLock(t->instH);
			t->cls = inst->cls;
			ujHeapHandleRelease(t->instH);
		}
		else{
		
			t->cls = (UjClass*)combined;
		}
	}
	TL(" return completes with locals=%u, sp=%u, pc=0x%06X\n", t->localsBase, t->spBase, t->pc);

	return UJ_ERR_NONE;
}

static void ujThreadPrvJumpIfNeeded(UjThread* t, Boolean needed){
	
	TL(" jump on cond %s at pc 0x%06X\n", needed ? "TRUE" : "FALSE", t->pc);

	if(needed){						//jump there
		
		t->pc += ujThreadReadBE16(t, t->pc);
		t->pc--;				//we cnanot combine this and next instr due to Int16's range. Think about (0x8000 - 1)
	}
	else t->pc += 2;					//skip offset and go on
	TL(" jumped to pc 0x%06X\n", t->pc);
}

static _INLINE_ UInt16 ujThreadPrvGetOffset(UjThread* t, Boolean wide){

	UInt16 t16;
	
	t16 = wide ? ujThreadReadBE16(t, t->pc) : ujThreadPrvFetchClassByte(t, t->pc);
	t->pc += wide ? 2 : 1;
	
	return t16;
}

static UInt32 ujThreadPrvReadConst32(UjThread* t, UInt16 idx, UInt8* typeP){

	UInt32 addr;
	UInt8 type;
	
	addr = ujThreadPrvFindConst(t, idx);
	type = ujThreadPrvFetchClassByte(t, addr++);
	if(typeP) *typeP = type;
	
	if(type == JAVA_CONST_TYPE_CLASS || type == JAVA_CONST_TYPE_STR_REF || type == JAVA_CONST_TYPE_STRING){	//handle these specially
	
		if(t->cls->ujc){
		#ifdef UJ_FTR_SUPPORT_UJC_FORMAT	
			return addr - 1;
		#endif
		}
		else{
		#ifdef UJ_FTR_SUPPORT_CLASS_FORMAT
			return ujThreadPrvFindConst(t, ujThreadReadBE16(t, addr));
		#endif
		}
	}
	
	return ujThreadReadBE32(t, addr);
}

#if defined(UJ_FTR_SUPPORT_LONG) || defined(UJ_FTR_SUPPORT_DOUBLE)
	static UInt64 ujThreadPrvReadConst64(UjThread* t, UInt16 idx){
	
		UInt24 addr;
		
		addr = ujThreadPrvFindConst(t, idx);
		return u64_from_halves(ujThreadReadBE32(t, addr + 1), ujThreadReadBE32(t, addr + 5));
	}
#endif

static UInt8 ujThreadPrvNewArray(char type, Int32 len, HANDLE* arrP){	//remember to zero contents

	HANDLE handle;
	UjArray* arr;
	
	if(len < 0) return UJ_ERR_NEG_ARR_SZ;
	
	handle = ujHeapHandleNew(sizeof(UjArray) + len * ujPrvJavaTypeToSize(type));
	if(!handle) return UJ_ERR_OUT_OF_MEMORY;
	
	arr = ujHeapHandleLock(handle);
	arr->cls = 0;
	arr->objType = (type == JAVA_TYPE_ARRAY || type == JAVA_TYPE_OBJ) ? OBJ_TYPE_OBJ_ARRAY : OBJ_TYPE_ARRAY;
	arr->length = len;

	ujHeapHandleRelease(handle);
	
	*arrP = handle;
	
	return UJ_ERR_NONE;
}

static UInt8 ujThreadPrvMultiNewArrayHelper(UjThread* t, UInt24 type, UInt8 thisDim, UInt8 totalDim, HANDLE* arrP){

	Int32 i, numElem = ujThreadPrvPeek(t, thisDim);
	char typeByte = ujThreadPrvFetchClassByte(t, type + totalDim - thisDim);
	HANDLE arr, sub;
	UInt8 t8;
	
	if(numElem < 0) return UJ_ERR_NEG_ARR_SZ;
	
	//first create our array
	t8 = ujThreadPrvNewArray(typeByte, numElem, &arr);
	if(t8 != UJ_ERR_NONE) return t8;
	
	//now create each element in turn, if needed
	if(thisDim){
		for(i = 0; i < numElem; i++){
		
			//alloc sub-array
			t8 = ujThreadPrvMultiNewArrayHelper(t, type, thisDim - 1, totalDim, &sub);
			if(t8 != UJ_ERR_NONE) return t8;
			
			//assign it
			ujThreadPrvArraySetRef(arr, i, sub);
		}
		
		if(i != numElem){
		
			arr = 0;	//we fail to alloc array, do not return partials	
		}
	}
	
	*arrP = arr;
	
	return UJ_ERR_NONE;
}

static UInt8 ujThreadPrvHandleMultiNewArray(UjThread* t, UInt16 typeIdx, UInt8 numDim, HANDLE* arrP){

	UInt8 ret;
	UInt24 typeAddr;
	
	if(!t->cls->ujc){
	#ifdef UJ_FTR_SUPPORT_CLASS_FORMAT
		typeIdx = ujThreadReadBE16(t, ujThreadPrvFindConst(t, typeIdx) + 1);
	#endif
	}
	
	typeAddr = ujThreadPrvFindConst(t, typeIdx) + 3;
	
	///UInt24 typeAddr = ujThreadPrvFindConst(t, typeIdx) + 3;	//string itself
	
	//UInt32 ujThreadPrvPeek(UjThread* t, UInt8 slots/* 0 is top of stack*/)	
	//we create the arrays from first dimension to last, but the sizes are pushed
	// in reverse order, so we ust use peek to get them (also we need to remember
	// to pop them later
	
	ret = ujThreadPrvMultiNewArrayHelper(t, typeAddr, numDim - 1, numDim, arrP);
	while(numDim--) ujThreadPrvPopInt(t);	//pop indices off the stack
	return ret;
}

static HANDLE ujThreadPrvNewInstance(UjClass* cls){

	UjInstance* inst;
	HANDLE handle = ujHeapHandleNew(sizeof(UjInstance) + cls->instDataOfst + cls->instDataSize);
	
	if(!handle) return 0;

	TL(" creating new instance of class 0x%08X of size %u (datsz=%u)\n", cls, cls->instDataOfst + cls->instDataSize, sizeof(UjInstance) + cls->instDataOfst + cls->instDataSize);
	
	inst = ujHeapHandleLock(handle);
	#ifdef UJ_FTR_SYNCHRONIZATION
		inst->mon.numHolds = 0;
	#endif
	inst->cls = cls;
	ujHeapHandleRelease(handle);
	
	return handle;
}

#ifdef UJ_DBG_HELPERS

	void ujDbgPrintJavaString(HANDLE handle){
	
		UjInstance* inst;
		UjClass* cls;
		UInt32 extra;
		UInt16 sz;
		
		inst = ujHeapHandleLock(handle);
		
		#ifdef UJ_OPT_RAM_STRINGS
			
			cls = NULL;
			extra = ujThreadPrvGet32(inst->data + inst->cls->supr->instDataOfst + 0);
		#else
		
			cls = (UjClass*)ujThreadPrvGet32(inst->data + inst->cls->supr->instDataOfst + 0);
			extra = ujThreadPrvGet32(inst->data + inst->cls->supr->instDataOfst + 4);
		#endif
		
		ujHeapHandleRelease(handle);
		
		if(cls){
		
			sz = ujThreadReadBE16_ex(cls->info.java.readD, extra);
			extra += 2;
			
			fprintf(stderr, "STRING (%u): '", sz);
			while(sz--) fprintf(stderr, "%c", ujReadClassByte(cls->info.java.readD, extra++));
			fprintf(stderr, "'\n");
		}
		else{
		
			UInt8* ptr = ujHeapHandleLock(extra);
			sz = ujThreadPrvGet16(ptr);
			ptr += 2;
			
			fprintf(stderr, "STRING (%u): '", sz);
			while(sz--) fprintf(stderr, "%c", *ptr++);
			fprintf(stderr, "'\n");
		}
	}

	void ujDbgPrintString(UjClass* cls, UInt16 idx){
	
		UInt24 addr = ujThreadPrvFindConst_ex(cls,cls->ujc ? idx : ujThreadReadBE16_ex(cls->info.java.readD, 1 + ujThreadPrvFindConst_ex(cls, idx)));
		UInt16 len = ujThreadReadBE16_ex(cls->info.java.readD, addr + 1);
		addr += 3;
	
		fprintf(stderr, "\"");
		while(len--) fprintf(stderr, "%c", ujReadClassByte(cls->info.java.readD, addr++));
		fprintf(stderr, "\"\n");
	}
	
	void ujDbgPrintClass(UjClass* cls){
	
		fprintf(stderr, "CLASS ");
		if(cls->native) fprintf(stderr, "\"%s\"\n", cls->info.native->clsName);
		else ujDbgPrintString(cls, ujThreadReadBE16_ex(cls->info.java.readD, cls->ujc ? 2 : cls->info.java.interfaces - 6));
	}
#endif


static UInt8 ujPrvNewStringObj(HANDLE* hP){

	UjPrvStrEqualParam p;
	static UjClass* cls = NULL;
	HANDLE handle;
	
	if(!cls){
		p.type = STR_EQ_PAR_TYPE_PTR;
		p.data.ptr.len = ujCstrlen(p.data.ptr.str = "java/lang/String");
		
		cls = ujThreadPrvFindClass(&p);
	}
	if(!cls) return UJ_ERR_DEPENDENCY_MISSING;
	
	handle = ujThreadPrvNewInstance(cls);
	if(!handle) return UJ_ERR_OUT_OF_MEMORY;
	
	*hP = handle;
	return UJ_ERR_NONE;
}

static UInt8 ujThreadPrvNewConstString(UjThread* t, UInt24 addr, HANDLE* handleP){

	UInt8 ret;
	UjInstance* inst;
	
	ret = ujPrvNewStringObj(handleP);
	if(ret != UJ_ERR_NONE) return ret;
	//we have to lock now it to avoid it being GCed
	inst = ujHeapHandleLock(*handleP);
	addr++;
		
	#ifdef UJ_OPT_RAM_STRINGS

		HANDLE stringData;
		UInt8* dst;
		UInt16 len;
		
		len = ujThreadReadBE16(t, addr);
		addr += 2;
		stringData = ujHeapHandleNew(len + 2);
		if(!stringData){
			
			ujHeapHandleRelease(*handleP);
			ujHeapHandleFree(*handleP);
			return UJ_ERR_OUT_OF_MEMORY;
		}
		
		ujThreadPrvPut32(inst->data + inst->cls->supr->instDataOfst + 0, stringData);
		
		dst = ujHeapHandleLock(stringData);
		ujThreadPrvPut16(dst, len);
		dst += 2;
		while(len--) *dst++ = ujThreadPrvFetchClassByte(t, addr++);
		ujHeapHandleRelease(stringData);
	
	#else
		
		ujThreadPrvPut32(inst->data + inst->cls->supr->instDataOfst + 0, (UInt32)t->cls);
		ujThreadPrvPut32(inst->data + inst->cls->supr->instDataOfst + 4, addr);
		
	#endif
	
	ujHeapHandleRelease(*handleP);
	
	return UJ_ERR_NONE;
}

static void ujThreadProcessTrippleRef(UjThread* t, UInt16 idx, UjPrvStrEqualParam* clsI, UjPrvStrEqualParam* nameI, UjPrvStrEqualParam* typeI){

	UInt24 addr = ujThreadPrvFindConst(t, idx);
	
	if(t->cls->ujc){	//ujc
	#ifdef UJ_FTR_SUPPORT_UJC_FORMAT
		
		if(clsI){
			
			clsI->type = STR_EQ_PAR_TYPE_ADR;
			clsI->data.adr.readD = t->cls->info.java.readD;
			clsI->data.adr.addr = ujThreadReadBE24(t, addr + 1);
		}
		
		if(nameI){
			
			nameI->type = STR_EQ_PAR_TYPE_ADR;
			nameI->data.adr.readD = t->cls->info.java.readD;
			nameI->data.adr.addr = ujThreadReadBE24(t, addr + 4);
		}
		
		if(typeI){
			
			typeI->type = STR_EQ_PAR_TYPE_ADR;
			typeI->data.adr.readD = t->cls->info.java.readD;
			typeI->data.adr.addr = ujThreadReadBE24(t, addr + 7);
		}
	#endif
	}
	else{			//java class
	#ifdef UJ_FTR_SUPPORT_CLASS_FORMAT
	
		if(clsI){
			
			clsI->type = STR_EQ_PAR_TYPE_IDX;
			clsI->data.idx.cls = t->cls;
			clsI->data.idx.strIdx = ujThreadReadBE16(t, ujThreadPrvFindConst(t, ujThreadReadBE16(t, addr + 1)) + 1);
		}
		
		addr = ujThreadPrvFindConst(t, ujThreadReadBE16(t, addr + 3));
		
		if(nameI){
			
			nameI->type = STR_EQ_PAR_TYPE_IDX;
			nameI->data.idx.cls = t->cls;
			nameI->data.idx.strIdx = ujThreadReadBE16(t, addr + 1);
		}
		
		if(typeI){
			
			typeI->type = STR_EQ_PAR_TYPE_IDX;
			typeI->data.idx.cls = t->cls;
			typeI->data.idx.strIdx = ujThreadReadBE16(t, addr + 3);
		}
	#endif
	}
}

static UInt8 ujThreadPrvInvoke(UjThread* t, HANDLE threadH, UInt8 numParams, UInt8 invokeType, UInt8 pcBytes){

	UjPrvStrEqualParam p1, p2, p3;
	UjClass* cls = NULL;
	HANDLE objRef = 0;
	UInt24 addr;
	UInt16 len, nameIdx;
	UInt8 ret;
	Boolean isSyncNow = false;
	
	
	if(numParams){
		
		nameIdx = numParams - 1;
	}
	else{
		
		ujThreadProcessTrippleRef(t, ujThreadReadBE16(t, t->pc), &p3, &p1, &p2);
		t->pc += pcBytes;
		
		TL(" invoking pc=0x%06X sp=%u locals=%u\n", t->pc, t->spBase, t->localsBase);
	
		ujThreadPrvStrEqualProcessParam(&p2);
		addr = p2.data.adr.addr;
		len = ujThreadReadBE16(t, addr);	//string length
		addr += 2;
		nameIdx = 1;				//number of slots params take	("this" ref is implicit)
		ret = 1;				//set if we're counting, clear if not
		
		if(ujThreadPrvFetchClassByte(t, addr++) != '('){
			
			return UJ_ERR_METHOD_NONEXISTENT;	//invalid method...
		}
		
		while(len--){
		
			switch(ujThreadPrvFetchClassByte(t, addr++)){
				
				case JAVA_TYPE_DOUBLE:
				case JAVA_TYPE_LONG:
				
					if(ret) nameIdx++;
					//fallthrough
				
				case JAVA_TYPE_BYTE:
				case JAVA_TYPE_CHAR:
				case JAVA_TYPE_FLOAT:
				case JAVA_TYPE_INT:
				case JAVA_TYPE_SHORT:
				case JAVA_TYPE_BOOL:
				
					if(ret) nameIdx++;
					ret = 1;
					break;
				
				case JAVA_TYPE_ARRAY:
					
					if(ret) nameIdx++;
					ret = 0;
					break;
				
				case JAVA_TYPE_OBJ:
					
					if(ret) nameIdx++;
					while(len-- && ujThreadPrvFetchClassByte(t, addr++) != JAVA_TYPE_OBJ_END);
					break;
				
				case ')':	//done
					
					len = 0;
					break;
			}
		}
		
		if(invokeType == UJ_INVOKE_STATIC) nameIdx--;
	}
	len = 0;	//now used for flags
	TL("  prelim: %u stack spots used by params\n" ,nameIdx);
	
	switch(invokeType){
		
		case UJ_INVOKE_VIRTUAL:		//TODO: cannot access private funcs
		case UJ_INVOKE_SPECIAL:		//TODO: can access private funcs
		case UJ_INVOKE_INTERFACE:	//XXX: is this correct?
		
			objRef = (HANDLE)ujThreadPrvPeek(t, nameIdx - 1);
			if(!objRef) return UJ_ERR_NULL_POINTER;
			if(invokeType != UJ_INVOKE_SPECIAL){	//dynamic binding
				
				UjInstance* inst = (UjInstance*)ujHeapHandleIsLocked(objRef);
				ret = 0;
				if(!inst){
					inst = (UjInstance*)ujHeapHandleLock(objRef);
					ret = 1;
				}
				
				cls = inst->cls;
				
				if(ret) ujHeapHandleRelease(objRef);
			}
			break;
		
		case UJ_INVOKE_STATIC:
		
			len = JAVA_ACC_STATIC;
			break;
	}

        TL("  final: %u stack spots used by params\n" ,nameIdx);

	/*
	
		TODO:
		XXX:
		
		 invokevirtual will just use function name to do lookup and walk hierarchy backwards
		 invokespecial will use given class
		 invokedynamic is weird
		 invokestatic alway uses given class, duh
		 invokeinterface is the same as invokevirtual for us
	
	*/
	
	
	if(!cls){
		
		cls = numParams ? t->cls : ujThreadPrvFindClass(&p3);
		if(!cls){
			
			return UJ_ERR_METHOD_NONEXISTENT;
		}
	}
	
	if(numParams){
	
		addr = ujThreadReadBE16(t, t->pc);
		addr = (addr << 4) - (addr << 1) - addr;
		addr += cls->info.java.methods ;
		t->pc += 2;
		len = ujThreadReadBE16(t, addr + 2);
		addr = ujThreadReadBE24(t, addr + 12);
	}
	else{
		addr = ujThreadPrvGetMethodAddr(&cls, &p1, &p2, JAVA_ACC_STATIC, len, &len);	//len now has flags
		if(addr == UJ_PC_BAD){
			
			return UJ_ERR_METHOD_NONEXISTENT;
		}
	}
	
	TL("  addr = 0x%06X\n", addr);

	#ifdef UJ_FTR_SYNCHRONIZATION
		if(len & JAVA_ACC_SYNCHRONIZED){
		
			UjMonitor* mon;
		
			mon = &cls->mon;	
			if(invokeType != UJ_INVOKE_STATIC) mon = &((UjInstance*)ujHeapHandleLock(objRef))->mon;
			ret = ujThreadPrvMonEnter(threadH, mon);
			if(invokeType != UJ_INVOKE_STATIC) ujHeapHandleRelease(objRef);
			if(!ret) return UJ_ERR_RETRY_LATER;
			
			isSyncNow = true;
		}
	#endif
	
	if(!cls->native){	//java classes get the whole thing done for them. native ones don't need these crutches
			
		//nameIdx is now the number of stack slots our params take up (assuming object reference is included
		//make stack space
		
		//take top "numToMove" items off the stack, push "moveBy" garbage slots onto stack, repush popped items back
		//set sp to point to the garbage (leaving the re-pushed items above stack pointer and thus inaccessible by pop)
		//used for function calls (where stack items become local variables)
		if(!ujThreadPrvDup(t, THREAD_RET_INFO_SZ | DUP_FLAG_DONT_COPY, nameIdx - THREAD_RET_INFO_SZ)) return UJ_ERR_STACK_SPACE;
		t->spBase -= nameIdx;
		
		//push return info
		ret = ujThreadPushRetInfo(t);
		if(ret != UJ_ERR_NONE) return ret;
		
		
		t->flags.access.syncronized = isSyncNow;
	}

	TL("  goto 0x%06X with cls 0x%08X and obj %u\n", addr, cls, objRef);
	ret = ujThreadPrvGoto(t, cls, objRef, addr);
	return ret;
}

static UjClass* ujThreadPrvClassFromRef(UjClass* cls, UInt16 classDescrIdx){

	UjPrvStrEqualParam p;
	
	if(cls->ujc){
	#ifdef UJ_FTR_SUPPORT_UJC_FORMAT
		p.type = STR_EQ_PAR_TYPE_IDX;
		p.data.idx.strIdx = classDescrIdx;
		p.data.idx.cls = cls;
	#endif
	}
	else{		//java class
	#ifdef UJ_FTR_SUPPORT_CLASS_FORMAT
		p.type = STR_EQ_PAR_TYPE_REF;
		p.data.ref.from.cls = cls;
		p.data.ref.idx = classDescrIdx;
		p.data.ref.offset = 1;
	#endif
	}
	
	return ujThreadPrvFindClass(&p);	
}

static UInt32 ujThreadPrvNewObj(UjThread* t, UInt16 classDescrIdx, HANDLE* handleP){

	UjClass* cls = ujThreadPrvClassFromRef(t->cls, classDescrIdx);
	if(!cls) return UJ_ERR_DEPENDENCY_MISSING;
	
	*handleP = ujThreadPrvNewInstance(cls);
	if(!*handleP) return UJ_ERR_OUT_OF_MEMORY;
	
	return UJ_ERR_NONE;
}

#define UJ_ACCESS_PUT		1	//these are not random and canot be changed (see instr decoding)
#define UJ_ACCESS_FIELD		2


static UInt8 ujThreadPrvAccessClass(UjThread* t, char knownType, UInt16 descrIdx_or_ofst, UInt8 flags){	//see UJ_ACCESS_PUT, UJ_ACCESS_FIELD

	UjPrvStrEqualParam p1, p2, p3;
	UjClass* cls;
	UInt16 n, ofst, wantedFlag = JAVA_ACC_STATIC, numFields;
	UInt24 addr;
	HANDLE handle = 0;
	UInt8* ptr;
	UInt8 sz = 0;
	char type;
	#ifdef UJ_OPT_CLASS_SEARCH
		UInt8 wantedNameHash = 0, fieldNameHash;
	#endif

	TL(" performing class access on descr %u at pc 0x%06X with sp=%u\n", descrIdx_or_ofst,t->pc, t->spBase);
	
	if(knownType){
	
		cls = t->cls;	
	}
	else{
	
		ujThreadProcessTrippleRef(t, descrIdx_or_ofst, &p1, &p2, &p3);	//n = type
	
		cls = ujThreadPrvFindClass(&p1);
		if(!cls) return UJ_ERR_DEPENDENCY_MISSING;
		
		ujThreadPrvStrEqualProcessParam(&p3);
		type = ujReadClassByte(cls->info.java.readD, p3.data.adr.addr + 2);	//first char of type
	}
	
	if(flags & UJ_ACCESS_FIELD){
	
		wantedFlag = 0;
		ofst = cls->instDataOfst;
	}
	else{
	
		ofst = cls->clsDataOfst;
	}
	
	if(knownType){
	
		type = knownType;
		ofst += descrIdx_or_ofst;
		sz = ujPrvJavaTypeToSize(type);
	}
	else{
		addr = cls->info.java.fields;
		numFields = ujThreadReadBE16_ex(cls->info.java.readD, addr - 2);
		
		#ifdef UJ_OPT_CLASS_SEARCH
			wantedNameHash = ujPrvHashString(&p2);
		#endif
	
		while(1){
			if(!numFields--) return UJ_ERR_FIELD_NOT_FOUND;
		
			if((ujThreadReadBE16_ex(cls->info.java.readD, addr) & JAVA_ACC_STATIC) == wantedFlag){
				
				if(cls->ujc){	//handle ujc
				#ifdef UJ_FTR_SUPPORT_UJC_FORMAT
				
					#ifdef UJ_OPT_CLASS_SEARCH
						fieldNameHash = ujReadClassByte(cls->info.java.readD, addr + 2);
					#endif
					p1.type = STR_EQ_PAR_TYPE_ADR;
					p1.data.adr.readD = t->cls->info.java.readD;
					p1.data.adr.addr = ujThreadReadBE24_ex(cls->info.java.readD, addr + 4) + 1;	
				
					sz = ujReadClassByte(cls->info.java.readD, ujThreadReadBE24_ex(cls->info.java.readD, addr + 7) + 3);
				#endif
				}
				else{		//java class
				#ifdef UJ_FTR_SUPPORT_CLASS_FORMAT
				
					//we have no hash, so pretend it matched
					#ifdef UJ_OPT_CLASS_SEARCH
						fieldNameHash = wantedNameHash;
					#endif
					p1.type = STR_EQ_PAR_TYPE_IDX;
					p1.data.idx.cls = t->cls;
					p1.data.idx.strIdx = ujThreadReadBE16_ex(cls->info.java.readD, addr + 2);
					
					sz = ujReadClassByte(cls->info.java.readD, ujThreadPrvFindConst_ex(cls, ujThreadReadBE16_ex(cls->info.java.readD, addr + 4)) + 3);
				
				#endif
				}
				sz = ujPrvJavaTypeToSize(sz);
				
			#ifdef UJ_OPT_CLASS_SEARCH
				if(fieldNameHash == wantedNameHash){
			#else
				if(1){
			#endif
					if(ujThreadPrvStrEqualEx(&p1, &p2)){	//we found it
				
						break;
					}
				}
				ofst += sz;
			}
			
			if(cls->ujc){	//handle UJC
			#ifdef UJ_FTR_SUPPORT_UJC_FORMAT
				addr += 10;
			#endif
			}
			else{		//java class
			#ifdef UJ_FTR_SUPPORT_CLASS_FORMAT
			
				n = ujThreadReadBE16_ex(cls->info.java.readD, addr + 6);
				addr += 8;
				while(n--) addr = ujPrvSkipAttribute(cls->info.java.readD, addr);
			#endif
			}
		}
	}
	//if we got here, we found it and "ofst" is the offset

	if(flags & UJ_ACCESS_FIELD){
	
		handle = (HANDLE)ujThreadPrvPeek(t, (flags & UJ_ACCESS_PUT) ? ((sz + 3) >> 2) :  0);
		if(!handle) return UJ_ERR_NULL_POINTER;
		ptr = ((UjInstance*)ujHeapHandleLock(handle))->data;
	}
	else{
	
		ptr = cls->data;	
	}
	ptr += ofst;

	TL("  decided on offset %u into %s (cls 0x%08X o=%u cs=%u io=%u is=%u)\n",
		ofst, (flags & UJ_ACCESS_FIELD) ? "inst" : "cls", cls,
		cls->clsDataOfst, cls->clsDataSize, cls->instDataOfst, cls->instDataSize);
	if(flags & UJ_ACCESS_PUT){
	
		switch(sz){
		
			case 1:
				
				*ptr = ujThreadPrvPopInt(t);
				break;
			
			case 2:
				
				ujThreadPrvPut16(ptr, ujThreadPrvPopInt(t));
				break;
			
			case 4: 
				
				ujThreadPrvPut32(ptr, ujThreadPrvPopInt(t));
				break;
			
		#if defined(UJ_FTR_SUPPORT_LONG) || defined(UJ_FTR_SUPPORT_DOUBLE)
			case 8:
				
				///XXX: this order must mesh well with {push,pop}{long,double} and constant initializations!
				ujThreadPrvPut32(ptr + 0, ujThreadPrvPopInt(t));
				ujThreadPrvPut32(ptr + 4, ujThreadPrvPopInt(t));
				break;
		#endif
			
			default:
				return UJ_ERR_INVALID_OPCODE;
		}
		//now pop ref, if needed
		if(flags & UJ_ACCESS_FIELD) ujThreadPrvPop(t);
		TL(" class access W done on descr at pc 0x%06X with sp=%u\n",t->pc, t->spBase);
	}
	else{
		//pop ref now, if needed
		if(flags & UJ_ACCESS_FIELD) ujThreadPrvPop(t);
		
		switch(type){
			
			case JAVA_TYPE_BYTE:
			case JAVA_TYPE_BOOL:
			
				ujThreadPrvPushInt(t, (Int32)(Int8)*ptr);
				break;
			
			case JAVA_TYPE_SHORT:
				
				ujThreadPrvPushInt(t, (Int32)(Int16)ujThreadPrvGet16(ptr));
				break;
				
			case JAVA_TYPE_CHAR:
				
				ujThreadPrvPushInt(t, (UInt32)(UInt16)ujThreadPrvGet16(ptr));
				break;
			
			case JAVA_TYPE_INT:
			case JAVA_TYPE_FLOAT:
				
				ujThreadPrvPushInt(t, ujThreadPrvGet32(ptr));
				break;
			
		#if defined(UJ_FTR_SUPPORT_LONG) || defined(UJ_FTR_SUPPORT_DOUBLE)
			case JAVA_TYPE_DOUBLE:
			case JAVA_TYPE_LONG:
				
				ujThreadPrvPushInt(t, ujThreadPrvGet32(ptr + 4));
				ujThreadPrvPushInt(t, ujThreadPrvGet32(ptr + 0));
				break;
		#endif
			
			case JAVA_TYPE_ARRAY:
			case JAVA_TYPE_OBJ:
				
				ujThreadPrvPushRef(t, ujThreadPrvGet32(ptr));
				break;
			
			default:
				return UJ_ERR_INVALID_OPCODE;
		}
		TL(" class access R done at pc 0x%06X with sp=%u\n",t->pc, t->spBase);
	}
	if(handle) ujHeapHandleRelease(handle);
	return UJ_ERR_NONE;
}

static UInt8 ujThreadPrvInstanceof(UjThread* t, UInt16 descrIdx, HANDLE objHandle){

	Boolean isIface;
	UjClass* wantedCls;
	UjClass* cls;
	
	
	wantedCls = ujThreadPrvClassFromRef(t->cls, descrIdx);
	if(!wantedCls) return UJ_ERR_DEPENDENCY_MISSING;
	
	isIface = !!(ujThreadReadBE16_ex(wantedCls->info.java.readD, wantedCls->ujc ? 6 : wantedCls->info.java.interfaces - 8) & JAVA_ACC_INTERFACE);
	
	//in case of interfaces, we could have to traverse a large tree, and we shoudl do that without recursion and extra data structures. We get clever then :)
	if(isIface){
	
		//reset all marks on all classes
		cls = gFirstClass;
		while(cls){
		
			cls->mark = 0;
			cls = cls->nextClass;	
		}	
	}
	
	cls = ((UjInstance*)ujHeapHandleLock(objHandle))->cls;
	ujHeapHandleRelease(objHandle);
	
	while(cls){
		
		if(cls == wantedCls) return UJ_ERR_NONE;
		if(isIface) cls->mark = 1;
		cls = cls->supr;	
	}
	
	if(isIface){
	
		cls = gFirstClass;
		while(cls){
		
			if((cls->mark == 1) && (!cls->native)){	//only java classes implement interfaces in our case	XXX: this may change at some time
			
				UInt24 addr;
				UInt16 i, N;
				UjClass* tc = NULL;
				
				cls->mark = 2;
				addr = cls->info.java.interfaces;
				N = ujThreadReadBE16_ex(cls->info.java.readD, addr - 2);
				
				for(i = 0; i < N; i++){
				
					if(cls->ujc){
					#ifdef UJ_FTR_SUPPORT_UJC_FORMAT
						
						UjPrvStrEqualParam p;
	
						p.type = STR_EQ_PAR_TYPE_ADR;
						p.data.adr.readD = cls->info.java.readD;
						p.data.adr.addr = ujThreadReadBE24_ex(cls->info.java.readD, addr) + 1;
						
						tc = ujThreadPrvFindClass(&p);
						addr += 3;
					#endif
					}
					else{
					#ifdef UJ_FTR_SUPPORT_CLASS_FORMAT
					
						tc = ujThreadPrvClassFromRef(cls, ujThreadReadBE16_ex(cls->info.java.readD, addr));
						addr += 2;
					#endif
					}
					if(!tc) return UJ_ERR_DEPENDENCY_MISSING;
					
					tc->mark = 1;
					if(tc == wantedCls) return UJ_ERR_NONE;
				}
				cls = gFirstClass;	
			}
			else cls = cls->nextClass;	
		}	
	}
	
	return UJ_ERR_FALSE;
}

#ifdef UJ_FTR_SUPPORT_EXCEPTIONS
	static UInt8 ujThreadPrvThrow(UjThread* t, HANDLE threadH, HANDLE excH){

		do{
			UInt16 pcOfst = t->pc - t->methodStartPc;
			UInt24 addr;
			UInt16 handler, i, typ, numExcEntries;
			UInt8 ret;

			if(t->cls->ujc){
			#ifdef UJ_FTR_SUPPORT_UJC_FORMAT
				
				//step 0: addr = pointer to exception table, numExcEntries is number of entries
				addr = t->methodStartPc - 6;
				numExcEntries = ujThreadReadBE16(t, addr);
				addr -= ((UInt24)numExcEntries) << 3;
				
				//step 1: find how many locals this method has/had
				i = ujThreadReadBE16(t, t->methodStartPc - 4);
			#endif
			}
			else{
			#ifdef UJ_FTR_SUPPORT_CLASS_FORMAT
			
				//step 0: addr = pointer to exception table, numExcEntries is number of entries
				addr = t->methodStartPc + ujThreadReadBE32(t, t->methodStartPc - 4);
				numExcEntries = ujThreadReadBE16(t, addr);
				addr += 2;
				
				//step 1: find how many locals this method has/had
				i = ujThreadReadBE16(t, t->methodStartPc - 6);
			#endif
			}

			//step 1: blow away the stack
			while(t->spBase != t->localsBase + i) ujThreadPrvPop(t);

			//step 2: push the exception ref to the stack
			ujThreadPrvPushRef(t, excH);

			//step 3: check the exception table for handlers
			for(i = 0; i < numExcEntries; i++, addr += 8){

				if((UInt16)ujThreadReadBE16(t, addr) < pcOfst && (UInt16)ujThreadReadBE16(t, addr + 2) >= pcOfst){	//range match (we pass in new pc, so it's not as you'd expect)

					handler = ujThreadReadBE16(t, addr + 4);
					
					typ = ujThreadReadBE16(t, addr + 6);	//get type
					if(typ){

						ret = ujThreadPrvInstanceof(t, typ, excH);
						if(ret == UJ_ERR_FALSE) continue;	//it's not this one - try the next one
						if(ret != UJ_ERR_NONE) return ret;	//return all errors					
					}
					//if we got here, we have a match
					t->pc = handler + t->methodStartPc;
					return UJ_ERR_NONE;
				}
			}
			//if we got here, nobody in this frame caught the exception, unwind to the next frame and try again
			ujThreadPrvRet(t, threadH);

		}while(t->pc != UJ_PC_DONE);	//do not unwind past top :)

		//if we got here, we unwound the entire stack and still found no handlers. Oops...
		return UJ_ERR_USER_EXCEPTION;
	}
#endif


UInt32 ujGetNumInstrs(void){

	return gNumInstrs;
}


static UInt8 ujThreadPrvInstr(HANDLE threadH, UjThread* t){	//return success of execution

#if defined(UJ_FTR_SUPPORT_LONG) || defined(UJ_FTR_SUPPORT_DOUBLE)
	Int64 i64, v64;
#endif
#if defined(UJ_FTR_SUPPORT_FLOAT)
	UjFloat uf, uf2;
#endif
#if defined(UJ_FTR_SUPPORT_DOUBLE)
	Double64 ud, ud2;
#endif
	UInt8 ret, instr;
	UInt16 t16, v16;
	Int32 i32, v32, t32;
	HANDLE h, h2;
	UjInstance* obj;
	Boolean wide = false;

	gNumInstrs++;

instr_start:
	
	//fetch first byte of instruction and then decide what to do

	TL("Instr at pc 0x%06X in thread %u (0x%08X)\n", t->pc, threadH, t);
	
	instr = ujThreadPrvFetchClassByte(t, t->pc++);

	TL(" instr 0x%02x with sp=%u, locals=%u\n", instr, t->spBase, t->localsBase);
	
	switch(instr){
	
		case 0x00:	//nop
		
			break;
		
		case 0x01:	//aconst_null
		
			ujThreadPrvPushRef(t, 0);	//push object reference, in theory class shoudl be "object" but it's ok for now
			break;
		
		case 0x02:	//iconst_X
		case 0x03:
		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
		case 0x08:
			
			ujThreadPrvPushInt(t, ((Int8)instr) - 3);
			break;
		
		case 0x09:	//lconst_X
		case 0x0A:
		
		#if defined(UJ_FTR_SUPPORT_LONG)
			ujThreadPrvPushLong(t, u64_32_to_64(instr - 9));
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x0B:	//fconst_X
		case 0x0C:
		case 0x0D:
		
		#if defined(UJ_FTR_SUPPORT_FLOAT)
			ujThreadPrvPushFloat(t, instr - 0x0B);
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x0E:	//dconst_X
		case 0x0F:
		
		#if defined(UJ_FTR_SUPPORT_DOUBLE)
			ujThreadPrvPushDouble(t, d64_fromi(instr - 0x0E));
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x10:	//bipush
		
			ujThreadPrvPushInt(t, (Int8)ujThreadPrvFetchClassByte(t, t->pc++));
			break;
		
		case 0x11:	//sipush
		
			ujThreadPrvPushInt(t, ujThreadReadBE16(t, t->pc));
			t->pc += 2;
			break;
		
		case 0x12:	//ldc
		
			wide = true;		//clever hack)
			//fallthrough intended
		
		case 0x13:	//ldc_w
		
			v32 = ujThreadPrvReadConst32(t, ujThreadPrvGetOffset(t, !wide), &instr);
			if(instr == JAVA_CONST_TYPE_STR_REF || instr == JAVA_CONST_TYPE_STRING){
				
				ret = ujThreadPrvNewConstString(t, v32, &h);
				if(ret != UJ_ERR_NONE) goto out;
				ujThreadPrvPushRef(t, h);
			}
			else{
				
				ujThreadPrvPushInt(t, v32);
			}
			break;
		
		case 0x14:	//ldc2_w
		
		#if defined(UJ_FTR_SUPPORT_LONG) || defined (UJ_FTR_SUPPORT_DOUBLE)
			v64 = ujThreadPrvReadConst64(t, ujThreadReadBE16(t, t->pc));
			t->pc += 2;
			ujThreadPrvPushLong(t, v64);
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x15:	//iload
		case 0x17:	//fload
		
			ujThreadPrvPushInt(t, ujThreadPrvLocalLoadInt(t, ujThreadPrvGetOffset(t, wide)));
			break;
		
		case 0x16:	//lload
		case 0x18:	//dload
		
		#if defined(UJ_FTR_SUPPORT_LONG) || defined(UJ_FTR_SUPPORT_DOUBLE)
			ujThreadPrvPushLong(t, ujThreadPrvLocalLoadLong(t, ujThreadPrvGetOffset(t, wide)));
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x19:	//aload
		
			ujThreadPrvPushRef(t, ujThreadPrvLocalLoadRef(t, ujThreadPrvGetOffset(t, wide)));
			break;
		
		case 0x1A:	//iload_X
		case 0x1B:
		case 0x1C:
		case 0x1D:
		
			instr -= 0x1A;
	do_iloadX:
			ujThreadPrvPushInt(t, ujThreadPrvLocalLoadInt(t, instr));
			break;
		
		case 0x1E:	//lload_X
		case 0x1F:
		case 0x20:
		case 0x21:
		
			instr -= 0x1E;
	
		#if defined(UJ_FTR_SUPPORT_LONG) || defined(UJ_FTR_SUPPORT_DOUBLE)
	do_lloadX:
			ujThreadPrvPushLong(t, ujThreadPrvLocalLoadLong(t, instr));
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x22:	//fload_X
		case 0x23:
		case 0x24:
		case 0x25:
		
			instr -= 0x22;
			goto do_iloadX;
		
		case 0x26:	//dload_X
		case 0x27:
		case 0x28:
		case 0x29:
		
		#if defined(UJ_FTR_SUPPORT_DOUBLE)
			instr -= 0x26;
			goto do_lloadX;
		#else
			goto invalid_instr;
		#endif
		
		case 0x2A:	//aload_X
		case 0x2B:
		case 0x2C:
		case 0x2D:
		
			ujThreadPrvPushRef(t, ujThreadPrvLocalLoadRef(t, instr - 0x2A));
			break;
		
		case 0x2E:	//iaload
		case 0x30:	//faload
		
			i32 = ujThreadPrvPopInt(t);
			h = ujThreadPrvPopArrayref(t);
			ret = ujThreadPrvArrayBoundsCheck(h, i32);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvPushInt(t, ujThreadPrvArrayGetInt(h, i32));
			break;
		
		case 0x2F:	//laload
		case 0x31:	//daload
		
		#if defined(UJ_FTR_SUPPORT_LONG) || defined(UJ_FTR_SUPPORT_DOUBLE)
			i32 = ujThreadPrvPopInt(t);
			h = ujThreadPrvPopArrayref(t);
			ret = ujThreadPrvArrayBoundsCheck(h, i32);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvPushLong(t, ujThreadPrvArrayGetLong(h, i32));
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x32:	//aaload
		
			i32 = ujThreadPrvPopInt(t);
			h = ujThreadPrvPopArrayref(t);
			ret = ujThreadPrvArrayBoundsCheck(h, i32);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvPushRef(t, ujThreadPrvArrayGetRef(h, i32));
			break;
		
		case 0x33:	//baload
		
			i32 = ujThreadPrvPopInt(t);
			h = ujThreadPrvPopArrayref(t);
			ret = ujThreadPrvArrayBoundsCheck(h, i32);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvPushInt(t, ujThreadPrvArrayGetByte(h, i32));
			break;
		
		case 0x34:	//caload
		
			i32 = ujThreadPrvPopInt(t);
			h = ujThreadPrvPopArrayref(t);
			ret = ujThreadPrvArrayBoundsCheck(h, i32);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvPushInt(t, ujThreadPrvArrayGetChar(h, i32));
			break;
		
		case 0x35:	//saload
		
			i32 = ujThreadPrvPopInt(t);
			h = ujThreadPrvPopArrayref(t);
			ret = ujThreadPrvArrayBoundsCheck(h, i32);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvPushInt(t, ujThreadPrvArrayGetShort(h, i32));
			break;
		
		case 0x36:	//istore
		case 0x38:	//fstore
		
			ujThreadPrvLocalStoreInt(t, ujThreadPrvGetOffset(t, wide), ujThreadPrvPopInt(t));
			break;
		
		case 0x37:	//lstore
		case 0x39:	//dstore
		
		#if defined(UJ_FTR_SUPPORT_LONG) || defined(UJ_FTR_SUPPORT_DOUBLE)
			ujThreadPrvLocalStoreLong(t, ujThreadPrvGetOffset(t, wide), ujThreadPrvPopLong(t));
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x3A:	//astore
		
			ujThreadPrvLocalStoreRef(t, ujThreadPrvGetOffset(t, wide), ujThreadPrvPopRef(t));
			break;
		
		case 0x3B:	//istore_X
		case 0x3C:
		case 0x3D:
		case 0x3E:
			
			instr -= 0x3B;
			
	do_istore:
			ujThreadPrvLocalStoreInt(t, instr, ujThreadPrvPopInt(t));
			break;
		
		case 0x3F:	//lstore_X
		case 0x40:
		case 0x41:
		case 0x42:
		
			instr -= 0x3F;
			
		#if defined(UJ_FTR_SUPPORT_LONG) || defined(UJ_FTR_SUPPORT_DOUBLE)
	do_lstore:
			ujThreadPrvLocalStoreLong(t, instr, ujThreadPrvPopLong(t));
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x43:	//fstore_X
		case 0x44:
		case 0x45:
		case 0x46:
		
			instr -= 0x43;
			goto do_istore;
		
		case 0x47:	//dstore_X
		case 0x48:
		case 0x49:
		case 0x4A:
		#if defined(UJ_FTR_SUPPORT_DOUBLE)
			instr -= 0x47;
			goto do_lstore;
		#else
			goto invalid_instr;
		#endif
			
		case 0x4B:	//astore_X
		case 0x4C:
		case 0x4D:
		case 0x4E:
		
			ujThreadPrvLocalStoreRef(t, instr - 0x4B, ujThreadPrvPopRef(t));
			break;
		
		case 0x4F:	//iastore
		case 0x51:	//fastore
			
			v32 = ujThreadPrvPopInt(t);
			i32 = ujThreadPrvPopInt(t);
			h = ujThreadPrvPopArrayref(t);
			ret = ujThreadPrvArrayBoundsCheck(h, i32);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvArraySetInt(h, i32, v32);
			break;
		
		case 0x50:	//lastore
		case 0x52:	//dastore
			
		#if defined(UJ_FTR_SUPPORT_LONG) || defined(UJ_FTR_SUPPORT_DOUBLE)
			i64 = ujThreadPrvPopLong(t);
			i32 = ujThreadPrvPopInt(t);
			h = ujThreadPrvPopArrayref(t);
			ret = ujThreadPrvArrayBoundsCheck(h, i32);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvArraySetLong(h, i32, i64);
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x53:	//aastore
			
			h2 = ujThreadPrvPopRef(t);
			i32 = ujThreadPrvPopInt(t);
			h = ujThreadPrvPopArrayref(t);
			ret = ujThreadPrvArrayBoundsCheck(h, i32);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvArraySetRef(h, i32, h2);
			break;
		
		case 0x54:	//bastore
			
			instr = ujThreadPrvPopInt(t);
			i32 = ujThreadPrvPopInt(t);
			h = ujThreadPrvPopArrayref(t);
			ret = ujThreadPrvArrayBoundsCheck(h, i32);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvArraySetByte(h, i32, (Int8)instr);
			break;
		
		case 0x55:	//castore
			
			t16 = ujThreadPrvPopInt(t);
			i32 = ujThreadPrvPopInt(t);
			h = ujThreadPrvPopArrayref(t);
			ret = ujThreadPrvArrayBoundsCheck(h, i32);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvArraySetChar(h, i32, t16);
			break;
		
		case 0x56:	//sastore
			
			t16 = ujThreadPrvPopInt(t);
			i32 = ujThreadPrvPopInt(t);
			h = ujThreadPrvPopArrayref(t);
			ret = ujThreadPrvArrayBoundsCheck(h, i32);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvArraySetShort(h, i32, (Int16)t16);
			break;
		
		case 0x57:	//pop
		
			ujThreadPrvPopInt(t);
			break;
		
		case 0x58:	//pop2
		
			ujThreadPrvPopInt(t);
			ujThreadPrvPopInt(t);
			break;
		
		case 0x59:	//dup
		
			if(!ujThreadPrvDup(t, 1, 0)) {
				
				ret = UJ_ERR_STACK_SPACE;
				goto out;
			}
			break;
		
		case 0x5A:	//dup_x1
		
			if(!ujThreadPrvDup(t, 1, 1)) {
				
				ret = UJ_ERR_STACK_SPACE;
				goto out;
			}
			break;
		
		case 0x5B:	//dup_x2
			
			if(!ujThreadPrvDup(t, 1, 2)) {
				
				ret = UJ_ERR_STACK_SPACE;
				goto out;
			}
			break;
		
		case 0x5C:	//dup2
		
			if(!ujThreadPrvDup(t, 2, 0)) {
				
				ret = UJ_ERR_STACK_SPACE;
				goto out;
			}
			break;
		
		case 0x5D:	//dup2_x1
			
			if(!ujThreadPrvDup(t, 2, 1)) {
				
				ret = UJ_ERR_STACK_SPACE;
				goto out;
			}
			break;
		
		case 0x5E:	//dup2_x2
			
			if(!ujThreadPrvDup(t, 2, 2)) {
				
				ret = UJ_ERR_STACK_SPACE;
				goto out;
			}
			break;
		
		case 0x5F:	//swap
			
			//quick hack :)
			if(!ujThreadPrvDup(t, 1, 1)) {
				
				ret = UJ_ERR_STACK_SPACE;
				goto out;
			}
			ujThreadPrvPopInt(t);
			break;
		
		case 0x60:	//i{add,sub,mul,div,rem,neg}
		case 0x64:
		case 0x68:
		case 0x6C:
		case 0x70:
		case 0x74:
			
			instr = (instr - 0x60) >> 2;		
			v32 = ujThreadPrvPopInt(t);
			i32 = 0;
			if(instr != 5) i32 = ujThreadPrvPopInt(t);
			switch(instr){
				
				case 0:		//add
					
					i32 += v32;
					break;
				
				case 1:		//sub
					
					i32 -= v32;
					break;
				
				case 2:		//mul
					
					i32 *= v32;
					break;
				
				case 3:		//div
					
					if(!v32){
						
						ret = UJ_ERR_DIV_BY_ZERO;
						goto out;
					}
					i32 /= v32;
					break;
				
				case 4:		//rem
					
					if(!v32){
						
						ret = UJ_ERR_DIV_BY_ZERO;
						goto out;
					}
					i32 %= v32;
					break;
				
				case 5:		//neg
					
					i32 = -v32;
					break;
			}
			ujThreadPrvPushInt(t, i32);
			break;
	
		case 0x61:	//l{add,sub,mul,div,rem,neg}
		case 0x65:
		case 0x69:
		case 0x6D:
		case 0x71:
		case 0x75:
			
		#if defined(UJ_FTR_SUPPORT_LONG)	
			instr = (instr - 0x61) >> 2;		
			v64 = ujThreadPrvPopLong(t);
			if(instr != 5) i64 = ujThreadPrvPopLong(t);
			switch(instr){
				
				case 0:		//add
					
					i64 = u64_add(i64, v64);
					break;
				
				case 1:		//sub
					
					i64 = u64_sub(i64, v64);
					break;
				
				case 2:		//mul
					
					i64 = u64_mul(i64, v64);
					break;
				
				case 3:		//div
					
					if(u64_isZero(v64)){
						
						ret = UJ_ERR_DIV_BY_ZERO;
						goto out;
					}
					i64 = i64_div(i64, v64);
					break;
				
				case 4:		//rem
					
					if(u64_isZero(v64)){
						
						ret = UJ_ERR_DIV_BY_ZERO;
						goto out;
					}
					i64 = i64_mod(i64, v64);
					break;
				
				case 5:		//neg
					
					i64 = u64_sub(u64_zero(), v64);
					break;
			}
			ujThreadPrvPushLong(t, i64);
		#else
			goto invalid_instr;
		#endif
			break;
	
		case 0x62:	//f{add,sub,mul,div,rem,neg}
		case 0x66:
		case 0x6A:
		case 0x6E:
		case 0x72:
		case 0x76:
			
		#if defined(UJ_FTR_SUPPORT_FLOAT)	
			instr = (instr - 0x62) >> 2;		
			uf = ujThreadPrvPopFloat(t);
			if(instr != 5) uf2 = ujThreadPrvPopFloat(t);
			switch(instr){
				
				case 0:		//add
					
					uf2 += uf;
					break;
				
				case 1:		//sub
					
					uf2 -= uf;
					break;
				
				case 2:		//mul
					
					uf2 *= uf;
					break;
				
				case 3:		//div
					
					uf2 /= uf;
					break;
				
				case 4:		//rem
					
					if(uf == 0 || uf2 / 2.0 == uf2) uf2 = NAN;
					else if(uf / 2.0 == uf) /* uf2 is already the result */;
					else{
					
						uf2 = uf2 - floorf(uf2 / uf) * uf;
					}
					break;
				
				case 5:		//neg
					
					uf2 = -uf;
					break;
			}
			ujThreadPrvPushFloat(t, uf2);
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x63:	//d{add,sub,mul,div,rem,neg}
		case 0x67:
		case 0x6B:
		case 0x6F:
		case 0x73:
		case 0x77:
			
		#if defined(UJ_FTR_SUPPORT_DOUBLE)	
			instr = (instr - 0x63) >> 2;
			ud = ujThreadPrvPopDouble(t);
			if(instr != 5) ud2 = ujThreadPrvPopDouble(t);
			switch(instr){
				
				case 0:		//add
					
					ud2 = d64_add(ud2, ud);
					break;
				
				case 1:		//sub
					
					ud2 = d64_sub(ud2, ud);
					break;
				
				case 2:		//mul
					
					ud2 = d64_mul(ud2, ud);
					break;
				
				case 3:		//div
					
					ud2 = d64_div(ud2, ud);
					break;
				
				case 4:		//rem
					
					if(d64_isZero(ud) || d64_isinf(ud2)) ud2 = d64_nan();
					else if(d64_isinf(ud)) /* ud2 is already the result */;
					else{
					
						ud2 = d64_sub(ud2, d64_mul(d64_floor(d64_div(ud2, ud)), ud));
					}
					break;
				
				case 5:		//neg
					
					ud2 = d64_neg(ud);
					break;
			}
			ujThreadPrvPushDouble(t, ud2);
		#else
			goto invalid_instr;
		#endif
			break;	
		
		case 0x78:	//i{shl,shr,ushr,and,or,xor}
		case 0x7A:
		case 0x7C:
		case 0x7E:
		case 0x80:
		case 0x82:
			
			instr = (instr - 0x78) >> 1;		
			v32 = ujThreadPrvPopInt(t);
			i32 = ujThreadPrvPopInt(t);
			switch(instr){
				
				case 0:	//shl
					
					i32 <<= v32;
					break;
				
				case 1:	//shr
					
					i32 >>= v32;
					break;
				
				case 2:	//ushr
				
					i32 = ((UInt32)i32) >> ((UInt32)v32);
					break;
				
				case 3:	//and
					
					i32 &= v32;
					break;
				
				case 4:	//or
					
					i32 |= v32;
					break;
				
				case 5:	//xor
				
					i32 ^= v32;
					break;
			}
			ujThreadPrvPushInt(t, i32);
			break;
		
		case 0x79:	//l{shl,shr,ushr,and,or,xor}
		case 0x7B:
		case 0x7D:
		case 0x7F:
		case 0x81:
		case 0x83:
			
		#if defined(UJ_FTR_SUPPORT_LONG)	
			instr = (instr - 0x79) >> 1;		
			v64 = ujThreadPrvPopLong(t);
			i64 = ujThreadPrvPopLong(t);
			switch(instr){
				
				case 0:	//shl
					
					i64 = u64_shl(i64, u64_64_to_32(v64));
					break;
				
				case 1:	//shr
					
					i64 = u64_ashr(i64, u64_64_to_32(v64));
					break;
				
				case 2:	//ushr
				
					i64 = u64_shr(i64, u64_64_to_32(v64));
					break;
				
				case 3:	//and
					
					i64 = u64_and(i64, v64);
					break;
				
				case 4:	//or
					
					i64 = u64_orr(i64, v64);
					break;
				
				case 5:	//xor
				
					i64 = u64_xor(i64, v64);
					break;
			}
			ujThreadPrvPushLong(t, i64);
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x84:	//iinc
	
			t16 = ujThreadPrvGetOffset(t, wide);	//index
			
			if(wide){
				
				v16 = ujThreadReadBE16(t, t->pc);
				t->pc += 2;
			}
			else{
			
				v16 = (Int16)(Int8)ujThreadPrvFetchClassByte(t, t->pc++);
			}
			ujThreadPrvLocalStoreInt(t, t16, ujThreadPrvLocalLoadInt(t, t16) + (Int16)v16);
			break;
		
		case 0x85:	//i2l
			
		#if defined(UJ_FTR_SUPPORT_LONG)	
			ujThreadPrvPushLong(t, i64_xtnd32(u64_32_to_64(ujThreadPrvPopInt(t))));
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x86:	//i2f
			
		#if defined(UJ_FTR_SUPPORT_FLOAT)	
			ujThreadPrvPushFloat(t, (UjFloat)ujThreadPrvPopInt(t));
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x87:	//i2d
		
		#if defined(UJ_FTR_SUPPORT_DOUBLE)
			ujThreadPrvPushDouble(t, d64_fromi(ujThreadPrvPopInt(t)));
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x88:	//l2i
			
		#if defined(UJ_FTR_SUPPORT_LONG)	
			ujThreadPrvPushInt(t, u64_64_to_32(ujThreadPrvPopLong(t)));
		#else
			goto invalid_instr;
		#endif
			break;
	
		case 0x89:	//l2f
			
		#if defined(UJ_FTR_SUPPORT_LONG) && defined(UJ_FTR_SUPPORT_FLOAT)
		
			i64 = ujThreadPrvPopLong(t);
			if(i64_isNeg(i64)){
				
				i64 = u64_sub(u64_zero(), i64);
				instr = 0;
			}
			uf = (UjFloat)u64_get_hi(i64);
			uf *= 4294967296.0F;
			uf += (UjFloat)u64_64_to_32(i64);
			if(!instr) uf = -uf;

			ujThreadPrvPushFloat(t, uf);
		#else
			goto invalid_instr;
		#endif
			break;
	
		case 0x8A:	//l2d
			
		#if defined(UJ_FTR_SUPPORT_DOUBLE)
			ujThreadPrvPushDouble(t, d64_froml(ujThreadPrvPopLong(t)));
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x8B:	//f2i
			
		#if defined(UJ_FTR_SUPPORT_FLOAT)
			
			uf = ujThreadPrvPopFloat(t);
			if(isnan(uf)) i32 = 0;		//NaN
			else if(uf >= 2147483647.0) i32 = 0x7FFFFFFF;
			else if(uf <= -2147483648.0) i32 = 0x80000000;
			else i32 = uf;
			ujThreadPrvPushInt(t, i32);
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x8C:	//f2l
		
		#if defined(UJ_FTR_SUPPORT_FLOAT) && defined(UJ_FTR_SUPPORT_LONG)
		
			uf = ujThreadPrvPopFloat(t);
			if(isnan(uf)) i64 = u64_zero();	//NaN
			else if(uf >= 9223372036854775807.0) i64 = u64_from_halves(0x7FFFFFFF, 0xFFFFFFFF);
			else if(uf <= -9223372036854775807.0) i64 = u64_from_halves(0x80000000, 0x00000000);
			else{
				if(uf < 0){
					instr = 0;
					uf = -uf;
				}
			
				uf2 = i32 = (Int32)(uf / 4294967296.0);
				uf -= uf2 * 4294967296.0;
				
				i64 = u64_from_halves(i32, (UInt32)uf);
				if(!instr) i64 = u64_sub(u64_zero(), i64);
			}
			ujThreadPrvPushLong(t, i64);
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x8D:	//f2d
		
		#if defined(UJ_FTR_SUPPORT_FLOAT) && defined(UJ_FTR_SUPPORT_DOUBLE)
			ujThreadPrvPushDouble(t, d64_fromf(ujThreadPrvPopFloat(t)));
		#else
			goto invalid_instr;
		#endif
			break;
			
		case 0x8E:	//d2i
			
		#if defined(UJ_FTR_SUPPORT_DOUBLE)
			ujThreadPrvPushInt(t, d64_toi(ujThreadPrvPopDouble(t)));
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x8F:	//d2l
			
		#if defined(UJ_FTR_SUPPORT_DOUBLE)
			 ujThreadPrvPushLong(t, d64_tol(ujThreadPrvPopDouble(t)));
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x90:	//d2f
		
		#if defined(UJ_FTR_SUPPORT_FLOAT) && defined(UJ_FTR_SUPPORT_DOUBLE)
			ujThreadPrvPushFloat(t, d64_tof(ujThreadPrvPopDouble(t)));
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x91:	//i2b
			
			ujThreadPrvPushInt(t, (Int8)ujThreadPrvPopInt(t));
			break;
		
		case 0x92:	//i2c
		
			ujThreadPrvPushInt(t, (UInt32)(UInt16)ujThreadPrvPopInt(t));
			break;
		
		case 0x93:	//i2s
		
			ujThreadPrvPushInt(t, (Int16)ujThreadPrvPopInt(t));
			break;
		
		case 0x94:	//lcmp
			
		#if defined(UJ_FTR_SUPPORT_LONG)	
			i64 = ujThreadPrvPopLong(t);
			v64 = ujThreadPrvPopLong(t);
			i64 = u64_sub(i64, v64);
			if(i64_isNeg(i64)) i32 = 1;
			else if(u64_isZero(i64)) i32 = 0;
			else i32 = -1;
			ujThreadPrvPushInt(t, i32);
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x95:	//fcmpl
		case 0x96:	//fcmpg
		
		#if defined(UJ_FTR_SUPPORT_FLOAT)	
			uf = ujThreadPrvPopFloat(t);
			uf2 = ujThreadPrvPopFloat(t);
			
			if(uf2 < uf) i32 = -1;
			else if(uf2 == uf) i32 = 0;
			else if(uf2 > uf) i32 = 1;
			else if(instr == 0x96) i32 = 1;	//NaN handling for fcmpg
			else i32 = -1;			//NaN handling for fcmpl
			ujThreadPrvPushInt(t, i32);
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x97:	//dcmpl
		case 0x98:	//dcmpg
		
		#if defined(UJ_FTR_SUPPORT_DOUBLE)	
			ud = ujThreadPrvPopDouble(t);
			ud2 = ujThreadPrvPopDouble(t);
			
			ud2 = d64_sub(ud2, ud);
			
			if(d64_isnan(ud) || d64_isnan(ud2)){			//NaN handling for dcmpl
				
				i32 = (instr == 0x98) ? 1 : -1;
			}
			else if(d64_isZero(ud2)) i32 = 0;
			else if(d64_isNeg(ud2)) i32 = -1;
			else i32 = 1;
			ujThreadPrvPushInt(t, i32);
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0x99:	//ifeq
			
			ujThreadPrvJumpIfNeeded(t, ujThreadPrvPopInt(t) == 0);
			break;
		
		case 0x9A:	//ifne
			
			ujThreadPrvJumpIfNeeded(t, ujThreadPrvPopInt(t) != 0);
			break;
		
		case 0x9B:	//iflt
			
			ujThreadPrvJumpIfNeeded(t, ujThreadPrvPopInt(t) < 0);
			break;
			
		case 0x9C:	//ifge
			
			ujThreadPrvJumpIfNeeded(t, ujThreadPrvPopInt(t) >= 0);
			break;
		
		case 0x9D:	//ifgt
			
			ujThreadPrvJumpIfNeeded(t, ujThreadPrvPopInt(t) > 0);
			break;
			
		case 0x9E:	//ifle
			
			ujThreadPrvJumpIfNeeded(t, ujThreadPrvPopInt(t) <= 0);
			break;
		
		case 0x9F:	//if_icmpeq
			
			v32 = ujThreadPrvPopInt(t);
			i32 = ujThreadPrvPopInt(t);
			ujThreadPrvJumpIfNeeded(t, i32 == v32);
			break;
		
		case 0xA0:	//if_icmpne
			
			v32 = ujThreadPrvPopInt(t);
			i32 = ujThreadPrvPopInt(t);
			ujThreadPrvJumpIfNeeded(t, i32 != v32);
			break;
		
		case 0xA1:	//if_icmplt
			
			v32 = ujThreadPrvPopInt(t);
			i32 = ujThreadPrvPopInt(t);
			ujThreadPrvJumpIfNeeded(t, i32 < v32);
			break;
		
		case 0xA2:	//if_icmpge
			
			v32 = ujThreadPrvPopInt(t);
			i32 = ujThreadPrvPopInt(t);
			ujThreadPrvJumpIfNeeded(t, i32 >= v32);
			break;
		
		case 0xA3:	//if_icmpgt
			
			v32 = ujThreadPrvPopInt(t);
			i32 = ujThreadPrvPopInt(t);
			ujThreadPrvJumpIfNeeded(t, i32 > v32);
			break;
		
		case 0xA4:	//if_icmple
			
			v32 = ujThreadPrvPopInt(t);
			i32 = ujThreadPrvPopInt(t);
			ujThreadPrvJumpIfNeeded(t, i32 <= v32);
			break;
		
		case 0xA5:	//if_acmpeq
			
			ujThreadPrvJumpIfNeeded(t, ujThreadPrvPopRef(t) == ujThreadPrvPopRef(t));
			break;
		
		case 0xA6:	//if_acmpne
			
			ujThreadPrvJumpIfNeeded(t, ujThreadPrvPopRef(t) == ujThreadPrvPopRef(t));
			break;
		
		case 0xA7:	//goto
		
			ujThreadPrvJumpIfNeeded(t,  true);
			break;
		
		case 0xA8:	//jsr
			
			ujThreadPrvPushInt(t, t->pc + 2);
			ujThreadPrvJumpIfNeeded(t,  true);
			break;
		
		case 0xA9:	//ret
			
			t->pc = ujThreadPrvLocalLoadInt(t, ujThreadPrvGetOffset(t, wide));
			break;
		
		case 0xAA:	//tableswitch
			
			instr = (t->pc - t->methodStartPc) & 3;	//calculate number of padding bytes
			if(instr) instr = 4 - instr;

			//get jump index off the stack
			v32 = ujThreadPrvPopInt(t);
			
			//read low range value
			i32 = ujThreadReadBE32(t, t->pc + instr + 4);
			
			//read high range value
			if(i32 <= v32 && v32 <= ujThreadReadBE32(t, t->pc + instr + 8)){//in range? calculate the offset of the jump destination that we need
			
				v32 -= i32;
				v32 <<= 2;	//make the offset value
				v32 += 8;
			}
			else v32 = 0;		//default: use default offset
			
			//read jump offset 
			i32 = ujThreadReadBE32(t, t->pc + instr + v32);
			
			t->pc += i32 - 1;	//do the jump
			break;
		
		case 0xAB:	//lookupswitch
			
			instr = (t->pc - t->methodStartPc) & 3;	//calculate number of padding bytes
			if(instr) instr = 4 - instr;

			//get jump index off the stack
			v32 = ujThreadPrvPopInt(t);
			
			//read num pairs
			i32 = ujThreadReadBE32(t, t->pc + instr + 4);
			
			//now go through pairs and read each to see if we have a match
			i32 <<= 3;
			for(t32 = 0; t32 != i32; t32 += 8){
			
				if(ujThreadPrvFetchClassByte(t, t->pc + instr + 8 + t32) == v32){	//key matches
				
					break;	
				}	
			}
			
			if(t32 == i32)	t32 = 0;	//get the offset
			else t32 += 12;
			
			//read jump offset 
			i32 = ujThreadReadBE32(t, t->pc + instr + t32);
			
			t->pc += i32 - 1;	//do the jump
			break;
		
		case 0xAC:	//ireturn
		case 0xAE:	//freturn
		
			i32 = ujThreadPrvPopInt(t);
			ret = ujThreadPrvRet(t, threadH);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvPushInt(t, i32);
			break;
		
		case 0xAD:	//lreturn
		case 0xAF:	//dreturn
		
		#if defined(UJ_FTR_SUPPORT_LONG) || defined(UJ_FTR_SUPPORT_DOUBLE)
			i64 = ujThreadPrvPopLong(t);
			ret = ujThreadPrvRet(t, threadH);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvPushLong(t, i64);
		#else
			goto invalid_instr;
		#endif
			break;
		
		case 0xB0:	//areturn
		
			h = ujThreadPrvPopRef(t);
			ret = ujThreadPrvRet(t, threadH);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvPushRef(t, h);
			break;
		
		case 0xB1:	//return
		
			ret = ujThreadPrvRet(t, threadH);
			if(ret != UJ_ERR_NONE) goto out;
			break;
		
		case 0xB2:	//getstatic
		case 0xB3:	//putstatic
		case 0xB4:	//getfield
		case 0xB5:	//putfield
			
			instr -= 0xB2;
			ret = 0;
			
		#if defined(UJ_FTR_SUPPORT_UJC_FORMAT)	
			if(wide){	//shortcut access for (*current* class)/(instance of *current* class)
				
				ret = ujThreadPrvFetchClassByte(t, t->pc++);
			}
		#endif
			ret = ujThreadPrvAccessClass(t, ret, ujThreadReadBE16(t, t->pc), instr);
			t->pc += 2;
			if(ret != UJ_ERR_NONE) goto out;
			break;
		
		case 0xB6:	//invokevirtual
		case 0xB7:	//invokespecial
		case 0xB8:	//invokestatic
		case 0xB9:	//invokeinterface
		
			ret = 0;
			instr -= 0xB6;
			t32 = t->pc - 1;
			
		#if defined(UJ_FTR_SUPPORT_UJC_FORMAT)	
			if(wide){	//shortcut access for (*current* class)/(instance of *current* class) invokes
				
				ret = ujThreadPrvFetchClassByte(t, t->pc++);
				t32--;	//to re-capture the "wide" instr
			}
		#endif
			
			ret = ujThreadPrvInvoke(t, threadH, ret, instr + UJ_INVOKE_VIRTUAL, (instr + UJ_INVOKE_VIRTUAL == UJ_INVOKE_INTERFACE && !ret) ? 4 : 2);
			if(ret == UJ_ERR_RETRY_LATER){
				
				t->pc = t32;
				goto out;
			}
			else if(ret != UJ_ERR_NONE) goto out;
			break;
		
		case 0xBA:	//invokedynamic
		
			goto invalid_instr;	//not used in Java, other languages compiled to JVM may, but we don't care
			break;
		
		case 0xBB:	//new
		
			ret = ujThreadPrvNewObj(t, ujThreadReadBE16(t, t->pc), &h);
			if(ret != UJ_ERR_NONE) goto out;
			t->pc += 2;
			ujThreadPrvPushRef(t, h);
			break;
		
		case 0xBC:	//newarray
			
			instr = ujThreadPrvFetchClassByte(t, t->pc++);
			if(instr < JAVA_ATYPE_FIRST) goto invalid_instr;	//invalid type
			if(instr > JAVA_ATYPE_LAST) goto invalid_instr;		//invalid type
			
			ret = ujThreadPrvNewArray(ujPrvAtypeToStrType[instr - JAVA_ATYPE_FIRST], ujThreadPrvPopInt(t), &h);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvPushRef(t, h);
			break;
		
		case 0xBD:	//anewarray
			
			t->pc += 2;	//we do not use the type :)
			ret = ujThreadPrvNewArray(JAVA_TYPE_OBJ, ujThreadPrvPopInt(t), &h);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvPushRef(t, h);
			break;
		
		case 0xBE:	//arraylength
		
			h = (HANDLE)ujThreadPrvPopArrayref(t);
			if(!h){
				ret = UJ_ERR_NULL_POINTER;
				goto out;
			}
			ujThreadPrvPushInt(t, ujThreadPrvArrayGetLength(h));
			break;
		
		case 0xBF:	//athrow
		
			h = ujThreadPrvPopRef(t);
			#ifdef UJ_FTR_SUPPORT_EXCEPTIONS
				if(!h){
					ret = UJ_ERR_NULL_POINTER;
					goto out;
				}
				ret = ujThreadPrvThrow(t, threadH, h);
				if(ret) goto out;
			#else
			
				ret = UJ_ERR_USER_EXCEPTION;
				goto out;
			#endif
			break;
		
		case 0xC0:	//checkcast
		
			h = ujThreadPrvPopRef(t);
			t16 = ujThreadReadBE16(t, t->pc);
			t->pc += 2;
			ret = ujThreadPrvInstanceof(t, t16, h);
			if(ret == UJ_ERR_FALSE){
				ret = UJ_ERR_INVALID_CAST;
				goto out;
			}
			else if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvPushRef(t, h);
			break;
		
		case 0xC1:	//instanceof
		
			h = ujThreadPrvPopRef(t);
			t16 = ujThreadReadBE16(t, t->pc);
			t->pc += 2;
			if(h){
				
				ret = ujThreadPrvInstanceof(t, t16, h);
				if(ret == UJ_ERR_FALSE) v32 = 0;
				else if(ret != UJ_ERR_NONE) goto out;
				else v32 = 1;
			}
			else v32 = 0;
			ujThreadPrvPushInt(t, v32);
			break;
		
		
		case 0xC2:	//monitorenter
			
			h = ujThreadPrvPopRef(t);
			if(!h){
				ret = UJ_ERR_NULL_POINTER;
				goto out;
			}
			#ifdef UJ_FTR_SYNCHRONIZATION
				obj = (UjInstance*)ujHeapHandleLock(h);
				
				if(!ujThreadPrvMonEnter(threadH, &obj->mon)){	//fail
				
					ujThreadPrvPushRef(t, h);	//re-push the object for later
					t->pc--;			//re-execute this instr later	
				}
				ujHeapHandleRelease(h);
			#endif
			break;
		
		case 0xC3:	//monitorexit
		
			h = ujThreadPrvPopRef(t);
			if(!h){
				ret = UJ_ERR_NULL_POINTER;
				goto out;
			}
			#ifdef UJ_FTR_SYNCHRONIZATION
				obj = (UjInstance*)ujHeapHandleLock(h);
				ret = ujThreadPrvMonExit(threadH, &obj->mon);
				ujHeapHandleRelease(h);
				if(!ret) goto out;
			#endif
			break;
			
		case 0xC4:	//wide
		
			wide = true;
			goto instr_start;
			
		case 0xC5:	//multianewarray
		
			v16 = ujThreadReadBE16(t, t->pc);		//get index to type
			t->pc += 2;
			ret = ujThreadPrvHandleMultiNewArray(t, v16, ujThreadPrvFetchClassByte(t, t->pc++), &h);
			if(ret != UJ_ERR_NONE) goto out;
			ujThreadPrvPushRef(t, h);
			break;
			
		case 0xC6:	//ifnull
			
			ujThreadPrvJumpIfNeeded(t, ujThreadPrvPopRef(t) == 0);
			break;
		
		case 0xC7:	//ifnonnull
			
			ujThreadPrvJumpIfNeeded(t, ujThreadPrvPopRef(t) != 0);
			break;
		
		case 0xC8:	//goto_w
			
			i32 = ujThreadReadBE32(t, t->pc);
			t->pc += i32 - 1;
			break;
		
		case 0xC9:	//jsr_w
			
			ujThreadPrvPushInt(t, t->pc + 4);
			i32 = ujThreadReadBE32(t, t->pc);
			t->pc += i32 - 1;
			break;
		
		case 0xFE:	//load const from code
			
			#ifdef UJ_FTR_SUPPORT_UJC_FORMAT
			
				ujThreadPrvPushInt(t,ujThreadReadBE32(t, t->pc));
				t->pc += 4;
			
			#else
				goto invalid_instr;
			#endif
			break;
		
		default:
			
			goto invalid_instr;		
	}
	
	ret = UJ_ERR_NONE;
	
out:
	
	return ret;
	
invalid_instr:
	
	return UJ_ERR_INVALID_OPCODE;
}

UInt8 ujInstr(void){		//return UJ_ERR_*
	
	HANDLE h;
	UjThread* t;
	UInt8 ret, i;
	Boolean died;
	
	t = ujHeapHandleLock(h = gCurThread);

	for(i = 0; i < UJ_THREAD_QUANTUM; i++){
		
		ret = ujThreadPrvInstr(h, t);
	
		if(ret == UJ_ERR_RETRY_LATER){
		
			ret = UJ_ERR_NONE;		//do not bother with the rest of time quantum if we're already stuck
			break;
		}
		died = (t->pc == UJ_PC_DONE);
		
		if(died) break;
		if(ret != UJ_ERR_NONE) break;
	}

	gCurThread = t->nextThread;
	ujHeapHandleRelease(h);
	if(!gCurThread) gCurThread = gFirstThread;
	
	if(died) ujThreadDestroy(h);
	
	return ret;
}	

UInt8 ujThreadDestroy(HANDLE threadH){

	
	HANDLE* handleP = &gFirstThread;
	HANDLE handleToUnlock = 0;
	UInt8 ret = UJ_ERR_INTERNAL;
	
	while(*handleP && *handleP != threadH){
	
		if(handleToUnlock) ujHeapHandleRelease(handleToUnlock);
		handleP = &((UjThread*)ujHeapHandleLock(handleToUnlock = *handleP))->nextThread;
	}
	
	if(*handleP){	//found it
		
		
		*handleP = ((UjThread*)ujHeapHandleLock(threadH))->nextThread;
		ujHeapHandleRelease(threadH);
		ujHeapHandleFree(threadH);
		ret = UJ_ERR_NONE;
	}
	
	if(handleToUnlock) ujHeapHandleRelease(handleToUnlock);
	return ret;
}

UInt8 ujInit(UjClass** objectClsP){
	
	gNumInstrs = 0;
	gFirstThread = 0;
	gFirstClass = NULL;
	ujHeapInit();
	return ujInitBuiltinClasses(objectClsP);
}

UInt8 ujInitAllClasses(void){

	HANDLE threadH = 0;
	UjClass* cls = gFirstClass;
	UInt8 ret;
	
	
	while(cls){
	
		if(!threadH) threadH = ujThreadCreate(0);
		if(!threadH) return UJ_ERR_OUT_OF_MEMORY;
		ret = ujThreadGoto(threadH, cls, "<clinit>", "()V");
		
		if(ret == UJ_ERR_METHOD_NONEXISTENT){
		
			//nothing to do here	
		}
		else if(ret != UJ_ERR_NONE){
		
			return ret;	
		}
		else{
		
			while(ujCanRun()) ujInstr();
			threadH = 0;
		}
		
		cls = cls->nextClass;
	}
	
	if(threadH) ujThreadDestroy(threadH);
	return UJ_ERR_NONE;
}

/*
	we hate recursion and we hate creating new data structures since they use space.
	This is an extreme case of space-optimization: we choose an O(n^3) [or worse]
	algorithm over a O(n^2) one because the former can run in no extra memory. Clever,
	huh? the speed gods will hate us forever... So what's the story? Well, each object
	in the managed heap is always in one of two states: locked or unlocked. Locked
	state means that native code has a pointer to it and it thus cannot be moved.
	Obviously we cnanot GC those. Unlocked chunks are now subdivided into three groups:
	(0) those we haven;t yet seen in our object tree walk, (1) Those we have seen but
	took no further action yet, and (2) those we'se seen and whose children we've
	iterated over. Is it becoming clearer yet? to GC we: (0) Set the "mark" value on
	all unlocked chunks to 0; (1) Walk the stacks, classes, etc and mark all object
	references we see to a mark value of 1; (2) We now repeat step 3 until an iteration
	of it produces no changes; (3) Find the first object marked with mark value 1,
	mark it to value 2, walk all of its children objects, and mark them all to mark
	value 1 if they were at zero. As you can see after this we'll have a heap full of
	objects at either mark level 2 or zero. All objects marked at zero are available
	to be deleted, since we have seen no references to them. Why is this slow? The
	step where we find the "first object marked with a 1" is O(n), and we do that O(n^2)
	times at best. :)
*/

static void ujGcPrvMarkClass(UjClass* cls, UjInstance* inst){	//if inst is NULL, mark static for class. if inst is false, mark instance
	
	while(cls){
		
		if(cls->native){	//native class
		
			if(inst){
				
				if(cls->info.native->gcInstF) cls->info.native->gcInstF(cls, inst);
			}
			else{
			
				if(cls->info.native->gcClsF) cls->info.native->gcClsF(cls);
			}
		}
		else{			//java class
		
			UInt24 addr = cls->info.java.fields;
			UInt16 numFields, n, wantedFlag = inst ? 0 : JAVA_ACC_STATIC;
			UInt8* ptr;
			char type = 0;
			
			ptr = inst ? (inst->data + cls->instDataOfst) : (cls->data + cls->clsDataOfst);
			
			numFields = ujThreadReadBE16_ex(cls->info.java.readD, addr - 2);
	
			while(numFields--){
			
				if((ujThreadReadBE16_ex(cls->info.java.readD, addr) & JAVA_ACC_STATIC) == wantedFlag){
		
					if(cls->ujc){
					#ifdef UJ_FTR_SUPPORT_UJC_FORMAT
						
						type = ujReadClassByte(cls->info.java.readD, ujThreadReadBE24_ex(cls->info.java.readD, addr + 7) + 3);
					#endif
					}
					else{
					#ifdef UJ_FTR_SUPPORT_CLASS_FORMAT
					
						type = ujReadClassByte(cls->info.java.readD, ujThreadPrvFindConst_ex(cls, ujThreadReadBE16_ex(cls->info.java.readD, addr + 4)) + 3);
					#endif
					}
					
					if(type == JAVA_TYPE_ARRAY || type == JAVA_TYPE_OBJ){
					
						HANDLE var = (HANDLE)ujThreadPrvGet32(ptr);

						if(var) ujHeapMark(var, 1);
					}
					
					ptr += ujPrvJavaTypeToSize(type);
				}
				
				if(cls->ujc){
				#ifdef UJ_FTR_SUPPORT_UJC_FORMAT
				
					addr += 10;
				#endif
				}
				else{
				#ifdef UJ_FTR_SUPPORT_CLASS_FORMAT
				
					n = ujThreadReadBE16_ex(cls->info.java.readD, addr + 6);
					addr += 8;
					while(n--) addr = ujPrvSkipAttribute(cls->info.java.readD, addr);
				#endif
				}
			}
		}
	
		//note: we do not need to go to superlcass in static case since we'll go over all classes loaded anyways
		if(!inst) break;
		cls = cls->supr;
	}
}

static void* ujGcPrvLock(HANDLE handle, Boolean* needsReleaseP){

	void* ret;
	
	*needsReleaseP = 0;
	ret = ujHeapHandleIsLocked(handle);
	if(ret) return ret;
	*needsReleaseP = 1;
	return ujHeapHandleLock(handle);
	
}

UInt8 ujGC(void){

	UjThread* th;
	UjClass* cls;
	HANDLE handle, h2;
	UInt16 t16;
	UInt8 t8;
	Boolean needsRelease;
	UjInstance* inst;
	
	
	//step 1 for class vars

	TL("Begin GC\n");
	
	cls = gFirstClass;
	
	while(cls){
	
		TL(" gc marking class %08X\n", cls);
		ujGcPrvMarkClass(cls, NULL);
		cls = cls->nextClass;
	}
	
	//step 1 for stacks and "this" pointers
	handle = gFirstThread;
	
	while(handle){
		
		TL(" gc marking thread %u\n", handle);
		ujHeapMark(handle, 2);	//mark the chunk holding the thrad as a 2. it doe snot support further traversal and it is not garbage
		
		th = ujGcPrvLock(handle, &needsRelease);
		if(th->flags.access.hasInst) ujHeapMark(th->instH, 1);

		TL(" gc marking thread stack\n");

		for(t16 = 0; t16 < th->spBase; t16++){
		
			if(ujThreadPrvBitGet(th, t16)){
			
				if((HANDLE)th->stack[t16]) ujHeapMark((HANDLE)th->stack[t16], 1);
			}	
		}
		h2 = th->nextThread;
		if(needsRelease) ujHeapHandleRelease(handle);
		handle = h2;
	}
	
	//step 2
	
	while((handle = ujHeapFirstMarked(1)) != 0){
	
		ujHeapMark(handle, 2);
		inst = ujGcPrvLock(handle, &needsRelease);
			
		if(inst->cls){	//object - handle it
			
			TL(" gc marking instance %u (0x%08X) of class 0x%08X\n", handle, inst, inst->cls);
			ujGcPrvMarkClass(inst->cls, inst);
			if(needsRelease) ujHeapHandleRelease(handle);
		}
		else{		//something that isn't an object - handle that
		
			UInt32 t;
			HANDLE h;
			
			t8 = ((UjArray*)inst)->objType;
			if(needsRelease) ujHeapHandleRelease(handle);
			
			switch(t8){
			
				case OBJ_TYPE_OBJ_ARRAY:
					
					t = ujThreadPrvArrayGetLength(handle);
					while(t){
						
						h = ujThreadPrvArrayGetRef(handle, --t);
						if(h) ujHeapMark(h, 1);
					}
					break;
			}
		}
	}

	TL(" GC done\n");
	
	return UJ_ERR_NONE;
}









////builtin classes



/*
	HUGE WARNING:	if you pop your params from java stack, and then try to allocate something, they might get GCed. How do we prevent that?
			option 1: pop later (may not be possible)
			option 2: re-push before any allocations (painfully slow)
			option 3: lock them - bad cause it makes GC cry in shame for more likely failing
			option 4: mark them somehow? how?
			option 5: peek at them at start, pop later (easiest)
*/







static UInt8 ujNat_Object_hashCode(UjThread* t, _UNUSED_ UjClass* cls){

	ujThreadPrvPushInt(t, ujThreadPrvPopRef(t));	//must be done since it is no longer a ref and should't impede GC's work

	return UJ_ERR_NONE;
}

static UInt8 ujNat_Object_Object(UjThread* t, _UNUSED_ UjClass* cls){

	ujThreadPrvPop(t);	//pop and do nothing

	return UJ_ERR_NONE;
}

typedef UInt8 (*ujNat_MiniString_ConstF)(UjThread* t, UjClass* cls, UInt24 addr, UInt32 extra);
typedef UInt8 (*ujNat_MiniString_RamF)(UjThread* t, UInt8* dataP, UInt32 extra);

#ifdef UJ_OPT_RAM_STRINGS
	#define ujNat_MiniString_genericF(t, cls, cF, rF, extra) ujNat_MiniString_genericF_(t, cls, NULL, rF, extra)
#else
	#define ujNat_MiniString_genericF(t, cls, cF, rF, extra) ujNat_MiniString_genericF_(t, cls, cF, rF, extra)
#endif

static UInt8 ujNat_MiniString_genericF_(UjThread* t, UjClass* cls, ujNat_MiniString_ConstF cF, ujNat_MiniString_RamF rF, UInt32 extra){

	HANDLE handle = ujThreadPrvPopRef(t);
	UjInstance* inst = ujHeapHandleLock(handle);
	UjClass* strCls;
	UInt32 strData;
	UInt8 v;
	
	
	#ifdef UJ_OPT_RAM_STRINGS

		strCls = NULL;
		strData = ujThreadPrvGet32(inst->data + cls->instDataOfst + 0);
	#else
		
		strCls = (UjClass*)ujThreadPrvGet32(inst->data + cls->instDataOfst + 0);
		strData = ujThreadPrvGet32(inst->data + cls->instDataOfst + 4);
	#endif

	ujHeapHandleRelease(handle);
	
	if(strCls){
	
		v = cF(t, strCls, strData, extra);
	}
	else{
	
		handle = (HANDLE)strData;
		v = rF(t, ujHeapHandleLock(handle), extra);
		ujHeapHandleRelease(handle);
	}
	
	return v;
}

static UInt8 ujNat_MiniString_prv_class_XbyteAt_(UjThread* t, UjClass* strCls, UInt24 addr, UInt32 extra){

	ujThreadPrvPushInt(t, ujReadClassByte(strCls->info.java.readD, addr + extra));
	
	return UJ_ERR_NONE;
}

static UInt8 ujNat_MiniString_prv_ram_XbyteAt_(UjThread* t, UInt8* dataP, UInt32 extra){

	ujThreadPrvPushInt(t, dataP[extra]);
	
	return UJ_ERR_NONE;
}

static UInt8 ujNat_MiniString_XbyteAt_(UjThread* t, UjClass* cls){

	return ujNat_MiniString_genericF(t, cls, ujNat_MiniString_prv_class_XbyteAt_, ujNat_MiniString_prv_ram_XbyteAt_, ujThreadPrvPopInt(t) + 2);
}

static UInt8 ujNat_MiniString_prv_class_Xlen_(UjThread* t, UjClass* strCls, UInt24 addr, UInt32 extra){

	ujThreadPrvPushInt(t, ujThreadReadBE16_ex(strCls->info.java.readD, addr));
	
	return UJ_ERR_NONE;
}

static UInt8 ujNat_MiniString_prv_ram_Xlen_(UjThread* t, UInt8* dataP, UInt32 extra){

	ujThreadPrvPushInt(t, ujThreadPrvGet16(dataP));
	
	return UJ_ERR_NONE;
}

static UInt8 ujNat_MiniString_Xlen_(UjThread* t, UjClass* cls){

	return ujNat_MiniString_genericF(t, cls, ujNat_MiniString_prv_class_Xlen_, ujNat_MiniString_prv_ram_Xlen_, 0);
}

#ifdef UJ_FTR_STRING_FEATURES

	static UInt8 ujNat_MiniString_prv_class_charAt(UjThread* t, UjClass* strCls, UInt24 addr, UInt32 idx){
	
		UInt16 L = ujThreadReadBE16_ex(strCls->info.java.readD, addr);
		UInt8 b, len;
		
		addr += 2;
		
		while(L){
		
			b = ujReadClassByte(strCls->info.java.readD, addr);
			if((b & 0xE0) == 0xC0) len = 2;
			else if((b & 0xF0) == 0xE0) len = 3;
			else len = 1;
		
			if(!idx--){
			
				break;
			}
			L -= len;
			addr += len;
		}
		if(!L) return UJ_ERR_ARRAY_INDEX_OOB;
		switch(len - 1){
			
			case 1 - 1:
				
				L = b;
				break;
			
			case 2 - 1:
				
				L = b & 0x1F;
				goto piece;
			
			case 3 - 1:
				L = b;
				L <<= 6;
				L |= ujReadClassByte(strCls->info.java.readD, ++addr) & 0x3F;
		piece:
				L <<= 6;
				L |= ujReadClassByte(strCls->info.java.readD, ++addr) & 0x3F;
				break;
		}
		
		ujThreadPrvPushInt(t, L);
	
		return UJ_ERR_NONE;
	}
	
	static UInt8 ujNat_MiniString_prv_ram_charAt(UjThread* t, UInt8* dataP, UInt32 idx){
	
		UInt16 L = ujThreadPrvGet16(dataP);
		UInt8 b, len;
		
		dataP += 2;
		
		while(L){
		
			b = *dataP;
			if((b & 0xE0) == 0xC0) len = 2;
			else if((b & 0xF0) == 0xE0) len = 3;
			else len = 1;
		
			if(!idx--){
			
				break;
			}
			L -= len;
			dataP += len;
		}
		if(!L) return UJ_ERR_ARRAY_INDEX_OOB;
		switch(len - 1){
			
			case 1 - 1:
				
				L = b;
				break;
			
			case 2 - 1:
				
				L = b & 0x1F;
				goto piece;
			
			case 3 - 1:
				L = b;
				L <<= 6;
				L |= (*++dataP) & 0x3F;
		piece:
				L <<= 6;
				L |= (*++dataP) & 0x3F;
				break;
		}
		
		ujThreadPrvPushInt(t, L);
	
		return UJ_ERR_NONE;
	}

	static UInt8 ujNat_MiniString_charAt(UjThread* t, UjClass* cls){
	
		return ujNat_MiniString_genericF(t, cls, ujNat_MiniString_prv_class_charAt, ujNat_MiniString_prv_ram_charAt, ujThreadPrvPopInt(t));
	}
	
	static UInt8 ujNat_MiniString_prv_class_length(UjThread* t, UjClass* strCls, UInt24 addr, UInt32 extra){
	
		UInt16 L = ujThreadReadBE16_ex(strCls->info.java.readD, addr);
		UInt8 b, len;
		UInt32 ret = 0;
		
		addr += 2;
		
		while(L){
		
			b = ujReadClassByte(strCls->info.java.readD, addr);
			if((b & 0xE0) == 0xC0) len = 2;
			else if((b & 0xF0) == 0xE0) len = 3;
			else len = 1;
		
			ret++;
			L -= len;
			addr += len;
		}
		
		ujThreadPrvPushInt(t, ret);
	
		return UJ_ERR_NONE;
	}
	
	static UInt8 ujNat_MiniString_prv_ram_length(UjThread* t, UInt8* dataP, UInt32 extra){
	
		UInt16 L = ujThreadPrvGet16(dataP);
		UInt8 b, len;
		UInt32 ret = 0;
		
		dataP += 2;
		
		while(L){
		
			b = *dataP;
			if((b & 0xE0) == 0xC0) len = 2;
			else if((b & 0xF0) == 0xE0) len = 3;
			else len = 1;
		
			ret++;
			L -= len;
			dataP += len;
		}
		
		ujThreadPrvPushInt(t, ret);
	
		return UJ_ERR_NONE;
	}

	static UInt8 ujNat_MiniString_length(UjThread* t, UjClass* cls){
	
		return ujNat_MiniString_genericF(t, cls, ujNat_MiniString_prv_class_length, ujNat_MiniString_prv_ram_length, 0);
	}
#endif


static UInt8 ujNat_MiniString_init(UjThread* t, UjClass* cls){

	HANDLE objHandle = (HANDLE)ujThreadPrvPeek(t, 1);
	HANDLE arrayHandle = (HANDLE)ujThreadPrvPeek(t, 0);
	Int32 i, len = ujThreadPrvArrayGetLength(arrayHandle);
	UInt8* data;
	UInt8 ret, ofst;
	UjInstance* obj;
	HANDLE handle;
	
	
	handle = ujHeapHandleNew(len + 2);
	if(!handle) return UJ_ERR_OUT_OF_MEMORY;
	
	data = ujHeapHandleLock(handle);
	ujThreadPrvPut16(data, len);
	data += 2;
	
	for(i = 0; i < len; i++){
	
		*data++ = ujThreadPrvArrayGet1B(arrayHandle, i);
	}
	ujHeapHandleRelease(handle);
	
	obj = ujHeapHandleLock(objHandle);
	#ifdef UJ_OPT_RAM_STRINGS
	
		ofst = 0;
	#else
	
		ujThreadPrvPut32(obj->data + cls->instDataOfst, 0);
		ofst = 4;
	#endif
	ujThreadPrvPut32(obj->data + cls->instDataOfst + ofst, (UInt32)handle);
	ujHeapHandleRelease(objHandle);
	
	ujThreadPrvPop(t);		//pop off our params
	ujThreadPrvPop(t);		//pop off our params
	
	return UJ_ERR_NONE;
}

static UInt8 ujNat_RT_consolePut(UjThread* t, _UNUSED_ UjClass* myCls){

	ujLog("%c", (char)ujThreadPop(t));

	return UJ_ERR_NONE;
}

static UInt8 ujNat_RT_threadCreate(UjThread* oldT, _UNUSED_ UjClass* myCls){

	HANDLE handle;
	UjInstance* inst;
	UInt24 addr;
	UjClass* cls;
	HANDLE threadH;
	UInt8 ret;
	UjThread* t;
	UjPrvStrEqualParam name, type;
	
	
	handle = ujThreadPrvPopRef(oldT);
	if(!handle) return UJ_ERR_NULL_POINTER;
	inst = ujHeapHandleLock(handle);
	
	threadH = ujThreadCreate(0);
	if(!threadH) return UJ_ERR_OUT_OF_MEMORY;
	
	name.type = STR_EQ_PAR_TYPE_PTR;
	name.data.ptr.len = ujCstrlen(name.data.ptr.str = "run");
	
	type.type = STR_EQ_PAR_TYPE_PTR;
	type.data.ptr.len = ujCstrlen(type.data.ptr.str = "()V");
	
	cls = inst->cls;
	
	addr = ujThreadPrvGetMethodAddr(&cls, &name, &type, 0, 0, NULL);
	if(addr == UJ_PC_BAD) return UJ_ERR_METHOD_NONEXISTENT;
	
	t = ujHeapHandleLock(threadH);
	ujThreadPrvLocalStoreRef(t, 0, handle);
	
	ujHeapHandleRelease(handle);
	
	ret = ujThreadPrvGoto(t, cls, handle, addr);
	ujHeapHandleRelease(threadH);
	
	return UJ_ERR_NONE;	
}

static void ujNat_MiniString_instGc(UjClass* cls, UjInstance* inst){

	UjClass* strCls;
	HANDLE h;
	UInt8 ofst;
	
	#ifdef UJ_OPT_RAM_STRINGS
		
		strCls = NULL;
		ofst = 0;
	#else
	
		strCls = (UjClass*)ujThreadPrvGet32(inst->data + cls->instDataOfst + 0);
		ofst = 4;
	
	#endif
	if(!strCls){
		
		h = (HANDLE)ujThreadPrvGet32(inst->data + cls->instDataOfst + ofst);
		
		//if we get here from an alloc inside a call to <init> on this very instance, handle will be zero - we should be prepared for that
		if(h) ujHeapMark(h, 2);
	}
}

static const UjNativeClass ujNatCls_Object =		{
								"java/lang/Object",
								0,
								0,
								NULL,
								NULL,
								2,
								{
									{
										"hashCode",
										"()I",
										ujNat_Object_hashCode,
										JAVA_ACC_PUBLIC | JAVA_ACC_NATIVE
									},
									{
										"<init>",
										"()V",
										ujNat_Object_Object,
										JAVA_ACC_PUBLIC | JAVA_ACC_NATIVE
									}
								}
							};

static const UjNativeClass  ujNatCls_MiniString =	{
								"uj/lang/MiniString",
								0,
								#ifdef UJ_OPT_RAM_STRINGS
									4,
								#else
									8,
								#endif
								NULL,
								ujNat_MiniString_instGc,
								#ifdef UJ_FTR_STRING_FEATURES
									
									2 +
									
								#endif
								3,
								{
									#ifdef UJ_FTR_STRING_FEATURES
										{
											"charAt",
											"(I)C",
											ujNat_MiniString_charAt,
											JAVA_ACC_PUBLIC | JAVA_ACC_NATIVE
										},
										{
											"length",
											"()I",
											ujNat_MiniString_length,
											JAVA_ACC_PUBLIC | JAVA_ACC_NATIVE
										},
									#endif
									{
										"XbyteAt_",
										"(I)B",
										ujNat_MiniString_XbyteAt_,
										JAVA_ACC_PUBLIC | JAVA_ACC_NATIVE
									},
									{
										"Xlen_",
										"()I",
										ujNat_MiniString_Xlen_,
										JAVA_ACC_PUBLIC | JAVA_ACC_NATIVE
									},
									{
										"<init>",
										"([B)V",
										ujNat_MiniString_init,
										JAVA_ACC_PUBLIC | JAVA_ACC_NATIVE
									} 
								 }
							};
							
static const UjNativeClass ujNatCls_UJ =		{
								"uj/lang/RT",
								0,
								0,
								NULL,
								NULL,
								2,
								{
									{
										"consolePut",
										"(C)V",
										ujNat_RT_consolePut,
										JAVA_ACC_PUBLIC | JAVA_ACC_NATIVE | JAVA_ACC_STATIC	
									},
									{	"threadCreate",
										"(Ljava/lang/Runnable;)V",
										ujNat_RT_threadCreate,
										JAVA_ACC_PUBLIC | JAVA_ACC_NATIVE | JAVA_ACC_STATIC	
									}
								}
							};


static UInt8 ujInitBuiltinClasses(UjClass** objectClsP){

	UjClass* cls;
	UInt8 ret;

	ret = ujRegisterNativeClass(&ujNatCls_Object, NULL, &cls);
	if(ret != UJ_ERR_NONE) return ret;

	if(objectClsP) *objectClsP = cls;

	ret = ujRegisterNativeClass(&ujNatCls_MiniString, cls, NULL);
	if(ret != UJ_ERR_NONE) return ret;

	ret = ujRegisterNativeClass(&ujNatCls_UJ, cls, NULL);
	if(ret != UJ_ERR_NONE) return ret;

	return UJ_ERR_NONE;
}



#ifndef _UJ_H_
#define _UJ_H_


#include "common.h"
#include "ujHeap.h"

#define UJ_FTR_SUPPORT_CLASS_FORMAT

//callback types
UInt8 ujReadClassByte(UInt32 userData, UInt32 offset);

#ifdef UJ_LOG
	void ujLog(const char* fmtStr, ...);
#else
	#define ujLog(...)
#endif

//api

#define UJ_DEFAULT_STACK_SIZE	192

//some of these will thrown an exception at a higher level, perhaps?
#define UJ_ERR_NONE			0	//				not an error (may mean true)
#define UJ_ERR_FALSE			1	//				also not an error - just means false

#define UJ_ERR_INVALID_OPCODE		2	// InternalError [?]
#define UJ_ERR_METHOD_NONEXISTENT	3	// UnknownError [?]		ujThreadGoto to an invalid place
#define UJ_ERR_DEPENDENCY_MISSING	4	// NoClassDefFoundError		ujClassLoad fails because needed superlcass is missing or otherwise class not found

#define UJ_ERR_STACK_SPACE		16	// StackOverflowError
#define UJ_ERR_METHOD_FLAGS_MISMATCH	17	// UnknownError [?]
#define UJ_ERR_DIV_BY_ZERO		18	// ArithmeticException
#define UJ_ERR_INVALID_CAST		19	// ClassCastException
#define UJ_ERR_OUT_OF_MEMORY		20	// OutOfMemoryError
#define UJ_ERR_ARRAY_INDEX_OOB		21	// IndexOutOfBoundsException	index out of bounds
#define UJ_ERR_FIELD_NOT_FOUND		22	// UnknownError [?]		such a field doe snot exist (by name)
#define UJ_ERR_NULL_POINTER		23	// NullPointerException
#define UJ_ERR_MON_STATE_ERR		24	// IllegalMonitorStateException	monitor state exception
#define UJ_ERR_NEG_ARR_SZ		25	// NegativeArraySizeException

#define UJ_ERR_RETRY_LATER		50	// 				not an error, just retry later

#define UJ_ERR_USER_EXCEPTION		99	// 				in case of exceptions disabled... or uncaught
#define UJ_ERR_INTERNAL			100	// UnknownError [?]		bad internal error


#define UJ_THREAD_QUANTUM		10	//instrs

struct UjClass;
struct UjThread;
struct UjInstance;


UInt8 ujInit(struct UjClass** objectClsP);

UInt8 ujLoadClass(UInt32 readD, struct UjClass** clsP);

UInt8 ujInitAllClasses(void);

HANDLE ujThreadCreate(UInt16 stackSz /*zero for default*/);
UInt32 ujThreadDbgGetPc(HANDLE threadH);
UInt8 ujThreadGoto(HANDLE threadH, struct UjClass* cls, const char* methodNamePtr, const char* methodTypePtr);	//static call only (used to clal main or some such thing)
Boolean ujCanRun(void);
UInt8 ujInstr(void);		//return UJ_ERR_*
UInt8 ujThreadDestroy(HANDLE threadH);
UInt8 ujGC(void);				//called by heap manager
UInt32 ujGetNumInstrs(void);



//some flags
#define JAVA_ACC_PUBLIC 	0x0001 	//Declared public; may be accessed from outside its package.
#define JAVA_ACC_PRIVATE 	0x0002 	//Declared private; accessible only within the defining class.
#define JAVA_ACC_PROTECTED 	0x0004 	//Declared protected; may be accessed within subclasses.
#define JAVA_ACC_STATIC 	0x0008 	//Declared static.
#define JAVA_ACC_FINAL 		0x0010 	//Declared final; may not be overridden.
#define JAVA_ACC_SYNCHRONIZED 	0x0020 	//Declared synchronized; invocation is wrapped in a monitor lock.
#define JAVA_ACC_NATIVE 	0x0100 	//Declared native; implemented in a language other than Java.
#define JAVA_ACC_INTERFACE	0x0200	//Is an interface, not a class. 
#define JAVA_ACC_ABSTRACT 	0x0400 	//Declared abstract; no implementation is provided.
#define JAVA_ACC_STRICT 	0x0800	//Declared strictfp; floating-point mode is FP-strict 


UInt32 ujThreadPop(struct UjThread* t);
Boolean ujThreadPush(struct UjThread* t, UInt32 v, Boolean isRef);
UInt32 ujArrayLen(UInt32 arr);
UInt32 ujArrayGetByte(UInt32 arr, UInt32 idx);
UInt32 ujArrayGetShort(UInt32 arr, UInt32 idx);
UInt32 ujArrayGetInt(UInt32 arr, UInt32 idx);
void* ujArrayRawAccessStart(UInt32 arr);
void ujArrayRawAccessFinish(UInt32 arr);

//native classes

typedef UInt8 (*ujNativeMethodF)(struct UjThread*, struct UjClass*);
typedef void (*ujNativeGcInstF)(struct UjClass* cls, struct UjInstance* inst);
typedef void (*ujNativeGcClsF)(struct UjClass* cls);

typedef struct{

	const char* name;
	const char* type;
	ujNativeMethodF func;
	UInt16 flags;

}UjNativeMethod;

typedef struct{

	const char* clsName;
	UInt16 clsDatSz;
	UInt16 instDatSz;
	
	ujNativeGcClsF gcClsF;		//called once per gc to mark all used handles to level 1
	ujNativeGcInstF gcInstF;	//called once per gc per object to mark all used handles to 1
	
	UInt16 numMethods;
	UjNativeMethod methods[];

}UjNativeClass;

UInt8 ujRegisterNativeClass(const UjNativeClass* nCls/*references and must remain valid forever*/, struct UjClass* super, struct UjClass** clsP);



#endif

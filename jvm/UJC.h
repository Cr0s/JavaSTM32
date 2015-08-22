#ifndef _UJC_H_
#define _UJC_H_
#include "common.h"

#define UJC_MAGIC	0x4AEC

typedef struct{

	UInt16 magic;		//UJC_MAGIC
	UInt16 clsName;		//constant index
	UInt16 suprClsName;	//constant index
	UInt16 flags;		//java flags

	UInt24 interfaces;	//pointer into data store
	UInt24 methods;		//pointer into data store
	UInt24 fields;		//pointer into data store

	UInt8 clsNameHash;	//hash of class name

	UInt8 data[];		//data store
	
}UjcClass;

//data store layout order:	CONSTANT_REFS, CONSTANTS, INTERFACES, METHODS, FIELDS, CODE

typedef struct {
	
	UInt16 flags;		//0x00
	UInt8 nameHash;		//0x02: hash of name for quicker matching
	UInt8 typeHash;		//0x03: hash of type for quicker matching
	
	UInt24 nameAddr;	//0x04: pointer to string in constant area
	UInt24 typeAddr;	//0x07: pointer to string in constant area
	
	UInt24 codeAddr;	//0x0A: pointer to code in constant area
	
}UjcMethod;

typedef struct {

	UInt16 flags;
	UInt8 nameHash;		//hash of name for quicker matching
	UInt8 typeHash;		//hash of type for quicker matching
	UInt24 nameAddr;	//pointer to string in constant area
	UInt24 typeAddr;	//pointer to string in constant area

}UjcField;

typedef struct{

	UInt16 numConstants;	//off by one just like java
	UInt24 constantAddrs;	//pointers to data in ocnstant area
	
}UjcConstantRefs;

typedef struct{

	UInt16 numInterfaces;
	UInt24 interfaces[];	//pointers to strings in constant area

}UjcInterfaces;

typedef struct{

	UInt16 numMethods;
	UjcMethod methods[];

}UjcMethods;

typedef struct{
	
	UInt16 numFields;
	UjcField fields[];

}UjcFields;

typedef struct{
	
	UInt8 type;
	UInt8 data[];

}UjcConstant;

/* method storage in data area:

	excStruct excs [numExcs]
	UInt16 numExcs;
	UInt16 locals;
	UInt16 stackSz;
	UInt8 code[]		<----- code pointer points here


*/




#endif

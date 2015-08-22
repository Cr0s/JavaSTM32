#ifndef _COMMON_H_
#define _COMMON_H_

typedef signed char Int8;
typedef signed short Int16;
typedef signed long Int32;
typedef unsigned char UInt8;
typedef unsigned short UInt16;
typedef unsigned long UInt32;
typedef unsigned char Boolean;
typedef float UjFloat;

#ifdef __HITECH__
	typedef unsigned short long UInt24;
#else
	typedef unsigned long UInt24;
#endif

#define true	1
#define false	0
#ifndef NULL
	#define NULL	0
#endif
#ifndef NAN
	#define NAN	(0.0f/0.0f)
#endif

void err(const char* str);

//#define offsetof(struc, membr)	((int)(&((struc*)0)->membr))

#include <stdio.h>
#define TL(...)		//fprintf(stderr, "**UL** " __VA_ARGS__)

#define DEBUG		1

#define _UNUSED_	__attribute__((unused))
#define _INLINE_	__attribute__((always_inline)) inline 


#define GET_ADDRESS(x)		((UInt32)(&(x)))
UInt8 pgm_read(UInt32 addr);
#define pgm_read_str(addr)	(*((UInt8*)(addr)))
#define _PROGMEM_
#define HEAP_ALIGN		2
#define INCLUDE_PIC_FILES
#define SDLOADER
#define GPIO_PORTS		2
#define GPIO_PINS_PER_PORT	16
#define EEPROM_SIZE		1536
#define _HEAP_ATTRS_		__attribute__((far))
#define EEPROM_EMUL_ADDR	0x800000UL	//we use 4 pages

#define F_CPU 48000000
#define HEAP_SZ 10000


#ifdef INCLUDE_ATMEGA_FILES

	#define BAUD 38400UL
	#include <avr/io.h>
	#include <avr/pgmspace.h>
	#include <avr/eeprom.h>
	#include <avr/interrupt.h>
	#include <util/setbaud.h>
	#include <util/delay.h>
	#include <avr/boot.h>
#endif

#ifdef INCLUDE_PIC_FILES

	#ifdef CPU_DSPIC
		#include <p33FJ128GP802.h>
	#endif

	#ifdef CPU_PIC24
		#include <p24FJ32GA002.h>
	#endif

#endif


#ifdef UJ_LOG

	void ujLog(const char* fmtStr, ...);

#else

	#define ujLog(...)
#endif 

#endif

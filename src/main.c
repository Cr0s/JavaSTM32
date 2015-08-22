#include "main.h"

#include <libopencm3/cm3/scb.h>
#include <libopencm3/stm32/crc.h>
#include <libopencm3/stm32/f1/bkp.h>
#include <libopencm3/stm32/f1/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencmsis/core_cm3.h>
#include <libopencm3/cm3/nvic.h>

#include "../jvm/common.h"
#include "../jvm/uj.h"

UInt8 ujReadClassByte(UInt32 userData, UInt32 offset) {
	return class_bytes[offset];
}

extern const UjNativeClass nativeCls_UC;

void do_delay(void) {
    volatile int i = 0;
    for (i = 0; i < 100000; i++);
}

void led_on(void) {
	GPIO_BSRR(GPIOB) = GPIO10;
}


void led_off(void) {
	GPIO_BRR(GPIOB) = GPIO10;
}

void do_blink(void) {
	uint8_t i = 0;
	for (i = 0; i < 3; i++) {
		do_delay();
		GPIO_BSRR(GPIOB) = GPIO10;

		do_delay();
		GPIO_BRR(GPIOB) = GPIO10;
	}

	do_delay();
	do_delay();
	do_delay();
	do_delay();

	for (i = 0; i < 10; i++) {
		do_delay();
		GPIO_BSRR(GPIOB) = GPIO10;

		do_delay();
		GPIO_BRR(GPIOB) = GPIO10;
	}
}

int main(void) {
	//rcc_clock_setup_in_hsi_out_48mhz();
	rcc_clock_setup_in_hse_8mhz_out_72mhz();
    rcc_periph_clock_enable(RCC_GPIOB);
    gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, GPIO10);
    do_blink();

    GPIO_BRR(GPIOB) = GPIO10;

	struct UjClass* mainClass = NULL;
	struct UjClass* objectClass;
	UInt32 threadH;

	UInt8 ret = ujInit(&objectClass);
	if(ret != UJ_ERR_NONE){
	    GPIO_BSRR(GPIOB) = GPIO10;
	    while (true) {}
	}

	ret = ujRegisterNativeClass(&nativeCls_UC, objectClass, NULL);
	if(ret != UJ_ERR_NONE){
		GPIO_BSRR(GPIOB) = GPIO10;
		while (true) {}
	}

	ret = ujLoadClass(0, &mainClass);
	if(ret != UJ_ERR_NONE){
		GPIO_BSRR(GPIOB) = GPIO10;
		while (true) {}
	}

	ret = ujInitAllClasses();
	if(ret != UJ_ERR_NONE){
		GPIO_BSRR(GPIOB) = GPIO10;
		while (true) {}
	}

	//now classes are loaded, time to call the entry point

	threadH = ujThreadCreate(0);
	if(!threadH) {
		GPIO_BSRR(GPIOB) = GPIO10;
		while (true) {}
	}

	UInt8 h = ujThreadGoto(threadH, mainClass, "main", "()V");
	if(h == UJ_ERR_METHOD_NONEXISTENT) {
		GPIO_BSRR(GPIOB) = GPIO10;
		while (true) {}
	}

	while(ujCanRun()) {
		h = ujInstr();
		if(h != UJ_ERR_NONE) {
			GPIO_BSRR(GPIOB) = GPIO10;
			while (true) {}
		}
	}


	GPIO_BSRR(GPIOB) = GPIO10;
	while (true) {}
}

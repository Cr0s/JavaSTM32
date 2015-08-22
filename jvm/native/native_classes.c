#include "native_classes.h"
#include <libopencm3/stm32/f1/gpio.h>

UInt8 native_setLed(_UNUSED_ struct UjThread* t, _UNUSED_ struct UjClass* cls) {
	Boolean led = ujThreadPop(t);

	if (led) {
		GPIO_BSRR(GPIOB) = GPIO10;
	} else {
		GPIO_BRR(GPIOB) = GPIO10;
	}

	return UJ_ERR_NONE;
}



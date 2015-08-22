#ifndef NATIVE_NATIVE_CLASSES_H_
#define NATIVE_NATIVE_CLASSES_H_

#include "common.h"
#include "UJC.h"
#include "uj.h"
#include <stdlib.h>
#include <libopencm3/stm32/gpio.h>

UInt8 native_setLed(_UNUSED_ struct UjThread* t, _UNUSED_ struct UjClass* cls);

const UjNativeClass nativeCls_UC =
	{
		"Native",
		0,
		0,
		NULL,
		NULL,

		1

		,{
			{
				"setLed",
				"(Z)V",
				native_setLed,
				JAVA_ACC_PUBLIC | JAVA_ACC_NATIVE | JAVA_ACC_STATIC
			},
		}
	};

#endif /* NATIVE_NATIVE_CLASSES_H_ */

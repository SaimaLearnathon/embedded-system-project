#ifndef INC_CORE_SYSTEM_H
#define INC_CORE_SYSTEM_H

#include "common-defines.h"

#define BLINK_PERIOD_MS  2000

/* Initialise RCC, GPIO (on-board LED) and the SysTick 1 ms tick. */
void system_setup(void);

/* Number of SysTick milliseconds elapsed since boot. */
uint32_t system_get_ticks(void);

void system_delay(uint64_t milliseconds);

#endif /* INC_CORE_SYSTEM_H */

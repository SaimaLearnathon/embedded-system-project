#include <libopencm3/stm32/rcc.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/systick.h>

#include "core/system.h"

/* SysTick fires every 1 ms; the ISR just keeps a running millisecond count. */
static volatile uint32_t ms_ticks;

/* Prototype so -Wmissing-prototypes is happy for the ISR. */
void sys_tick_handler(void);

static void rcc_setup(void)
{
	/* 8 MHz crystal (HSE), PLL -> 72 MHz. */
	rcc_clock_setup_pll(&rcc_hse_configs[RCC_CLOCK_HSE8_72MHZ]);
}

static void systick_setup(void)
{
	/* AHB is 72 MHz; divide by 72000 (RELOAD + 1) -> 1 kHz = 1 ms tick. */
	systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
	systick_set_reload(72000 - 1);
	systick_clear();                  /* start counting from reload */
	systick_interrupt_enable();       /* core exception, no NVIC needed */
	systick_counter_enable();
}

void systick_teardown(void)
{
	systick_interrupt_disable();
	systick_counter_disable();
	systick_clear();
	SCB_ICSR = SCB_ICSR_PENDSTCLR;
}

void system_setup(void)
{
	rcc_setup();
	systick_setup();
}

/* SysTick exception #15 handler (name matches the libopencm3 vector table). */
void sys_tick_handler(void)
{
	++ms_ticks;
}

uint32_t system_get_ticks(void)
{
	return ms_ticks;
}

void system_delay(uint32_t milliseconds)
{
	uint32_t start = ms_ticks;
	while ((ms_ticks - start) < milliseconds) {
		/* busy-wait */
	}
}

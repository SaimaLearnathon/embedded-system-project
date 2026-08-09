#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>

#include "core/system.h"

/* BluePill on-board LED, active-low, on PC13. */
#define LED_PORT  GPIOC
#define LED_PIN   GPIO13

static void gpio_setup(void)
{
	/* Enable port C clock and configure PC13 as 2 MHz push-pull output. */
	rcc_periph_clock_enable(RCC_GPIOC);
	gpio_set_mode(LED_PORT, GPIO_MODE_OUTPUT_2_MHZ,
	              GPIO_CNF_OUTPUT_PUSHPULL, LED_PIN);
}

int main(void)
{
	system_setup();
	gpio_setup();

	uint32_t last_toggle = 0;
	while (1) {
		uint32_t now = system_get_ticks();
		if ((now - last_toggle) >= BLINK_PERIOD_MS) {
			gpio_toggle(LED_PORT, LED_PIN);  /* active-low: toggle ON/OFF */
			last_toggle = now;
		}
		__asm__("wfi");  /* sleep until the next SysTick exception */
	}

	return 0;
}
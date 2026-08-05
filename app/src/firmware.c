#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>

#define LED_PORT  GPIOC
#define LED_PIN   GPIO13

static void rcc_setup(void)
{
	/* BluePill has an 8 MHz crystal on the board (HSE). Run at 72 MHz. */
	rcc_clock_setup_pll(&rcc_hse_configs[RCC_CLOCK_HSE8_72MHZ]);
}

static void gpio_setup(void)
{
	/* Enable the peripheral clock for port C (where PC13 LED lives). */
	rcc_periph_clock_enable(RCC_GPIOC);

	/* PC13 LED -> push-pull output, 2 MHz. */
	gpio_set_mode(LED_PORT, GPIO_MODE_OUTPUT_2_MHZ,
	              GPIO_CNF_OUTPUT_PUSHPULL, LED_PIN);
}

int main(void)
{
	rcc_setup();
	gpio_setup();

	while (1) {
		gpio_clear(LED_PORT, LED_PIN);  /* LED ON (active-low) */
		for (volatile uint32_t i = 0; i < 5000000; i++) {
			__asm__("nop");
		}

		gpio_set(LED_PORT, LED_PIN);    /* LED OFF */
		for (volatile uint32_t i = 0; i < 5000000; i++) {
			__asm__("nop");
		}
	}

	return 0;
}
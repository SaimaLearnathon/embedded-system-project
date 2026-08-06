#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/cm3/systick.h>

#define LED_PORT  GPIOC
#define LED_PIN   GPIO13

/* Toggle the LED every 500 ms. */
#define BLINK_PERIOD_MS  2000

/* SysTick fires every 1 ms; the ISR counts up to BLINK_PERIOD_MS. */
static volatile uint32_t ms_ticks;

void sys_tick_handler(void);

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

static void systick_setup(void)
{
	/* AHB is 72 MHz; divide by 72000 (RELOAD + 1) -> 1 kHz = 1 ms tick. */
	systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
	systick_set_reload(72000 - 1);
	systick_clear();                  /* start counting from reload */
	systick_interrupt_enable();       /* SysTick is a core exception, no NVIC needed */
	systick_counter_enable();
}

/* SysTick exception #15 handler (name matches the libopencm3 vector table). */
void sys_tick_handler(void)
{
	if (++ms_ticks >= BLINK_PERIOD_MS) {
		ms_ticks = 0;
		gpio_toggle(LED_PORT, LED_PIN);  /* active-low: toggle ON/OFF */
	}
}

int main(void)
{
	rcc_setup();
	gpio_setup();
	systick_setup();

	while (1) {
		__asm__("wfi");                /* sleep until the next SysTick exception */
	}

	return 0;
}
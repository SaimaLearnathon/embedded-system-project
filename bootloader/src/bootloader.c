#include "common-defines.h"
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/stm32/memorymap.h>
#include "core/uart.h"
#include "core/system.h"
#include "comms.h"
#include "bl-flash.h"
#include "core/simple-timer.h"

#define BOOTLOADER_SIZE (0x8000U)
#define MAIN_APP_START_ADDRESS (FLASH_BASE + BOOTLOADER_SIZE)
#define FLASH_END_ADDRESS (FLASH_BASE + (256U * 1024U))
#define FLASH_PAGE_SIZE (2048U)
#define SRAM_BASE_ADDRESS (0x20000000U)
#define SRAM_END_ADDRESS (SRAM_BASE_ADDRESS + (64U * 1024U))
#define ENABLE_FLASH_WRITE_TEST (0U)

static void jump_to_main(void) __attribute__((noreturn));
static bool application_is_valid(void);
static void prepare_to_jump(void);
static void run_flash_write_test(void);

static bool application_is_valid(void)
{
	const uint32_t *app_vector_table = (const uint32_t *)MAIN_APP_START_ADDRESS;
	const uint32_t app_stack = app_vector_table[0];
	const uint32_t app_reset = app_vector_table[1];
	const uint32_t app_reset_address = app_reset & ~1U;

	return ((app_stack >= SRAM_BASE_ADDRESS) && (app_stack <= SRAM_END_ADDRESS) &&
		((app_stack & 0x7U) == 0U) && ((app_reset & 1U) != 0U) &&
		(app_reset_address >= MAIN_APP_START_ADDRESS) &&
		(app_reset_address < FLASH_END_ADDRESS));
}

/* Plain-text diagnostics for viewing in a raw serial terminal (e.g. PuTTY).
 * Do not run this in the same session as the fw-updater packet protocol:
 * it shares the UART with comms.c's fixed-length binary framing, and text
 * bytes mixed into that stream will desync the host's packet parser. */
static void uart_write_string(const char *s)
{
	while (*s) {
		uart_write_byte((uint8_t)*s++);
	}
}

static void uart_write_hex32(uint32_t value)
{
	static const char hex_digits[] = "0123456789ABCDEF";
	uart_write_string("0x");
	for (int shift = 28; shift >= 0; shift -= 4) {
		uart_write_byte((uint8_t)hex_digits[(value >> shift) & 0xFU]);
	}
}

static void log_boot_diagnostics(void)
{
	const uint32_t *app_vector_table = (const uint32_t *)MAIN_APP_START_ADDRESS;
	const uint32_t app_stack = app_vector_table[0];
	const uint32_t app_reset = app_vector_table[1];

	uart_write_string("\r\n[bootloader] app_stack=");
	uart_write_hex32(app_stack);
	uart_write_string(" app_reset=");
	uart_write_hex32(app_reset);
	uart_write_string(application_is_valid() ? " valid=YES\r\n" : " valid=NO\r\n");
}

static void prepare_to_jump(void)
{
	systick_interrupt_disable();
	systick_counter_disable();
	nvic_disable_irq(NVIC_USART1_IRQ);
}

static void run_flash_write_test(void)
{
	uint8_t data[1024] = {0};

	for (uint16_t i = 0; i < 1024; i++) {
		data[i] = (uint8_t)(i & 0xffU);
	}

	bl_flash_erase_main_application();
	bl_flash_write(MAIN_APP_START_ADDRESS + (0U * FLASH_PAGE_SIZE), data, 1024);
	bl_flash_write(MAIN_APP_START_ADDRESS + (1U * FLASH_PAGE_SIZE), data, 1024);
	bl_flash_write(MAIN_APP_START_ADDRESS + (2U * FLASH_PAGE_SIZE), data, 1024);
	bl_flash_write(MAIN_APP_START_ADDRESS + (3U * FLASH_PAGE_SIZE), data, 1024);
}

static void jump_to_main(void){
	typedef void (*void_fn)(void);
	uint32_t *app_vector_table = (uint32_t *)MAIN_APP_START_ADDRESS;
	uint32_t app_stack = app_vector_table[0];
	uint32_t app_reset = app_vector_table[1];
	void_fn jump_fn = (void_fn)app_reset;

	prepare_to_jump();
	SCB_VTOR = MAIN_APP_START_ADDRESS;
	__asm volatile (
		"msr msp, %0    \n"
		"bx  %1         \n"
		:
		: "r" (app_stack), "r" (jump_fn)
		: "memory"
	);
	__builtin_unreachable();
}

int main(void)
{
	system_setup();
	
	
	// uart_setup();
	// log_boot_diagnostics();

	// if (ENABLE_FLASH_WRITE_TEST != 0U) {
	// 	uart_write_string("[bootloader] running flash write test\r\n");
	// 	run_flash_write_test();
	// 	uart_write_string("[bootloader] flash write test complete\r\n");
	// }

	// if (application_is_valid()) {
	// 	uart_write_string("[bootloader] application valid, jumping\r\n");
	// 	system_delay(10);
	// 	jump_to_main();
	// }

	// uart_write_string("[bootloader] application invalid, waiting for updater\r\n");
	// comms_setup();

	simple_timer_t timer;

	simple_timer_setup(&timer,500,true);


	while (true) {
		if(simple_timer_has_elapsed(&timer)){
			volatile int x=0;
			x++;

		}
	}
}

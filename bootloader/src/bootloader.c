#include "common-defines.h"
#include <libopencm3/cm3/scb.h>
#include <libopencm3/stm32/memorymap.h>
#include "core/uart.h"
#include "core/system.h"
#include "comms.h"
#include <libopencm3/stm32/gpio.h>
#include "bl-flash.h"

#define BOOTLOADER_SIZE (0x8000U)
#define MAIN_APP_START_ADDRESS (FLASH_BASE + BOOTLOADER_SIZE)
#define FLASH_END_ADDRESS (FLASH_BASE + (256U * 1024U))
#define SRAM_BASE_ADDRESS (0x20000000U)
#define SRAM_END_ADDRESS (SRAM_BASE_ADDRESS + (64U * 1024U))
#define UART_PORT (GPIOA)
#define RX_PIN (PA10)
#define TX_PIN (PA9)

static void jump_to_main(void) __attribute__((noreturn));
static bool application_is_valid(void);
static void invalid_application_halt(void) __attribute__((noreturn));

static void gpio_setup(void){
	gpio_set_mode(GPIO_BANK_USART1_TX, GPIO_MODE_OUTPUT_50_MHZ,
		      GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART1_TX);
	gpio_set_mode(GPIO_BANK_USART1_RX, GPIO_MODE_INPUT,
		      GPIO_CNF_INPUT_FLOAT, GPIO_USART1_RX);

}
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

static void invalid_application_halt(void)
{
	for (;;) {
		__asm__("wfi");
	}
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

static void jump_to_main(void){
	typedef void (*void_fn)(void);
	uint32_t *app_vector_table = (uint32_t *)MAIN_APP_START_ADDRESS;
	uint32_t app_stack = app_vector_table[0];
	uint32_t app_reset = app_vector_table[1];
	void_fn jump_fn = (void_fn)app_reset;

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
	uart_setup();
	log_boot_diagnostics();

	if (!application_is_valid()) {
		uart_write_string("[bootloader] halting: application invalid\r\n");
		invalid_application_halt();
	}

	uart_write_string("[bootloader] application valid, starting comms test loop\r\n");

	// comms_setup();
	// gpio_setup();
	// comms_packet_t packet ={
	// 	.length=9,
	// 	.data={1,2,3,4,5,6,7,8,9,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
    //     .crc=0
	// } ;
	// packet.crc=comms_compute_crc(&packet);

	

	while(true){
	

	}
	jump_to_main();
	return 0;
}

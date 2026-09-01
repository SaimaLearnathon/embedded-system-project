#include "common-defines.h"
#include <libopencm3/cm3/scb.h>
#include <libopencm3/stm32/memorymap.h>

#define BOOTLOADER_SIZE (0x8000U)
#define MAIN_APP_START_ADDRESS (FLASH_BASE + BOOTLOADER_SIZE)
#define FLASH_END_ADDRESS (FLASH_BASE + (256U * 1024U))
#define SRAM_BASE_ADDRESS (0x20000000U)
#define SRAM_END_ADDRESS (SRAM_BASE_ADDRESS + (64U * 1024U))

static void jump_to_main(void) __attribute__((noreturn));
static bool application_is_valid(void);
static void invalid_application_halt(void) __attribute__((noreturn));

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
	if (!application_is_valid()) {
		invalid_application_halt();
	}

	jump_to_main();
	return 0;
}

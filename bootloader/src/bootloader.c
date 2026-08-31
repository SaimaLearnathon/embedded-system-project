#include "common-defines.h"
#include <libopencm3/cm3/scb.h>
#include <libopencm3/stm32/memorymap.h>

#define BOOTLOADER_SIZE (0x8000U)
#define MAIN_APP_START_ADDRESS (FLASH_BASE + BOOTLOADER_SIZE)

static void jump_to_main(void) __attribute__((noreturn));

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
	jump_to_main();
	return 0;
}

#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/memorymap.h>
#include "bl-flash.h"

#define BOOTLOADER_SIZE (0x8000U)
#define MAIN_APP_START_ADDRESS (FLASH_BASE + BOOTLOADER_SIZE)
#define FLASH_END_ADDRESS (FLASH_BASE + (256U * 1024U))
#define FLASH_PAGE_SIZE (2048U)

void bl_flash_erase_main_application(void){

    flash_unlock();

    for (uint32_t page_address = MAIN_APP_START_ADDRESS;
         page_address < FLASH_END_ADDRESS;
         page_address += FLASH_PAGE_SIZE) {
        flash_erase_page(page_address);
    }

    flash_lock();
}

void bl_flash_write(const uint32_t address, const uint8_t *data, const uint32_t length){
    flash_unlock();

    for (uint32_t i = 0; i < length; i += 2U) {
        uint16_t half_word = data[i];

        if ((i + 1U) < length) {
            half_word |= ((uint16_t)data[i + 1U] << 8U);
        } else {
            half_word |= 0xff00U;
        }

        flash_program_half_word(address + i, half_word);
    }

    flash_lock();
}

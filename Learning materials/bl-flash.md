# Bootloader flash writing notes

This note explains `bootloader/src/bl-flash.c` from the ground up.

The job of this file is small but very important:

- erase the part of flash where the main application lives
- write new firmware bytes into flash
- keep the bootloader area protected by only touching the application area

In this project, the bootloader uses the `libopencm3` flash functions instead
of writing directly to STM32 flash control registers.

## Why we need flash write

The bootloader needs flash writing because firmware updates arrive as bytes, but
the microcontroller runs programs from flash memory.

When the STM32 resets, it starts executing code from flash, not from RAM. In
this project, the flash is split like this:

```text
0x08000000  -------------------------------
            | Bootloader                  |
0x08008000  -------------------------------
            | Main application            |
```

If a PC tool sends a new application over UART, the bootloader can receive those
bytes, but receiving them is not enough. The bootloader must permanently store
them in the application area of flash.

The update flow looks like this:

```text
PC/fw-updater sends firmware bytes
        |
        v
bootloader receives bytes
        |
        v
bootloader erases old application flash
        |
        v
bootloader writes the new bytes into flash
        |
        v
bootloader can jump to the new application
```

Without `bl_flash_write()`, the bootloader could receive firmware data, but it
could not save it. If the data only stayed in RAM, it would disappear when the
board reset or lost power.

So this function is what turns received firmware bytes into a real installed
application.

You would not need flash writing for a very simple bootloader that only jumps to
an application that was already flashed by a debugger. You need it when the
bootloader itself is responsible for installing or updating the application.

## The file

```c
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
```

## Why flash needs special code

Flash memory is not like RAM.

In RAM, the CPU can usually write one byte directly:

```c
buffer[0] = 0x12;
```

Flash has stricter rules:

- flash must be unlocked before writing
- flash must usually be erased before programming
- erase happens in pages, not single bytes
- on STM32F1, programming is done in half-words, which means 16 bits or 2 bytes
- after writing, flash should be locked again

This is why the bootloader needs helper functions instead of treating flash
like a normal array.

## Include lines

```c
#include <libopencm3/stm32/flash.h>
```

This gives access to flash helper functions from `libopencm3`, such as:

- `flash_unlock()`
- `flash_lock()`
- `flash_erase_page()`
- `flash_program_half_word()`

These functions hide the lower-level register details.

```c
#include <libopencm3/stm32/memorymap.h>
```

This gives memory address constants for the STM32. The important one here is:

```c
FLASH_BASE
```

For STM32F1, flash starts at:

```text
0x08000000
```

So `FLASH_BASE` represents the beginning of internal flash memory.

```c
#include "bl-flash.h"
```

This includes this module's own header file.

The header declares the functions that other files are allowed to call:

```c
void bl_flash_erase_main_application(void);
void bl_flash_write(const uint32_t address, const uint8_t *data, const uint32_t length);
```

The `.c` file contains the real function bodies.

The `.h` file tells the rest of the project those functions exist.

## Flash layout constants

### Bootloader size

```c
#define BOOTLOADER_SIZE (0x8000U)
```

`0x8000` is hexadecimal.

In decimal:

```text
0x8000 = 32768 bytes = 32 KiB
```

So this project reserves the first 32 KiB of flash for the bootloader.

The `U` means unsigned. It tells the compiler this number should be treated as
an unsigned integer constant.

### Main application start address

```c
#define MAIN_APP_START_ADDRESS (FLASH_BASE + BOOTLOADER_SIZE)
```

The STM32 flash starts at:

```text
FLASH_BASE = 0x08000000
```

The bootloader takes:

```text
BOOTLOADER_SIZE = 0x8000
```

So the main application starts at:

```text
0x08000000 + 0x8000 = 0x08008000
```

Memory layout:

```text
0x08000000  -------------------------------
            | Bootloader, 32 KiB          |
0x08008000  -------------------------------
            | Main application            |
            |                             |
0x08040000  -------------------------------
```

The bootloader lives at the start because the CPU begins execution from the
start of flash after reset.

The application lives after the bootloader.

### Flash end address

```c
#define FLASH_END_ADDRESS (FLASH_BASE + (256U * 1024U))
```

This project targets an STM32F103 with 256 KiB of flash.

```text
256 * 1024 = 262144 bytes
```

So:

```text
FLASH_END_ADDRESS = 0x08000000 + 0x40000
FLASH_END_ADDRESS = 0x08040000
```

Important detail: this is the first address after flash, not the last valid
flash byte.

That means the valid flash range is:

```text
0x08000000 up to 0x0803FFFF
```

And `0x08040000` is used as a stopping point.

### Flash page size

```c
#define FLASH_PAGE_SIZE (2048U)
```

This says each flash page is 2048 bytes, or 2 KiB.

Flash erase happens one page at a time.

So to erase the full application area, the code steps through flash in 2048-byte
jumps.

## Function: erase the main application

```c
void bl_flash_erase_main_application(void){
```

This function erases the application area of flash.

It does not erase the bootloader area.

That is important because if the bootloader erased itself, the board could lose
the code that receives and writes new firmware.

### Unlock flash

```c
flash_unlock();
```

STM32 flash is locked by default to prevent accidental writes or erases.

Before erasing or programming flash, the code must unlock it.

Think of this as entering "flash modification mode".

### Loop through application pages

```c
for (uint32_t page_address = MAIN_APP_START_ADDRESS;
     page_address < FLASH_END_ADDRESS;
     page_address += FLASH_PAGE_SIZE) {
    flash_erase_page(page_address);
}
```

This loop starts at the beginning of the application area:

```text
0x08008000
```

It stops before:

```text
0x08040000
```

Each loop moves forward by one flash page:

```text
2048 bytes
```

On each pass:

```c
flash_erase_page(page_address);
```

erases the page that starts at `page_address`.

The first few page addresses are:

```text
0x08008000
0x08008800
0x08009000
0x08009800
...
```

Because:

```text
2048 decimal = 0x800 hex
```

After erase, flash bytes usually read as:

```text
0xFF
```

That matters because flash programming can normally change bits from `1` to `0`,
but not freely back from `0` to `1`. Erase resets the page back to all `1`s.

### Lock flash again

```c
flash_lock();
```

After erasing, the code locks flash again.

This reduces the chance that a bug elsewhere accidentally writes to flash.

## Function: write bytes into flash

```c
void bl_flash_write(const uint32_t address, const uint8_t *data, const uint32_t length){
```

This function writes `length` bytes from `data` into flash starting at `address`.

The parameters are:

| Parameter | Meaning |
|---|---|
| `address` | flash address where writing should begin |
| `data` | pointer to bytes that should be written |
| `length` | number of bytes to write |

Example idea:

```c
uint8_t bytes[] = {0x11, 0x22, 0x33, 0x44};
bl_flash_write(0x08008000, bytes, 4);
```

This would write:

```text
0x11 0x22 0x33 0x44
```

starting at:

```text
0x08008000
```

### Unlock flash

```c
flash_unlock();
```

Writing flash also requires unlocking first.

### Write in steps of 2 bytes

```c
for (uint32_t i = 0; i < length; i += 2U) {
```

STM32F1 flash programming writes a half-word at a time.

A half-word is:

```text
16 bits = 2 bytes
```

That is why the loop increases `i` by `2`.

For `length = 6`, `i` becomes:

```text
0, 2, 4
```

For `length = 5`, `i` becomes:

```text
0, 2, 4
```

The last write in the 5-byte case has only one real byte left, so the code must
handle that carefully.

### Start building one half-word

```c
uint16_t half_word = data[i];
```

`data[i]` is one byte.

`half_word` is 16 bits wide.

This line puts the first byte into the lower 8 bits of the half-word.

Example:

```text
data[i] = 0x34
```

Then:

```text
half_word = 0x0034
```

### If a second byte exists

```c
if ((i + 1U) < length) {
    half_word |= ((uint16_t)data[i + 1U] << 8U);
}
```

This checks whether there is another byte available after `data[i]`.

If yes, that second byte is placed into the upper 8 bits of the half-word.

Example:

```text
data[i]     = 0x34
data[i + 1] = 0x12
```

The first byte starts as:

```text
half_word = 0x0034
```

The second byte is shifted left by 8 bits:

```text
0x12 << 8 = 0x1200
```

Then the OR operation combines them:

```text
0x0034 | 0x1200 = 0x1234
```

So these two bytes become one 16-bit flash write:

```text
0x1234
```

### Why the byte order looks reversed

The code writes:

```c
half_word = data[i] | (data[i + 1] << 8);
```

So the first byte goes into the low part, and the second byte goes into the high
part.

This matches little-endian memory order used by ARM Cortex-M.

If the bytes are:

```text
0x34 0x12
```

The 16-bit value is:

```text
0x1234
```

But in memory, it is stored as:

```text
low byte first, high byte second
```

So memory still contains:

```text
0x34 0x12
```

### If there is no second byte

```c
else {
    half_word |= 0xff00U;
}
```

This handles odd-length writes.

Example:

```text
length = 5
```

The first four bytes make two normal half-word writes:

```text
byte 0 + byte 1
byte 2 + byte 3
```

Then byte 4 is left by itself.

Flash still needs a 16-bit half-word write, so the code fills the missing upper
byte with:

```text
0xFF
```

Example:

```text
data[i] = 0xAB
```

Then:

```text
half_word = 0x00AB
half_word |= 0xFF00
half_word = 0xFFAB
```

Why use `0xFF` for the missing byte?

Because erased flash already contains `0xFF`. Writing `0xFF` into the unused
byte leaves it in the erased state.

### Program the half-word

```c
flash_program_half_word(address + i, half_word);
```

This writes the 16-bit value into flash.

The destination address is:

```text
address + i
```

If `address = 0x08008000`, the writes go to:

```text
i = 0  -> 0x08008000
i = 2  -> 0x08008002
i = 4  -> 0x08008004
```

Each write stores 2 bytes.

### Lock flash

```c
flash_lock();
```

After writing, flash is locked again.

## Full example

Suppose this is the data:

```c
uint8_t data[] = {0x11, 0x22, 0x33, 0x44, 0x55};
```

And the call is:

```c
bl_flash_write(0x08008000, data, 5);
```

The loop writes:

| Loop `i` | Bytes available | Half-word built | Flash address |
|---|---|---|---|
| `0` | `0x11`, `0x22` | `0x2211` | `0x08008000` |
| `2` | `0x33`, `0x44` | `0x4433` | `0x08008002` |
| `4` | `0x55` only | `0xFF55` | `0x08008004` |

The actual bytes in flash become:

```text
0x11 0x22 0x33 0x44 0x55 0xFF
```

That extra `0xFF` is padding for the final half-word.

## Important assumptions in this code

This file is intentionally simple. It assumes some things are already true.

### The target area was erased first

Before writing a new application, the bootloader should erase the application
area:

```c
bl_flash_erase_main_application();
```

Then it can write the new bytes:

```c
bl_flash_write(MAIN_APP_START_ADDRESS, firmware_data, firmware_length);
```

Writing without erasing first can fail or produce corrupted data.

### The address should be half-word aligned

Because STM32F1 writes half-words, the destination address should usually be
even:

```text
0x08008000 good
0x08008002 good
0x08008001 bad
```

This code does not check alignment. It expects the caller to pass a valid flash
address.

### The address should stay inside the application area

This code does not check whether `address` points inside the main application
region.

That means the caller must avoid passing addresses inside the bootloader.

For safety, a future version could check:

```c
address >= MAIN_APP_START_ADDRESS
```

and:

```c
address + length <= FLASH_END_ADDRESS
```

That would reduce the chance of accidentally overwriting the bootloader.

### The data pointer must be valid

The function expects:

```c
data != NULL
```

This code does not check for a null pointer.

That is common in small embedded helper functions, but it means the caller must
be careful.

## How this fits into a firmware update

A typical bootloader update flow is:

1. Receive firmware bytes from the host over UART.
2. Check packet CRCs while receiving.
3. Erase the old application area.
4. Write the new firmware into the application area.
5. Verify the full firmware image.
6. Jump to the application.

This file handles steps 3 and 4.

Other files handle communication, packet parsing, CRC, startup, and jumping.

## Key things to remember

- Flash starts at `0x08000000`.
- The bootloader uses the first 32 KiB.
- The application starts at `0x08008000`.
- The target chip has 256 KiB flash, ending at `0x08040000`.
- Flash erase happens by page.
- This project uses 2 KiB flash pages.
- STM32F1 flash programming happens in 16-bit half-words.
- `bl_flash_write()` converts byte data into half-word writes.
- Odd-length data is padded with `0xFF`.
- Flash should be unlocked before erase/write and locked afterward.

## Related files

- [Flash implementation](../bootloader/src/bl-flash.c)
- [Flash interface](../bootloader/inc/bl-flash.h)
- [Bootloader main file](../bootloader/src/bootloader.c)
- [Bootloader skeleton notes](bootloader_skeleton.md)

# Bootloader Skeleton Notes

This note collects the main ideas we have learned from the current bootloader setup in this project.

## Big picture

This project is split into **two parts**:

- **Bootloader**
  Lives at the beginning of flash.
- **Application**
  Lives after the bootloader.

The reason for this split is simple:

- the bootloader starts first after reset
- then it decides whether to stay in boot mode or jump to the main app

Right now, this project has a **very small skeleton bootloader**. It does not yet do update logic, UART reception, image verification, or rollback. It mainly proves that:

- the flash is divided correctly
- the app is placed after the bootloader
- the bootloader can jump into the app
- the app can run with its own vector table

## Flash layout

The important constant is:

```c
#define BOOTLOADER_SIZE (0x8000)
```

`0x8000` bytes = `32 KB`

That means the memory is arranged like this:

```text
0x08000000  ---------------------------------
            | Bootloader (32 KB)            |
0x08008000  ---------------------------------
            | Main application              |
            | (rest of flash)               |
            |                               |
0x08040000  ---------------------------------   (for a 256 KB device)
```

So:

- bootloader start address = `0x08000000`
- app start address = `0x08008000`

This matches the flash command we saw:

```text
st-flash write firmware.bin 0x08008000
```

That command writes the app at the app start address, not at the beginning of flash.

## What the bootloader linker script does

In `bootloader/linkerscript.ld`, the bootloader flash region is:

```ld
rom (rx)  : ORIGIN = 0x08000000, LENGTH = 32K
```

This means:

- the bootloader is linked to run from the beginning of flash
- it is limited to 32 KB

So the bootloader owns only the first part of flash.

## What the application linker script does

In [`app/linkerscript.ld`](/abs/path not available), the app flash region is:

```ld
rom (rx)  : ORIGIN = 0x08008000, LENGTH = 224K
```

This means:

- the application is linked to start at `0x08008000`
- it must assume the first 32 KB are already occupied by the bootloader

This is one of the most important bootloader ideas:

**the app must be built for its real flash address**

If the app were linked for `0x08000000` but flashed at `0x08008000`, reset vectors, code addresses, and interrupts would be wrong.

## The bootloader jump code

The current bootloader code is:

```c
#define BOOTLOADER_SIZE (0x8000)
#define MAIN_APP_START_ADDRESS (FLASH_BASE + BOOTLOADER_SIZE)

static void jump_to_main(void){

typedef void (*void_fn)(void);
	
uint32_t* reset_vector_entry = (uint32_t*) (MAIN_APP_START_ADDRESS + 4U);
uint32_t* reset_vector = (uint32_t*)(*reset_vector_entry);
void_fn jump_fn=(void_fn) reset_vector;
jump_fn();
}
```

### What this means

At the start of every Cortex-M vector table:

- word 0 = initial stack pointer
- word 1 = reset handler address

So:

- `MAIN_APP_START_ADDRESS + 0` points to the app's initial stack pointer
- `MAIN_APP_START_ADDRESS + 4` points to the app's reset handler entry

This code reads the app reset handler address, converts it into a function pointer, and jumps to it.

In plain words:

**the bootloader says: "start executing the application now."**

## One important detail about a full jump

The current skeleton jump is intentionally minimal. A more complete bootloader handoff usually also does things like:

- load the application's stack pointer into `MSP`
- disable or clear pending interrupts
- reset peripheral state if needed
- relocate the vector table

So the current version is a good learning skeleton, but not yet a production-grade handoff.

## Why the application needs `vector_setup()`

In the app we added:

```c
static void vector_setup(void){
	SCB_VTOR = BOOTLOADER_SIZE;
}
```

This exists because the app does **not** live at the beginning of flash.

Normally, after reset, the processor expects the vector table near the flash base. But our app is stored after the bootloader, at:

```text
0x08008000
```

So the app must tell the processor:

**"Use my interrupt vector table from the application location."**

That is what `SCB_VTOR` does.

Without this step, interrupts such as:

- SysTick
- EXTI
- USART
- timers

could use the wrong vector table and jump to the wrong handlers.

## Simple meaning of `SCB_VTOR = BOOTLOADER_SIZE`

This line means:

- the vector table is offset by `0x8000`
- the app's interrupt table begins after the bootloader

So we added it because:

**the app is not at flash address zero anymore**

That is the key reason.

## Why the app still starts even though the bootloader jumps directly

The bootloader jumps into the application's reset handler.

Then the app startup code runs as usual:

1. initialize memory sections like `.data` and `.bss`
2. call early startup/runtime code
3. reach `main()`

Inside `main()`, the app calls:

```c
vector_setup();
system_setup();
gpio_setup();
```

This means the app corrects the vector table early in its own startup path before normal interrupt-driven behavior is relied on.

## Why the bootloader binary is embedded into the app build

There is a small assembly file:

```asm
.section .bootloader-section
   .incbin "../bootloader/bootloader.bin"
```

This includes the raw bootloader binary inside the application image.

The app linker script keeps it with:

```ld
KEEP (*(.bootloader-section))
```

So the final app image contains the bootloader bytes at the beginning of flash layout.

This is useful because one produced image can represent:

- bootloader region
- application region

and can help program a complete combined image.

## Why the bootloader binary is padded

The script `bootloader/pad-bootloader.py` pads `bootloader.bin` to exactly `0x8000` bytes using `0xFF`.

That matters because:

- the bootloader region is fixed at 32 KB
- the app starts immediately after that region
- padding makes the layout exact and predictable

In flash memory, erased bytes are typically `0xFF`, so this padding matches normal flash behavior.

## What has already been proven by this skeleton

From this setup, we have already learned these core bootloader concepts:

1. A bootloader is just code placed at the beginning of flash that runs first.
2. The application can be linked to a later flash address.
3. The bootloader can transfer control by jumping to the application's reset handler.
4. The application must use its own vector table, not the bootloader's.
5. The linker script is what makes the flash split real.
6. Flash addresses in the build must match flash addresses used while programming.

## What this skeleton does not do yet

This is important so we do not confuse a learning skeleton with a finished bootloader.

Right now it does **not yet**:

- validate whether the app image is present
- check a checksum or signature
- receive a firmware update over UART/USB/CAN
- erase and rewrite the application area
- keep backup images
- recover from a broken update
- decide between boot mode and app mode using a button, flag, or timeout

So this is the foundation, not the final system.

## The startup flow in one simple sequence

Here is the current idea from reset to blinking LED:

1. MCU resets.
2. Bootloader starts at `0x08000000`.
3. Bootloader reads the application's reset handler from `0x08008004`.
4. Bootloader jumps to the application reset handler.
5. Application startup code runs.
6. `main()` starts.
7. `vector_setup()` points interrupts to the app vector table.
8. The app configures SysTick/GPIO and blinks the LED.

## The most important lesson

The most important thing we learned is this:

**A bootloader is not magic.**

It is mainly a careful arrangement of:

- flash addresses
- linker scripts
- vector tables
- jump logic

If those parts agree with each other, the bootloader and app can coexist cleanly.

## Short summary

This project's current bootloader skeleton shows a minimal but real boot flow:

- reserve the first 32 KB for the bootloader
- place the app at `0x08008000`
- jump from bootloader to app reset handler
- relocate the app vector table with `SCB_VTOR`

That is the core idea behind the bootloader skeleton you have built so far.

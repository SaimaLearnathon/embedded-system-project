# STM32 Bare-Metal Learning Notes
*Compiled from setup + first blinky episode — Low Byte Productions Bare Metal Series, adapted for STM32F103 (High-Density variant, 256KB flash / 64KB RAM)*

## Contents
**Part A — Setup**
1. Hardware Summary & Wiring
2. Toolchain & Software Setup (+ troubleshooting log)

**Part B — Reference Material**
3. Reference Manual (RM0008) — What to Read, and When
4. Series Feasibility on This Board
5. F1 vs F4 — Key Differences When Adapting Tutorial Code

**Part C — Core Concepts**
6. Memory: Flash vs SRAM
7. Memory-Mapped I/O — How Peripherals Are Addressed
8. RCC — Clocks Must Be Enabled First

**Part D — GPIO Deep Dive**
9. Why LED_PORT and LED_PIN Are Both Needed
10. GPIO Configuration — `gpio_set_mode()` (MODE / CNF fields)
11. Why GPIO Has Two Config Registers — CRL and CRH
12. Inside `gpio_set_mode()` — Full Source Walkthrough
13. Setting Pin State — `gpio_set()` / `gpio_clear()`
14. The Blink Loop (putting it all together)

**Part E — Looking Ahead: Interrupts & Timing**
15. Interrupt Vector Table
16. SysTick Timer

---
---

# Part A — Setup

## 1. Hardware Summary & Wiring

**Board:** STM32F103 High-Density (chipid `0x414`, dev-type `F1xx_HD`)
- Flash: 256KB (much more than a standard BluePill's 64KB)
- SRAM: 64KB (vs standard BluePill's 20KB)
- Core: Cortex-M3, no FPU

**Debug probe:** ST-Link V2

### Wiring (ST-Link ↔ target board)
| ST-Link pin | Target pin |
|---|---|
| SWDIO (pin 4) | SWIO |
| SWCLK (pin 2) | SWCLK |
| GND (pin 5 or 6) | GND |
| 3.3V (pin 7 or 8) | 3V3 |

⚠️ Don't confuse the 3.3V pins (7/8) with the 5.0V pins (9/10) — feeding 5V into a 3.3V-rated pin can damage the chip.

---

## 2. Toolchain & Software Setup

| Tool | Purpose | Windows source |
|---|---|---|
| `arm-none-eabi-gcc` | Cross-compiler for Cortex-M | https://developer.arm.com/downloads/-/gnu-rm |
| `stlink-tools` (`st-flash`, `st-info`, `st-util`) | Flash & probe via ST-Link | https://github.com/stlink-org/stlink/releases |
| `libusb-1.0.dll` | USB comms dependency for stlink-tools | https://github.com/libusb/libusb/releases |
| libopencm3 | Peripheral register library | Built via `make -C libopencm3 TARGETS=stm32/f1` |
| OpenOCD (optional alt. to stlink-tools) | Debug server | https://openocd.org/ |

### Build & flash commands
```powershell
# Build libopencm3 for F1 only (one-time)
make -C libopencm3 TARGETS=stm32/f1

# Build your app
make -C app

# Flash via ST-Link
st-flash write firmware.bin 0x08000000
```

### Troubleshooting log (useful if setup breaks again)
Worked through in order — each one caused the next symptom until fixed:

1. **`st-info --version` silent, `--probe` crashes (exit code `-1073741515` / `0xC0000135`)**
   → missing `libusb-1.0.dll`. Copy the correct-architecture DLL (match `st-info.exe`'s bitness — check with a PowerShell PE-header read) into the stlink `bin` folder.

2. **Exit code `-1073741701` (`0xC000007B`)**
   → architecture mismatch between the DLL and the exe (32-bit DLL with 64-bit exe, or vice versa). Use the matching MinGW64/x64 folder from the libusb package.

3. **`st-info` runs but "Found 0 stlink programmers"**
   → USB driver problem. Check `Get-PnpDevice | Where FriendlyName -like "*STLink*"` — if Status shows `Error`, fix with **Zadig** (https://zadig.akeo.ie/): select the ST-Link device, install the **WinUSB** driver.

4. **`Found 1 stlink programmers` but `flash: 0, sram: 0`, plus `chips: No such file or directory`**
   → the chip definition database (`.chip` files) is missing from the expected path. Copy them from the extracted package into `C:\Program Files (x86)\stlink\config\chips` (requires Admin PowerShell).

5. **`Failed to enter SWD mode`**
   → target board isn't wired/powered yet, or wiring is wrong (SWDIO/SWCLK swapped, no GND, no power). Fixed by physically wiring ST-Link to the target's SWD header — after that, `st-info --probe` returned real `flash`/`sram`/`chipid` values.

---
---

# Part B — Reference Material

## 3. Reference Manual (RM0008) — What to Read, and When

Don't read cover-to-cover. Read each chapter right before the episode that needs it:

1. **Memory Map** — peripheral base addresses (needed immediately)
2. **RCC chapter** — clock enable bits, PLL/HSE setup (needed immediately)
3. **GPIO chapter** — `CRL`/`CRH`, `ODR`, `IDR`, `BSRR`/`BRR` (needed immediately)
4. **AFIO chapter** — pin remapping, freeing SWD pins for other uses (needed when combining SWD + peripherals)
5. **NVIC / interrupts** (from ARM's Cortex-M3 docs, not RM0008) — needed for timer/UART interrupts
6. **USART chapter** — when you reach UART episodes
7. **Timers chapter** — when you reach PWM/timer episodes
8. **Flash/FPEC chapter** — save for bootloader episodes (flash unlock/erase/write sequence)

### Manual links
- **STM32F1 Reference Manual (RM0008)**: https://www.st.com/resource/en/reference_manual/rm0008-stm32f101xx-stm32f102xx-stm32f103xx-stm32f105xx-and-stm32f107xx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf
- **Cortex-M3 Technical Reference Manual** (core-level: exceptions, NVIC, SysTick, debug architecture): https://www.keil.com/dd/docs/datashts/arm/cortex_m3/r1p1/ddi0337e_cortex_m3_r1p1_trm.pdf
- **Cortex-M3 Devices Generic User Guide** (more approachable, practical): https://developer.arm.com/documentation/dui0552/latest/
- **ARMv7-M Architecture Reference Manual** (deepest spec — instruction set, memory model): https://developer.arm.com/documentation/ddi0403/latest/

Use RM0008 for almost everything day-to-day. Reach for the Cortex-M3 docs specifically once you hit NVIC/interrupts or SysTick.

---

## 4. Series Feasibility on This Board

- ✅ GPIO, timers, PWM, UART, basic bootloader (UART-based), crypto/signing — all fully doable, comfortably (256KB flash / 64KB RAM gives plenty of headroom, more than the original F401RE Nucleo's RAM even).
- ⚠️ Any **USB-based firmware update** episode — F103's USB peripheral is architecturally different from F4's OTG_FS core; this needs a from-scratch rewrite, not a simple port. Substitute UART or treat as conceptual-only when reached.

---

## 5. F1 vs F4 — Key Differences When Adapting Tutorial Code

| Aspect | F1 (your board) | F4 (tutorial's board) |
|---|---|---|
| GPIO config registers | `CRL`/`CRH` (4 bits/pin, mode+speed+type combined) | `MODER`/`OSPEEDR`/`OTYPER` (separate registers) |
| Clock setup function | `rcc_clock_setup_pll(&rcc_hse_configs[...])` | `rcc_clock_setup_pll(&rcc_hsi_configs[...])` |
| FPU | None (Cortex-M3) | Present (Cortex-M4F) |
| USB peripheral | Older "USB FS device" — different registers entirely | OTG_FS core (Synopsys) |
| Reference manual | RM0008 | RM0090 |

---
---

# Part C — Core Concepts

## 6. Memory: Flash vs SRAM

**Flash memory**
- Non-volatile — keeps contents when powered off
- Stores your compiled program (code + constants)
- Starts at address `0x08000000` on STM32
- Slower to write than RAM; limited erase/write cycles
- Relevant later for the bootloader (which erases/rewrites flash while running)

**SRAM**
- Volatile — wiped on power loss/reset
- Stores variables, the stack, the heap while the program runs
- Fast read/write
- Bootloaders buffer incoming firmware data here before writing it to flash

---

## 7. Memory-Mapped I/O — How Peripherals Are Addressed

Peripherals aren't special CPU instructions — they're just **fixed memory addresses** that hardware "listens" to.

Example: `GPIOC` address breakdown
```
0x40000000   ← PERIPH_BASE (start of all peripheral memory)
+ 0x10000    ← APB2PERIPH_BASE (APB2 bus region start)
+ 0x1000     ← GPIOC's specific offset within APB2
= 0x40011000 ← final GPIOC base address
```
GPIO ports are spaced `0x400` apart: GPIOA (`+0x0800`), GPIOB (`+0x0C00`), GPIOC (`+0x1000`), GPIOD (`+0x1400`), etc.

Writing to this address doesn't touch RAM — the hardware bus routes it directly into the GPIO peripheral's internal registers, physically changing pin voltages.

---

## 8. RCC — Clocks Must Be Enabled First

**Key fact:** almost every peripheral starts powered OFF (no clock signal) to save power. Writing to a peripheral's registers before enabling its clock does nothing or behaves unpredictably.

```c
rcc_periph_clock_enable(RCC_GPIOC);
```
- Sets one specific bit in `RCC_APB2ENR` (GPIOC = bit 4)
- Must be called **before** any `gpio_set_mode()` or other GPIOC access
- Every peripheral (timers, UART, SPI, ADC...) needs this same step, just a different bit/register (`APB1ENR`, `APB2ENR`, or `AHBENR` depending on which bus it's on)

### System clock setup (PLL)
```c
rcc_clock_setup_pll(&rcc_hse_configs[RCC_CLOCK_HSE8_72MHZ]);
```
- **HSE** = High Speed External oscillator (the physical 8 MHz crystal on the board), more accurate than the internal HSI oscillator
- **PLL** = Phase-Locked Loop — multiplies the 8 MHz crystal up to 72 MHz (×9)
- Also sets internal bus prescalers (e.g., APB1 max is 36 MHz, so it's auto-divided)
- Must be correct before UART baud rates or precise timing will work correctly
- ⚠️ F1-specific function/array names — the F4 tutorial video uses different ones (`rcc_hsi_configs`, `RCC_CLOCK_3V3_84MHZ`) that don't exist for F1

---
---

# Part D — GPIO Deep Dive
*Everything below builds on one running example: configuring PC13 (the onboard LED) as a push-pull output.*

## 9. Why LED_PORT and LED_PIN Are Both Needed

- **`LED_PORT`** (`GPIOC`, address `0x40011000`) answers **"which GPIO bank?"** — one port's registers (`CRL`, `CRH`, `ODR`, `BSRR`, etc.) hold config/state for **16 pins at once**, packed as different bits within the same 32-bit registers.
- **`LED_PIN`** (`GPIO13`) answers **"which specific bit/pin within that port?"** — without it, the port address alone is ambiguous (like saying "flip a switch on this power strip" without saying which outlet).

Keeping them as separate `#define`s (rather than one combined identifier) allows reusing the same port with different pins, or the same pin number across different ports, with the same functions.

---

## 10. GPIO Configuration — `gpio_set_mode()`

```c
gpio_set_mode(LED_PORT, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, LED_PIN);
```

Each GPIO pin has **4 configuration bits** split into two register fields:
```
[ MODE (2 bits) ] [ CNF (2 bits) ]
```
Stored in `GPIOx_CRL` (pins 0–7) or `GPIOx_CRH` (pins 8–15) — see Section 11 for why it's split this way.

### MODE field (speed/direction)
| Value | Meaning |
|---|---|
| `00` | Input |
| `01` | Output, 10 MHz max |
| `10` | Output, 2 MHz max |
| `11` | Output, 50 MHz max |

Higher speed = faster switching but more electrical noise (EMI). Match speed to the task: LEDs/simple outputs → 2 MHz is plenty; fast buses like SPI → 10/50 MHz genuinely needed.

### CNF field — meaning depends on MODE
**If output:**
| Value | Meaning |
|---|---|
| `00` | Push-pull |
| `01` | Open-drain |
| `10` | AF push-pull |
| `11` | AF open-drain |

**If input:**
| Value | Meaning |
|---|---|
| `00` | Analog |
| `01` | Floating |
| `10` | Pull-up/pull-down |
| `11` | Reserved |

### Push-pull vs Open-drain
- **Push-pull**: pin actively drives both HIGH (3.3V) and LOW (0V) using two internal switches. Use for single-device outputs like an LED.
- **Open-drain**: pin can only pull LOW; reaching HIGH requires an external pull-up resistor that gently drags the line up when the pin "lets go." Used for shared buses (e.g. I2C) where multiple devices might drive the same wire — prevents short-circuit conflicts, since no device can force HIGH, only pull LOW or release.

---

## 11. Why GPIO Has Two Config Registers — CRL and CRH

Each pin needs **4 config bits** (2 for MODE + 2 for CNF). A 32-bit register only fits:
```
32 bits ÷ 4 bits per pin = 8 pins per register
```
But a port has **16 pins** total — one register isn't enough. ST splits the job:
- **`CRL`** (Configuration Register **Low**) — pins **0–7**
- **`CRH`** (Configuration Register **High**) — pins **8–15**

Pin 13 (the LED) falls in the 8–15 range, so it lives in `CRH`, not `CRL`.

**Why code always handles both registers, even when only one pin is being set:** `gpio_set_mode()` is written generically to handle *any* pin or combination of pins in one call (e.g. `GPIO5 | GPIO13`, spanning both halves) — so it always reads/writes both `CRL` and `CRH` to stay correct for every possible input, even though in the single-LED case `CRL` is read and written back completely unchanged.

---

## 12. Inside `gpio_set_mode()` — Full Source Walkthrough

```c
void gpio_set_mode(uint32_t gpioport, uint8_t mode, uint8_t cnf, uint16_t gpios)
{
	uint16_t i, offset = 0;
	uint32_t crl = 0, crh = 0, tmp32 = 0;

	crl = GPIO_CRL(gpioport);
	crh = GPIO_CRH(gpioport);

	for (i = 0; i < 16; i++) {
		if (!((1 << i) & gpios)) {
			continue;
		}

		offset = (i < 8) ? (i * 4) : ((i - 8) * 4);
		tmp32 = (i < 8) ? crl : crh;

		tmp32 &= ~(0xf << offset);
		tmp32 |= (mode << offset) | (cnf << (offset + 2));

		crl = (i < 8) ? tmp32 : crl;
		crh = (i >= 8) ? tmp32 : crh;
	}

	GPIO_CRL(gpioport) = crl;
	GPIO_CRH(gpioport) = crh;
}
```

**Signature:** `gpios` (plural, 16-bit) means this function can configure **multiple pins in one call** — each bit represents one pin (bit 13 = PC13). `GPIO13` is just a single bit set at position 13.

**Read current values first:**
```c
crl = GPIO_CRL(gpioport);
crh = GPIO_CRH(gpioport);
```
Preserves other pins' existing config, since these registers hold data for all 16 pins — the function must not clobber pins it wasn't asked to touch.

**Loop through all 16 possible pins:**
```c
for (i = 0; i < 16; i++) {
    if (!((1 << i) & gpios)) { continue; }
```
`(1 << i)` builds a mask with only bit `i` set. ANDing against `gpios` checks "was pin `i` actually requested?" If not, skip it. For a single-pin call like `GPIO13`, only `i == 13` does real work; the other 15 iterations just `continue`.

**Offset calculation (matches the CRL/CRH split from Section 11):**
```c
offset = (i < 8) ? (i * 4) : ((i - 8) * 4);
```
For pin 13: `(13 - 8) * 4 = 20` — its 4-bit slot starts at bit 20 within `CRH`.

**Pick which register (as a scratch copy) to modify:**
```c
tmp32 = (i < 8) ? crl : crh;
```
Pin 13 → `tmp32` gets loaded with `crh`.

**Clear then set — the actual bit manipulation:**
```c
tmp32 &= ~(0xf << offset);                              // clear this pin's 4-bit slot
tmp32 |= (mode << offset) | (cnf << (offset + 2));       // write mode + cnf into it
```
- `0xf` = `1111` binary (4 bits). Shifted to `offset` and inverted (`~`), it zeroes out exactly this pin's slot without touching neighboring pins.
- `mode` goes into the first 2 bits of the slot, `cnf` into the next 2 bits (`offset + 2`) — matching the `[MODE][CNF]` layout from Section 10.

**Write the modified scratch value back to the right local variable:**
```c
crl = (i < 8) ? tmp32 : crl;
crh = (i >= 8) ? tmp32 : crh;
```
Only `crh` actually changes for pin 13; `crl` stays as originally read.

**Final hardware write — happens once, after the loop:**
```c
GPIO_CRL(gpioport) = crl;
GPIO_CRH(gpioport) = crh;
```
Building the full desired value in local variables first, then writing to real hardware registers only once at the end, is more efficient than writing on every loop iteration.

**Big picture:** this design lets one function call handle both a simple single-pin case (`GPIO13`) and more advanced multi-pin cases (`GPIO13 | GPIO14 | GPIO15`) without separate code paths.

---

## 13. Setting Pin State — `gpio_set()` / `gpio_clear()`

These are different from `gpio_set_mode()` — mode = "how the pin behaves" (configured once), set/clear = "what voltage right now" (called repeatedly).

- `gpio_set(port, pin)` → pin HIGH (3.3V). Writes to lower half of `BSRR`.
- `gpio_clear(port, pin)` → pin LOW (0V). Writes to upper half of `BSRR`.

`BSRR` (Bit Set/Reset Register) is used instead of directly writing `ODR` because it allows **atomic** single-pin changes — no risk of accidentally disturbing other pins or racing with an interrupt.

**Analogy:** `gpio_set_mode()` = installing a light switch (once). `gpio_set()`/`gpio_clear()` = flipping it (many times).

---

## 14. The Blink Loop
*Putting Sections 9–13 together into working firmware.*

```c
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
```

- **`while (1)`** — infinite loop; no OS to return to, firmware must run forever.
- **Active-low LED**: BluePill's onboard LED lights when the pin is LOW, not HIGH — hence `gpio_clear` = ON, `gpio_set` = OFF (opposite of naive expectation).
- **`volatile uint32_t i`** — prevents the compiler from optimizing away the "pointless" counting loop.
- **`__asm__("nop")`** — inserts a real "do nothing" CPU instruction, extra insurance the loop isn't optimized out.
- **Busy-wait delay**: crude but simple — wastes CPU cycles on purpose to create a visible delay. Will later be replaced with hardware timers (see Part E).

---
---

# Part E — Looking Ahead: Interrupts & Timing
*Not needed yet at your current stage — kept here for when the series introduces these topics.*

## 15. Interrupt Vector Table

The vector table is a list of addresses stored at the very start of flash (`0x08000000` onward) that tells the Cortex-M3 core **where to jump** when a given exception or interrupt occurs. It's not something you "call" — the hardware automatically looks up the right entry and jumps there whenever an event happens.

### Structure
Each entry is a 4-byte address (a function pointer). The table is split into two parts:

**1. Core exceptions (fixed, defined by ARM — same on every Cortex-M3, not STM32-specific)**
| Offset | Exception | Purpose |
|---|---|---|
| `0x00` | Initial Stack Pointer | Not a handler — the starting SP value loaded at reset |
| `0x04` | Reset | Runs first on power-up/reset — jumps to your `main()` eventually |
| `0x08` | NMI | Non-Maskable Interrupt — can't be disabled |
| `0x0C` | HardFault | Catches serious faults (bad memory access, etc.) |
| `0x10` | MemManage | Memory Protection Unit violations |
| `0x14` | BusFault | Bus/memory access errors |
| `0x18` | UsageFault | Illegal instruction / undefined behavior |
| `0x2C` | SVCall | Supervisor call (used in OS context switching, RTOS) |
| `0x30` | Debug Monitor | Debug-related |
| `0x38` | PendSV | Often used for RTOS task switching |
| `0x3C` | **SysTick** | Fires when the SysTick timer reaches zero (see Section 16) |

**2. Peripheral interrupts (IRQs) — STM32F1-specific, starts at offset `0x40`**
These are chip-specific and listed in RM0008's interrupt/NVIC chapter — e.g. `EXTI0`, `TIM2`, `USART1`, `DMA1_Channel1`, etc. Each peripheral that can generate an interrupt has its own fixed slot in this table.

### How it connects to your code
- In libopencm3 projects, you don't normally edit the vector table by hand — it's auto-generated by the linker script and startup code.
- To actually *handle* an interrupt, you write a function with a specific matching name (e.g. `void systick_handler(void)` or `void usart1_isr(void)`), and the startup code's table automatically points to it. This is a **naming convention**, not something you register manually — get the function name wrong and your handler silently never runs.
- You'll first touch this concept for real once you enable SysTick or start using UART/timer interrupts.

### Where to read more
- Core exceptions (offsets `0x00`–`0x3C`): Cortex-M3 Technical Reference Manual (link in Section 3) — exception model chapter.
- Peripheral IRQs (offset `0x40` onward): RM0008, NVIC/interrupt chapter — has the full numbered list specific to F103.

---

## 16. SysTick Timer

**What it is:** a simple 24-bit **down-counting timer** built directly into the Cortex-M3 **core itself** — not a peripheral on the APB bus like GPIO/UART/Timers. This means it's documented in the ARM Cortex-M3 TRM, not RM0008, and it's identical across every Cortex-M3/M4 chip regardless of manufacturer. Your STM32F103 has it because it's a Cortex-M3 part.

**What it's for:** generating a regular, precise time-based interrupt — most commonly a **1ms tick** — used for delays, timekeeping, or task scheduling, without wasting CPU the way a busy-wait `nop` loop does (see Section 14).

**How it will likely replace your current blink delay:**
1. Configure SysTick's reload value so it counts down from a number that takes exactly 1ms at your system clock (72 MHz, from Section 8)
2. Enable its interrupt — it fires every time the counter reaches zero and auto-reloads
3. Inside that interrupt handler (matching the vector table naming convention above — typically `sys_tick_handler()` in libopencm3), increment a global millisecond counter
4. Write a `delay_ms(500)` function that just busy-waits on reading that counter until it reaches the target — still technically "waiting," but now based on an accurate, hardware-driven timebase instead of guessed loop iterations

**Relevant libopencm3 functions you'll likely see:**
- `systick_set_reload(value)` — sets the countdown starting value
- `systick_set_clocksource(...)` — selects clock source (usually the main system clock, sometimes divided by 8)
- `systick_interrupt_enable()` — enables the interrupt-on-zero behavior
- `systick_counter_enable()` — starts the timer running

**Why this matters going forward:** SysTick is core-level, not chip-family-specific, so this part of your code will look *identical* whether following the F4-based tutorial video or writing it for your F103 — one of the few areas where there's no F1-vs-F4 translation needed at all.
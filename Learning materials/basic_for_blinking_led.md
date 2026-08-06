# STM32 Bare-Metal Learning Notes
*Compiled from setup + first blinky episode — Low Byte Productions Bare Metal Series, adapted for STM32F103 (High-Density variant, 256KB flash / 64KB RAM)*

---

## 1. Hardware Setup Summary

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

### Troubleshooting steps we actually hit (useful if setup breaks again)
1. **`st-info --version` silent, `--probe` crashes (exit code `-1073741515` / `0xC0000135`)** → missing `libusb-1.0.dll`. Copy the correct-architecture DLL (match `st-info.exe`'s bitness — check with a PowerShell PE-header read) into the stlink `bin` folder.
2. **Exit code `-1073741701` (`0xC000007B`)** → architecture mismatch between the DLL and the exe (32-bit DLL with 64-bit exe, or vice versa). Use the matching MinGW64/x64 folder from the libusb package.
3. **`st-info` runs but "Found 0 stlink programmers"** → USB driver problem. Check `Get-PnpDevice | Where FriendlyName -like "*STLink*"` — if Status shows `Error`, fix with **Zadig** (https://zadig.akeo.ie/): select the ST-Link device, install the **WinUSB** driver.
4. **`Found 1 stlink programmers` but `flash: 0, sram: 0`, plus `chips: No such file or directory`** → the chip definition database (`.chip` files) is missing from the expected path. Copy them from the extracted package into `C:\Program Files (x86)\stlink\config\chips` (requires Admin PowerShell).
5. **`Failed to enter SWD mode`** → target board isn't wired/powered yet, or wiring is wrong (SWDIO/SWCLK swapped, no GND, no power). Fixed by physically wiring ST-Link to the target's SWD header.

### Build & flash commands
```powershell
# Build libopencm3 for F1 only (one-time)
make -C libopencm3 TARGETS=stm32/f1

# Build your app
make -C app

# Flash via ST-Link
st-flash write firmware.bin 0x08000000
```

---

## 3. Memory Concepts

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

## 4. Memory-Mapped I/O — How Peripherals Are Addressed

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

## 5. RCC — Clocks Must Be Enabled First

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

## 6. GPIO Configuration — `gpio_set_mode()`

```c
gpio_set_mode(LED_PORT, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, LED_PIN);
```

Each GPIO pin has **4 configuration bits** split into two register fields:
```
[ MODE (2 bits) ] [ CNF (2 bits) ]
```
Stored in `GPIOx_CRL` (pins 0–7) or `GPIOx_CRH` (pins 8–15).

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

## 7. Setting Pin State — `gpio_set()` / `gpio_clear()`

These are different from `gpio_set_mode()` — mode = "how the pin behaves" (configured once), set/clear = "what voltage right now" (called repeatedly).

- `gpio_set(port, pin)` → pin HIGH (3.3V). Writes to lower half of `BSRR`.
- `gpio_clear(port, pin)` → pin LOW (0V). Writes to upper half of `BSRR`.

`BSRR` (Bit Set/Reset Register) is used instead of directly writing `ODR` because it allows **atomic** single-pin changes — no risk of accidentally disturbing other pins or racing with an interrupt.

**Analogy:** `gpio_set_mode()` = installing a light switch (once). `gpio_set()`/`gpio_clear()` = flipping it (many times).

---

## 8. The Blink Loop

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
- **Busy-wait delay**: crude but simple — wastes CPU cycles on purpose to create a visible delay. Will later be replaced with hardware timers (more precise, doesn't block the CPU).

---

## 9. F1 vs F4 — Key Differences to Remember When Adapting Tutorial Code

| Aspect | F1 (your board) | F4 (tutorial's board) |
|---|---|---|
| GPIO config registers | `CRL`/`CRH` (4 bits/pin, mode+speed+type combined) | `MODER`/`OSPEEDR`/`OTYPER` (separate registers) |
| Clock setup function | `rcc_clock_setup_pll(&rcc_hse_configs[...])` | `rcc_clock_setup_pll(&rcc_hsi_configs[...])` |
| FPU | None (Cortex-M3) | Present (Cortex-M4F) |
| USB peripheral | Older "USB FS device" — different registers entirely | OTG_FS core (Synopsys) |
| Reference manual | RM0008 | RM0090 |

---

## 10. Reference Manual (RM0008) — What to Read, and When

Don't read cover-to-cover. Read each chapter right before the episode that needs it:

1. **Memory Map** — peripheral base addresses (needed immediately)
2. **RCC chapter** — clock enable bits, PLL/HSE setup (needed immediately)
3. **GPIO chapter** — `CRL`/`CRH`, `ODR`, `IDR`, `BSRR`/`BRR` (needed immediately)
4. **AFIO chapter** — pin remapping, freeing SWD pins for other uses (needed when combining SWD + peripherals)
5. **NVIC / interrupts** (from ARM's PM0056, not RM0008) — needed for timer/UART interrupts
6. **USART chapter** — when you reach UART episodes
7. **Timers chapter** — when you reach PWM/timer episodes
8. **Flash/FPEC chapter** — save for bootloader episodes (flash unlock/erase/write sequence)

---

## 11. Why LED_PORT and LED_PIN Are Both Needed

- **`LED_PORT`** (`GPIOC`, address `0x40011000`) answers **"which GPIO bank?"** — one port's registers (`CRL`, `CRH`, `ODR`, `BSRR`, etc.) hold config/state for **16 pins at once**, packed as different bits within the same 32-bit registers.
- **`LED_PIN`** (`GPIO13`) answers **"which specific bit/pin within that port?"** — without it, the port address alone is ambiguous (like saying "flip a switch on this power strip" without saying which outlet).

Keeping them as separate `#define`s (rather than one combined identifier) allows reusing the same port with different pins, or the same pin number across different ports, with the same functions.

---

## 12. Why GPIO Has Two Config Registers — CRL and CRH

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

## 13. Inside `gpio_set_mode()` — Full Source Walkthrough

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

**Offset calculation (matches the CRL/CRH split above):**
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
- `mode` goes into the first 2 bits of the slot, `cnf` into the next 2 bits (`offset + 2`) — matching the `[MODE][CNF]` layout described in Section 6.

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

## 14. Series Feasibility on This Board

- ✅ GPIO, timers, PWM, UART, basic bootloader (UART-based), crypto/signing — all fully doable, comfortably (256KB flash / 64KB RAM gives plenty of headroom, more than the original F401RE Nucleo's RAM even).
- ⚠️ Any **USB-based firmware update** episode — F103's USB peripheral is architecturally different from F4's OTG_FS core; this needs a from-scratch rewrite, not a simple port. Substitute UART or treat as conceptual-only when reached.

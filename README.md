# Embedded_system

Bare-metal firmware experiments for the **STM32F103** (Cortex-M3), targeting the
[BluePill](https://stm32-base.org/boards/board-STM32F103C8T6-STM32F103CBT6-Blue-Pill.html)
board (or the high-density STM32F103 variant). The project uses the
[libopencm3](https://github.com/libopencm3/libopencm3) peripheral register
library and is built with a standard GNU Arm cross-toolchain.

## Project layout

```
.
├── app/
│   ├── Makefile              # Build rules (arm-none-eabi-gcc + libopencm3)
│   ├── linkerscript.ld       # STM32F103C8T6 memory map (64K flash, 20K RAM)
│   ├── inc/                  # Application headers (common-defense.h, ...)
│   └── src/firmware.c        # First sample: a PC13 LED blinky
├── libopencm3/               # Git submodule — built into app/lib
├── Learning materials/       # Notes accompanying the bare-metal series
└── .vscode/                  # Build / flash / debug tasks + J-Link scripts
```

## Target hardware

| Item | Value |
|------|-------|
| MCU | STM32F103C8T6 (BluePill) / STM32F103 high-density |
| Core | ARM Cortex-M3 @ 72 MHz |
| Flash | 64 KiB @ `0x08000000` |
| SRAM  | 20 KiB @ `0x20000000` |
| Demo LED | PC13 (active-low) |
| Debug probe | ST-Link V2 (SWD) or J-Link |

See [`Learning materials/basic_for_blinking_led.md`](Learning%20materials/basic_for_blinking_led.md)
for the full wiring and toolchain setup walkthrough (including the
`libusb`/Zadig troubleshooting steps hit during setup).

## Requirements

* `arm-none-eabi-gcc` toolchain — https://developer.arm.com/downloads/-/gnu-rm
* `stlink-tools` (`st-flash`, `st-info`, `st-util`) — https://github.com/stlink-org/stlink/releases
  *(plus matching-architecture `libusb-1.0.dll` on Windows)*
* OpenOCD (optional alternative debug server) — https://openocd.org/
* J-Link tools (optional, used by the `.vscode/jlink` power scripts)
* GNU Make

## Getting started

Clone **with submodules** so `libopencm3` is fetched:

```bash
git clone --recurse-submodules <this-repo-url> Embedded_system
cd Embedded_system
```

If you already cloned without `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

Build the libopencm3 library once (it ends up in `libopencm3/lib/libopencm3_stm32f1.a`):

```bash
make -C libopencm3 TARGETS=stm32/f1
```

Build the firmware (from the `app/` directory):

```bash
cd app
make            # produces firmware.elf, firmware.bin, firmware.hex, ...
```

Other targets:

```bash
make clean      # remove build artifacts
make images     # build .bin, .hex, .srec, .list, .map
make print-OPENCM3_DIR   # debug a make variable
```

## Flashing

### ST-Link (`st-flash`)

```bash
cd app
make stflash     # builds firmware.bin and runs: st-flash write firmware.bin 0x08000000
```

### OpenOCD

```bash
cd app
make bin
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "program firmware.bin verify reset exit 0x08000000"
```

## Debugging

VS Code tasks are provided in [`.vscode/tasks.json`](.vscode/tasks.json):

* `build_debug` — `make bin` in `app/`
* `flash_stlink` — build + `st-flash`
* `flash_openocd` — build + `openocd` program
* `power_on` / `power_off` — J-Link power-control scripts in
  `.vscode/jlink/`

Launch configurations for Cortex-Debug live in
[`.vscode/launch.json`](.vscode/launch.json).

## The first firmware

`app/src/firmware.c` is the canonical blinky:

* Clock setup: HSE 8 MHz crystal → PLL → 72 MHz
* PC13 configured as 2 MHz push-pull output
* Busy-wait loop toggling the on-board LED (active-low: `gpio_clear` = ON,
  `gpio_set` = OFF)

## License

Project code in `app/` is provided as-is for learning purposes. The
`libopencm3` submodule retains its own license (see `libopencm3/COPYING`).
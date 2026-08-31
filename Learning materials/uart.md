# Understanding the STM32 UART Driver Code (Simple Guide)

This document explains the `uart.c` file piece by piece, in plain language.

---

## 1. The Includes

```c
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/cm3/nvic.h>
```

This code uses **libopencm3** — a library that gives you easy-to-read function names for controlling STM32 hardware (GPIO pins, clocks, USART, interrupts) instead of writing raw register values yourself.

---

## 2. The Shared Variables

```c
static volatile uint8_t data_buffer = 0U;
static volatile bool data_available = false;
```

- **`data_buffer`** — stores the last received byte.
- **`data_available`** — a flag saying "yes, there's a new byte waiting to be read."

**Why `volatile`?**
Because these variables are changed inside an interrupt (which can happen at any time) and read in the normal program. Without `volatile`, the compiler might not realize the value can change unexpectedly, and could optimize the code incorrectly.

⚠️ This is a **single-byte buffer** — only one byte can be stored at a time. If a second byte arrives before the first one is read, it gets overwritten and lost.

---

## 3. Setting Up the Pins (`uart_setup`)

### Turning on power to the hardware
```c
rcc_periph_clock_enable(RCC_GPIOA);
rcc_periph_clock_enable(RCC_USART1);
```
STM32 peripherals are turned **off by default** (to save power). This turns on the clock signal for GPIO Port A and for USART1, so they can actually work.

### Telling the pins what job to do

```c
gpio_set_mode(GPIO_BANK_USART1_TX, GPIO_MODE_OUTPUT_50_MHZ,
              GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART1_TX);

gpio_set_mode(GPIO_BANK_USART1_RX, GPIO_MODE_INPUT,
              GPIO_CNF_INPUT_FLOAT, GPIO_USART1_RX);
```

**Simple idea:** Every pin (like PA9) can be used in different ways — like a wall socket that can be wired to different systems. This code tells the chip: *"Don't let PA9/PA10 behave like plain on/off pins — hand control of them over to the USART1 hardware instead."* This is called an **alternate function**.

- **PA9 (TX)** → set as **output**, because it needs to *send* signals out.
  - "Push-pull" = the pin can actively drive both high and low voltage — the normal way UART works.
- **PA10 (RX)** → set as **input**, because it needs to *listen* for signals.
  - "Floating" = it just reads whatever voltage is on the wire, without being artificially pulled high or low.

**Why this step matters:** Without it, USART1 could be configured perfectly on the inside, but its signals would never actually reach the real pins — because the pins would still be acting like plain, manually-controlled GPIO.

---

## 4. Setting the Communication Rules

```c
usart_set_baudrate(USART1, 115200);
usart_set_databits(USART1, 8);
usart_set_stopbits(USART1, USART_STOPBITS_1);
usart_set_mode(USART1, USART_MODE_TX_RX);
usart_set_parity(USART1, USART_PARITY_NONE);
usart_set_flow_control(USART1, USART_FLOWCONTROL_NONE);
```

Think of this like agreeing on the "rules of conversation" before two people talk — both sides must follow the same rules or it turns into gibberish.

| Setting | Meaning |
|---|---|
| **Baud rate = 115200** | How fast bits are sent. **Must match exactly** on both sides (e.g., your PuTTY setting), or you get garbage/nothing. |
| **8 data bits** | Each chunk of data sent is one byte (8 bits) — the standard. |
| **1 stop bit** | A short pause marking "this byte is done." |
| **TX_RX mode** | Both sending and receiving are turned on. |
| **No parity** | No extra error-checking bit added to each byte. |
| **No flow control** | Neither side can tell the other "wait, slow down" — data just flows continuously. This is part of why data loss can happen under heavy load. |

---

## 5. Turning On Interrupts

```c
usart_enable_rx_interrupt(USART1);
nvic_enable_irq(NVIC_USART1_IRQ);
usart_enable(USART1);
```

- **`usart_enable_rx_interrupt`** — tells USART1: "Interrupt the CPU whenever a new byte fully arrives."
- **`nvic_enable_irq`** — the **NVIC** is the chip's interrupt traffic controller. This tells it: "Yes, actually forward USART1's interrupts to the CPU" (a second, separate switch that also needs to be turned on).
- **`usart_enable`** — finally turns the whole USART1 peripheral ON. Nothing works until this line runs.

---

## 6. The Interrupt Handler — `usart1_isr()`

```c
void usart1_isr(void)
{
	const bool overrun_occurred = usart_get_flag(USART1, USART_FLAG_ORE) != 0;
	const bool received_data = usart_get_flag(USART1, USART_FLAG_RXNE) != 0;

	if (received_data || overrun_occurred) {
		data_buffer = (uint8_t)usart_recv(USART1);
		data_available = true;
	}
}
```

This function runs **automatically** whenever a UART interrupt happens — you never call it yourself.

- **`RXNE` flag** = "a new byte has arrived and is ready to be read."
- **`ORE` flag (Overrun Error)** = "a byte was lost! A new byte arrived before the old one was ever read."

If either flag is true, it reads the byte into `data_buffer` and sets `data_available = true`.

⚠️ **The core weakness:** since there's only one `data_buffer`, if a second byte arrives before the main program reads the first one, the **second byte overwrites the first — silently**. This is the exact single-byte-buffer problem from the original bare-metal video.

---

## 7. The Public Functions (what the rest of your program uses)

### Sending multiple bytes
```c
void uart_write(uint8_t *data, const uint32_t length)
```
Loops through an array and sends each byte one at a time using `uart_write_byte()`.

### Sending one byte
```c
void uart_write_byte(uint8_t data)
{
	usart_send_blocking(USART1, data);
}
```
"Blocking" means the CPU **waits here doing nothing else** until the byte is fully sent. Simple, but not efficient if you're sending a lot of data quickly.

### Reading one byte (safe version)
```c
uint32_t uart_read(uint8_t *data, const uint32_t length)
```
Checks if data is actually available first. If yes, copies it out and clears the flag. If no data is available, returns `0` (nothing read).

### Reading one byte (simple version)
```c
uint8_t uart_read_byte(void)
```
Just grabs whatever is in `data_buffer`, without checking if it's actually new. Riskier — should only be used after checking `uart_data_available()` first.

### Checking if new data has arrived
```c
bool uart_data_available(void)
{
	return data_available;
}
```
Lets your main program ask: *"Is there something new waiting for me?"* before trying to read it.

---

## Putting It All Together

A typical way this driver gets used in your main program:

```c
if (uart_data_available()) {
    uint8_t byte = uart_read_byte();
    // do something with byte
}
```

## The Big Picture

This driver works fine for **light, slow traffic**. But because it only stores **one byte at a time**, if data arrives faster than your main loop can read it, bytes get silently lost — exactly the problem that a **ring buffer** solves, by storing multiple incoming bytes in a queue instead of just one.

**Your next step:** replace `data_buffer` / `data_available` with a ring buffer (an array + head/tail index pair), so the interrupt can safely queue multiple bytes and your main loop can read them without losing data — this is the direct software baseline for comparing against your FPGA hardware version in your thesis.
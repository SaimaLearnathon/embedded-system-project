# Firmware Update State Machine in `bootloader.c`

This note explains the firmware update state machine currently implemented in:

```text
bootloader/src/bootloader.c
```

The goal of this bootloader is:

1. Wait for a firmware updater program on UART.
2. Confirm that the updater is talking to the correct device.
3. Receive the new application firmware in small packets.
4. Erase the old application area.
5. Write the new firmware into flash.
6. Check that the new application looks valid.
7. Jump into the new application if the update succeeded.

## Big Picture

The bootloader is a small program placed at the beginning of flash. It owns the first 32 KB:

```c
#define BOOTLOADER_SIZE (0x8000U)
```

The main application starts immediately after the bootloader:

```c
#define MAIN_APP_START_ADDRESS (FLASH_BASE + BOOTLOADER_SIZE)
```

For an STM32 with flash starting at `0x08000000`, this means:

```text
0x08000000  -------------------------------
            Bootloader
            32 KB

0x08008000  -------------------------------
            Main application firmware

0x08040000  -------------------------------
            End of 256 KB flash
```

The bootloader never writes over itself. It only erases and writes the main application area:

```text
MAIN_APP_START_ADDRESS  to  FLASH_END_ADDRESS
```

## Important Constants

These constants define the memory layout and protocol limits:

```c
#define BOOTLOADER_SIZE (0x8000U)
#define MAIN_APP_START_ADDRESS (FLASH_BASE + BOOTLOADER_SIZE)
#define FLASH_END_ADDRESS (FLASH_BASE + (256U * 1024U))
#define FLASH_PAGE_SIZE (2048U)
#define MAX_FW_LENGTH (FLASH_END_ADDRESS - MAIN_APP_START_ADDRESS)
```

Meaning:

| Constant | Meaning |
|---|---|
| `BOOTLOADER_SIZE` | The bootloader uses the first 32 KB of flash |
| `MAIN_APP_START_ADDRESS` | Where the application firmware begins |
| `FLASH_END_ADDRESS` | End of available flash |
| `FLASH_PAGE_SIZE` | Flash erase page size, 2048 bytes |
| `MAX_FW_LENGTH` | Maximum firmware size that can fit after the bootloader |

The bootloader also uses a device ID:

```c
#define DEVICE_ID (0x42)
```

The updater must prove that it is sending firmware for this device by sending back this ID.

## Packet Protocol Commands

The bootloader does not read random text commands. It uses small binary packets defined in `comms.h`.

Each packet has:

```c
typedef struct comms_packet_t {
    uint8_t length;
    uint8_t data[PACKET_DATA_LENGTH];
    uint8_t crc;
} comms_packet_t;
```

`PACKET_DATA_LENGTH` is 16 bytes:

```c
#define PACKET_DATA_LENGTH (16)
```

So each wire packet is:

```text
1 byte length + 16 bytes data + 1 byte CRC = 18 bytes total
```

The important firmware update command bytes are:

| Command byte | Meaning |
|---|---|
| `BL_PACKET_SYNC_OBSERVED_DATA0` / `0x20` | Bootloader saw the sync sequence |
| `BL_PACKET_FW_UPDATE_REQ_DATA0` / `0x31` | Host asks to start firmware update |
| `BL_PACKET_FW_UPDATE_RES_DATA0` / `0x37` | Bootloader accepts update request |
| `BL_PACKET_DEVICE_ID_REQ_DATA0` / `0x3C` | Bootloader asks host for device ID |
| `BL_PACKET_DEVICE_ID_RES_DATA0` / `0x3F` | Host replies with device ID |
| `BL_PACKET_FW_LENGTH_REQ_DATA0` / `0x42` | Bootloader asks for firmware length |
| `BL_PACKET_FW_LENGTH_RES_DATA0` / `0x45` | Host replies with firmware length |
| `BL_PACKET_READY_FOR_DATA_DATA0` / `0x48` | Bootloader is ready for next data packet |
| `BL_PACKET_UPDATE_SUCCESSFUL_DATA0` / `0x54` | Firmware update completed successfully |
| `BL_PACKET_NACK_DATA0` / `0x59` | Bootloader rejects the update |

The lower-level packet layer also has:

| Command byte | Meaning |
|---|---|
| `PACKET_ACK_DATA0` / `0x15` | Packet was received correctly |
| `PACKET_RETx_DATA0` / `0x19` | Please retransmit the last packet |

The packet layer handles CRC, ACK, and retransmit behavior. The bootloader state machine handles the meaning of the packets.

## State Machine List

The bootloader states are:

```c
typedef enum bl_state_t {
    BL_State_Sync,
    BL_State_WaitForUpdateReq,
    BL_State_DeviceIDReq,
    BL_State_DeviceIDRes,
    BL_State_FWLengthReq,
    BL_State_FWLengthRes,
    BL_State_EraseApplication,
    BL_State_ReceiveFirmware,
    BL_State_Done
} bl_state_t;
```

The state variable starts here:

```c
static bl_state_t state = BL_State_Sync;
```

So after reset, the bootloader begins in `BL_State_Sync`.

## Full Firmware Update Flow

Here is the complete high-level flow:

```text
BL_State_Sync
    |
    v
BL_State_WaitForUpdateReq
    |
    v
BL_State_DeviceIDReq
    |
    v
BL_State_DeviceIDRes
    |
    v
BL_State_FWLengthReq
    |
    v
BL_State_FWLengthRes
    |
    v
BL_State_EraseApplication
    |
    v
BL_State_ReceiveFirmware
    |
    v
BL_State_Done
    |
    v
Jump to main application if update_successful is true
```

At almost every waiting state, a timeout or unexpected packet causes failure:

```text
bad packet / timeout / invalid value
    |
    v
bootloading_fail()
    |
    v
send NACK
    |
    v
BL_State_Done
```

## Timeout Concept

The bootloader uses a simple timeout:

```c
#define DEFAULT_TIMEOUT (5000)
```

The timer is configured here:

```c
simple_timer_setup(&timer, DEFAULT_TIMEOUT, false);
```

Whenever the bootloader receives a valid response or moves to a new important step, it resets the timer:

```c
simple_timer_reset(&timer);
```

If the timer expires, this function is called:

```c
static void check_for_timeout(void) {
    if (simple_timer_has_elapsed(&timer)) {
        bootloading_fail();
    }
}
```

This prevents the bootloader from waiting forever if the updater disconnects or stops sending data.

## Failure Function

All fatal protocol errors go through:

```c
static void bootloading_fail(void) {
    comms_create_single_byte_packet(&packet, BL_PACKET_NACK_DATA0);
    comms_write(&packet);
    state = BL_State_Done;
}
```

Meaning:

1. Create a one-byte `NACK` packet.
2. Send it to the host.
3. Stop the bootloader state machine by entering `BL_State_Done`.

After this, because `update_successful` is still false, the bootloader does not jump to the app. It enters the final low-power wait loop.

## State 1: `BL_State_Sync`

Purpose:

The bootloader waits for a special four-byte sync sequence from the host.

```c
#define SYNC_SEQ_0 (0xc4)
#define SYNC_SEQ_1 (0x55)
#define SYNC_SEQ_2 (0x7e)
#define SYNC_SEQ_3 (0x10)
```

The sequence is:

```text
C4 55 7E 10
```

The bootloader stores the last four received UART bytes in:

```c
static uint8_t sync_seq[4] = {0};
```

Each time a new UART byte arrives, the array shifts:

```c
sync_seq[0] = sync_seq[1];
sync_seq[1] = sync_seq[2];
sync_seq[2] = sync_seq[3];
sync_seq[3] = uart_read_byte();
```

This lets the bootloader find the sync sequence even if random bytes arrived before it.

Example:

```text
Incoming UART bytes:
99 AA 12 C4 55 7E 10

The bootloader ignores:
99 AA 12

Then it sees:
C4 55 7E 10
```

When the sequence matches, the bootloader replies:

```c
BL_PACKET_SYNC_OBSERVED_DATA0
```

Then it moves to:

```c
BL_State_WaitForUpdateReq
```

Important detail:

In this state, the code reads raw UART bytes directly using `uart_read_byte()`. After sync is found, it switches to packet-based communication using `comms_update()`.

## State 2: `BL_State_WaitForUpdateReq`

Purpose:

The bootloader waits for the host to ask for a firmware update.

Expected packet:

```c
BL_PACKET_FW_UPDATE_REQ_DATA0
```

The code checks:

```c
comms_is_single_byte_packet(&packet, BL_PACKET_FW_UPDATE_REQ_DATA0)
```

If the packet is correct, the bootloader sends:

```c
BL_PACKET_FW_UPDATE_RES_DATA0
```

Then it moves to:

```c
BL_State_DeviceIDReq
```

If the packet is wrong, the bootloader sends `NACK` and stops.

Concept:

The sync sequence only says "someone is trying to talk to the bootloader." This state confirms that the host really wants to start a firmware update.

## State 3: `BL_State_DeviceIDReq`

Purpose:

The bootloader asks the host to prove that the firmware is meant for this device.

The bootloader sends:

```c
BL_PACKET_DEVICE_ID_REQ_DATA0
```

Then it moves immediately to:

```c
BL_State_DeviceIDRes
```

This state does not wait for a packet. It only sends the request.

## State 4: `BL_State_DeviceIDRes`

Purpose:

The bootloader waits for the host's device ID response.

Expected packet format:

```text
length = 2
data[0] = BL_PACKET_DEVICE_ID_RES_DATA0
data[1] = DEVICE_ID
data[2..15] = 0xFF padding
```

The helper function first checks the packet shape:

```c
static bool is_device_id_packet(const comms_packet_t* rx_packet)
```

It requires:

```c
rx_packet->length == 2
rx_packet->data[0] == BL_PACKET_DEVICE_ID_RES_DATA0
rx_packet->data[2..15] == 0xff
```

Then the state machine checks the actual device ID:

```c
packet.data[1] == DEVICE_ID
```

If correct, it moves to:

```c
BL_State_FWLengthReq
```

If wrong, it sends `NACK` and stops.

Concept:

This protects against flashing firmware meant for another product or board.

## State 5: `BL_State_FWLengthReq`

Purpose:

The bootloader asks the host how many firmware bytes it will send.

The bootloader sends:

```c
BL_PACKET_FW_LENGTH_REQ_DATA0
```

Then it moves to:

```c
BL_State_FWLengthRes
```

Again, this state only sends a request. The next state waits for the response.

## State 6: `BL_State_FWLengthRes`

Purpose:

The bootloader receives the firmware length and checks whether it can fit in flash.

Expected packet format:

```text
length = 5
data[0] = BL_PACKET_FW_LENGTH_RES_DATA0
data[1] = firmware length byte 0
data[2] = firmware length byte 1
data[3] = firmware length byte 2
data[4] = firmware length byte 3
data[5..15] = 0xFF padding
```

The firmware length is reconstructed as a 32-bit little-endian integer:

```c
fw_length = (
    ((uint32_t)packet.data[1]) |
    ((uint32_t)packet.data[2] << 8) |
    ((uint32_t)packet.data[3] << 16) |
    ((uint32_t)packet.data[4] << 24)
);
```

Little-endian means the smallest byte comes first.

Example:

```text
data[1] = 0x34
data[2] = 0x12
data[3] = 0x00
data[4] = 0x00

fw_length = 0x00001234
```

The bootloader accepts the length only if:

```c
is_fw_length_packet(&packet) &&
(fw_length > 0U) &&
(fw_length <= MAX_FW_LENGTH)
```

So the firmware length must:

1. Use the correct packet format.
2. Be greater than zero.
3. Fit inside the application flash area.

If valid:

```c
bytes_written = 0;
state = BL_State_EraseApplication;
```

If invalid, the bootloader sends `NACK` and stops.

Concept:

The bootloader checks the firmware size before erasing flash. This avoids accepting an impossible update that would overflow past the valid application region.

## State 7: `BL_State_EraseApplication`

Purpose:

The bootloader erases the old application firmware from flash.

The call is:

```c
bl_flash_erase_main_application();
```

Inside `bl-flash.c`, that function erases every flash page from:

```text
MAIN_APP_START_ADDRESS
```

up to:

```text
FLASH_END_ADDRESS
```

After erasing, the bootloader tells the host:

```c
BL_PACKET_READY_FOR_DATA_DATA0
```

Then it moves to:

```c
BL_State_ReceiveFirmware
```

Concept:

Flash memory cannot usually be changed directly from any value to any value. It must be erased first, which sets bytes back to `0xFF`. Then the bootloader can program the new firmware bytes.

Important warning:

After this state, the old application is gone. If the update fails after erase, this bootloader currently does not have a rollback image.

## State 8: `BL_State_ReceiveFirmware`

Purpose:

The bootloader receives firmware data packets and writes them into flash.

Each received packet contains up to 16 real data bytes:

```c
const uint8_t packet_length = packet.length;
```

Before writing, the bootloader checks:

```c
if ((packet_length == 0U) || ((bytes_written + packet_length) > fw_length)) {
    bootloading_fail();
    break;
}
```

This rejects:

1. Empty firmware data packets.
2. Packets that would write more bytes than the promised firmware length.

If valid, the bootloader writes the packet data to flash:

```c
bl_flash_write(MAIN_APP_START_ADDRESS + bytes_written,
               packet.data,
               packet_length);
```

Then it updates the counter:

```c
bytes_written += packet_length;
```

So the first packet writes at:

```text
MAIN_APP_START_ADDRESS + 0
```

The second packet writes at:

```text
MAIN_APP_START_ADDRESS + previous packet length
```

And so on.

### If More Data Is Still Needed

If this is not the end:

```c
bytes_written < fw_length
```

the bootloader sends:

```c
BL_PACKET_READY_FOR_DATA_DATA0
```

This tells the host:

```text
"I wrote that chunk. Send the next one."
```

### If All Firmware Bytes Were Received

If:

```c
bytes_written >= fw_length
```

the bootloader checks whether the application looks valid:

```c
if (application_is_valid()) {
    update_successful = true;
    comms_create_single_byte_packet(&packet, BL_PACKET_UPDATE_SUCCESSFUL_DATA0);
    comms_write(&packet);
    state = BL_State_Done;
} else {
    bootloading_fail();
}
```

If valid:

1. `update_successful` becomes true.
2. The bootloader sends `BL_PACKET_UPDATE_SUCCESSFUL_DATA0`.
3. The state machine enters `BL_State_Done`.

If invalid:

1. The bootloader sends `NACK`.
2. The update is considered failed.

## State 9: `BL_State_Done`

Purpose:

This state exits the main bootloader loop:

```c
while (state != BL_State_Done) {
    ...
}
```

After the loop, the bootloader checks:

```c
if (update_successful && application_is_valid()) {
    jump_to_main();
}
```

So the bootloader jumps to the application only when:

1. The update completed successfully.
2. The application still passes the validity check.

If not, it waits forever:

```c
while (1) {
    __asm__("wfi");
}
```

`wfi` means "wait for interrupt." It is a low-power idle instruction.

## Application Validity Check

The function is:

```c
static bool application_is_valid(void)
```

It reads the first two words of the application vector table:

```c
const uint32_t *app_vector_table = (const uint32_t *)MAIN_APP_START_ADDRESS;
const uint32_t app_stack = app_vector_table[0];
const uint32_t app_reset = app_vector_table[1];
```

On Cortex-M, the vector table starts like this:

```text
word 0 = initial stack pointer
word 1 = reset handler address
```

The bootloader checks:

```c
app_stack >= SRAM_BASE_ADDRESS
app_stack <= SRAM_END_ADDRESS
(app_stack & 0x7U) == 0U
(app_reset & 1U) != 0U
app_reset_address >= MAIN_APP_START_ADDRESS
app_reset_address < FLASH_END_ADDRESS
```

Meaning:

| Check | Why it matters |
|---|---|
| Stack is inside SRAM | A real app must start with a valid RAM stack pointer |
| Stack is 8-byte aligned | Cortex-M stack alignment requirement |
| Reset handler bit 0 is set | Cortex-M uses Thumb mode, so function addresses have bit 0 set |
| Reset handler points into app flash | The reset code should live inside the application area |

This is not a cryptographic verification. It does not prove the firmware is authentic. It only checks whether the first vector table entries look sane enough to jump to.

## Jumping to the Application

The jump function is:

```c
static void jump_to_main(void)
```

It reads:

```c
uint32_t app_stack = app_vector_table[0];
uint32_t app_reset = app_vector_table[1];
```

Then it prepares the MCU:

```c
prepare_to_jump();
SCB_VTOR = MAIN_APP_START_ADDRESS;
```

`prepare_to_jump()` disables bootloader-related interrupt activity:

```c
systick_interrupt_disable();
systick_counter_disable();
nvic_disable_irq(NVIC_USART1_IRQ);
```

`SCB_VTOR` changes the vector table location to the application. This is necessary because interrupts should now use the application's handlers, not the bootloader's handlers.

Finally, inline assembly loads the application's stack pointer and branches to the application's reset handler:

```c
__asm volatile (
    "msr msp, %0    \n"
    "bx  %1         \n"
    :
    : "r" (app_stack), "r" (jump_fn)
    : "memory"
);
```

Meaning:

1. `msr msp, app_stack` sets the main stack pointer to the application's stack.
2. `bx app_reset` jumps to the application's reset handler.

At this point, the bootloader is no longer running.

## Request and Response Sequence

This is the normal conversation between host and bootloader:

```text
Host        -> Bootloader: raw sync bytes C4 55 7E 10
Bootloader  -> Host:       SYNC_OBSERVED

Host        -> Bootloader: FW_UPDATE_REQ
Bootloader  -> Host:       FW_UPDATE_RES

Bootloader  -> Host:       DEVICE_ID_REQ
Host        -> Bootloader: DEVICE_ID_RES + device ID 0x42

Bootloader  -> Host:       FW_LENGTH_REQ
Host        -> Bootloader: FW_LENGTH_RES + firmware length

Bootloader  -> Host:       READY_FOR_DATA
Host        -> Bootloader: firmware data packet 0
Bootloader  -> Host:       READY_FOR_DATA
Host        -> Bootloader: firmware data packet 1
Bootloader  -> Host:       READY_FOR_DATA
...
Host        -> Bootloader: final firmware data packet
Bootloader  -> Host:       UPDATE_SUCCESSFUL

Bootloader  -> Application: jump to app reset handler
```

At the packet layer, each valid non-control packet is also acknowledged with `PACKET_ACK_DATA0`. If a packet has a bad CRC, the receiver asks for retransmission with `PACKET_RETx_DATA0`.

## Why Two Kinds of ACK Exist

There are two different "success" ideas:

1. Packet-level success.
2. Bootloader-command-level success.

Packet-level success:

```text
"The 18-byte packet arrived correctly. CRC matched."
```

This is handled inside `comms.c` using `PACKET_ACK_DATA0`.

Bootloader-command-level success:

```text
"The command makes sense and the bootloader accepted it."
```

This is handled inside `bootloader.c` using packets like:

```text
FW_UPDATE_RES
READY_FOR_DATA
UPDATE_SUCCESSFUL
NACK
```

So a packet can have a valid CRC but still be rejected by the bootloader if it is the wrong command for the current state.

## Important Variables

| Variable | Purpose |
|---|---|
| `state` | Current bootloader state |
| `fw_length` | Total number of firmware bytes expected |
| `bytes_written` | Number of firmware bytes already written to flash |
| `update_successful` | True only after the full update is received and the app validates |
| `sync_seq[4]` | Sliding four-byte buffer used to detect the sync sequence |
| `timer` | Timeout guard |
| `packet` | Shared packet object used for receiving and sending |

## Why `bytes_written` Matters

The bootloader receives firmware in small chunks. It needs to know where the next chunk belongs in flash.

This line chooses the destination address:

```c
MAIN_APP_START_ADDRESS + bytes_written
```

At the beginning:

```text
bytes_written = 0
```

After writing 16 bytes:

```text
bytes_written = 16
```

After writing another 16 bytes:

```text
bytes_written = 32
```

So `bytes_written` is both:

1. A progress counter.
2. The offset used to calculate the next flash write address.

## Flash Write Detail

`bl_flash_write()` writes flash in half-words, meaning 2 bytes at a time:

```c
for (uint32_t i = 0; i < length; i += 2U) {
    uint16_t half_word = data[i];

    if ((i + 1U) < length) {
        half_word |= ((uint16_t)data[i + 1U] << 8U);
    } else {
        half_word |= 0xff00U;
    }

    flash_program_half_word(address + i, half_word);
}
```

If the firmware length is odd, the last half-word is padded with `0xFF` in the upper byte.

Example:

```text
Data bytes:
11 22 33

Flash half-words:
0x2211
0xFF33
```

This matches how erased flash naturally contains `0xFF`.

## Full State Table

| Current state | Expected event | Action | Next state |
|---|---|---|---|
| `BL_State_Sync` | Raw UART sync sequence `C4 55 7E 10` | Send `SYNC_OBSERVED` | `BL_State_WaitForUpdateReq` |
| `BL_State_WaitForUpdateReq` | `FW_UPDATE_REQ` packet | Send `FW_UPDATE_RES` | `BL_State_DeviceIDReq` |
| `BL_State_DeviceIDReq` | No wait | Send `DEVICE_ID_REQ` | `BL_State_DeviceIDRes` |
| `BL_State_DeviceIDRes` | `DEVICE_ID_RES` with `0x42` | Accept device ID | `BL_State_FWLengthReq` |
| `BL_State_FWLengthReq` | No wait | Send `FW_LENGTH_REQ` | `BL_State_FWLengthRes` |
| `BL_State_FWLengthRes` | `FW_LENGTH_RES` with valid length | Store length, set `bytes_written = 0` | `BL_State_EraseApplication` |
| `BL_State_EraseApplication` | No wait | Erase app flash, send `READY_FOR_DATA` | `BL_State_ReceiveFirmware` |
| `BL_State_ReceiveFirmware` | Firmware data packet | Write packet to flash | Stay here or go to `BL_State_Done` |
| `BL_State_Done` | Update success | Jump to app | Does not return |
| `BL_State_Done` | Update failed | Wait forever | No next state |

## Failure Table

| Where failure happens | Cause | Result |
|---|---|---|
| Any waiting state | Timeout | Send `NACK`, enter `BL_State_Done` |
| `WaitForUpdateReq` | Wrong packet | Send `NACK`, enter `BL_State_Done` |
| `DeviceIDRes` | Wrong packet format or wrong ID | Send `NACK`, enter `BL_State_Done` |
| `FWLengthRes` | Length is zero or too large | Send `NACK`, enter `BL_State_Done` |
| `ReceiveFirmware` | Empty packet | Send `NACK`, enter `BL_State_Done` |
| `ReceiveFirmware` | Packet would exceed promised firmware length | Send `NACK`, enter `BL_State_Done` |
| End of receive | Application vector table is invalid | Send `NACK`, enter `BL_State_Done` |

## One Important Design Limitation

The bootloader erases the old application before receiving the new firmware:

```text
Erase old app -> Receive new app
```

That is simple, but it means an interrupted update can leave the board without a valid application.

More advanced bootloaders often use one of these approaches:

1. Store the new image in a second flash slot, then swap after validation.
2. Keep a rollback image.
3. Add a recovery mode that always allows retrying the update.
4. Verify a cryptographic signature before booting the app.

Your current implementation is a good learning bootloader because the state machine is clear and direct.

## Short Summary

The firmware update state machine is a controlled conversation between the updater and the bootloader.

The bootloader does not blindly accept bytes. It requires:

1. A sync sequence.
2. A firmware update request.
3. A matching device ID.
4. A valid firmware length.
5. Correctly received data packets.
6. A sane application vector table after flashing.

Only after all of those steps pass does it set:

```c
update_successful = true;
```

Then it sends:

```c
BL_PACKET_UPDATE_SUCCESSFUL_DATA0
```

Finally, it jumps to the new application by loading the application's stack pointer, relocating the vector table, and branching to the application's reset handler.

# Firmware Updater Sync Debugging Notes

This note records the UART sync problems seen while testing:

```text
fw-updater/index.ts
bootloader/src/bootloader.c
shared/src/core/uart.c
bootloader/src/comms.c
```

The goal was to make the PC updater send a raw sync sequence:

```text
c4 55 7e 10
```

and have the bootloader reply with:

```c
BL_PACKET_SYNC_OBSERVED_DATA0
```

## Symptom 1: Timed Out Waiting for Bootloader Sync

Updater output:

```text
[.] Connected to COM8 at 115200 baud
[.] Attempting to sync with bootloader...
[>] TX 4 bytes: c4 55 7e 10
[>] TX 4 bytes: c4 55 7e 10
[!] Timed out waiting for bootloader sync
```

Meaning:

```text
PC is transmitting sync bytes.
PC is not receiving a valid bootloader response.
```

Possible causes:

1. Bootloader is not running.
2. Wrong COM port.
3. USB-UART wiring problem.
4. Bootloader timed out and stopped waiting.
5. Debugger/OpenOCD halted the MCU.

The serial monitor confirmed the bootloader was running because it printed:

```text
[bootloader] app_stack=0xFFFFFFFF app_reset=0xFFFFFFFF valid=NO
[bootloader] application invalid, waiting for updater
```

That proved:

1. STM32 TX to PC RX worked.
2. COM8 was correct.
3. The bootloader was alive.

So the next suspects were timing, binary parser desync, or PC TX to STM32 RX.

## Symptom 2: RETX During Sync

Updater output:

```text
[>] RX 18 bytes: 01 19 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 3f
[>] RETX received but no last packet to resend
```

`0x19` is:

```c
PACKET_RETx_DATA0
```

This was an important clue. It meant the bootloader was no longer in raw sync mode. It had already entered packet mode.

So the bootloader had probably already seen:

```text
c4 55 7e 10
```

and moved from:

```c
BL_State_Sync
```

to:

```c
BL_State_WaitForUpdateReq
```

But the updater did not receive or accept the `SYNC_OBSERVED` packet, so it kept sending raw sync bytes.

Once the bootloader is in packet mode, raw sync bytes are no longer valid. The packet parser expects 18-byte packets:

```text
1 byte length + 16 data bytes + 1 CRC byte
```

If the packet parser sees `0xc4` as a packet length, that is invalid because the maximum payload length is 16. So the bootloader replies:

```text
RETX
```

## Issue 1: Plain Text Logs Mixed With Binary Packets

The bootloader was printing diagnostic text on the same UART used by the packet protocol:

```text
[bootloader] app_stack=...
[bootloader] application invalid...
```

That is useful for PuTTY debugging, but bad for `fw-updater/index.ts`.

The updater expects binary packets. Plain ASCII text can enter the receive buffer and confuse packet parsing.

Fix:

```c
#define ENABLE_UART_DIAGNOSTICS (0U)
```

The diagnostic functions are now behind:

```c
#if ENABLE_UART_DIAGNOSTICS
...
#endif
```

So normal updater mode is binary-only.

## Issue 2: Leftover Raw Sync Bytes

The updater sends the sync sequence repeatedly:

```text
c4 55 7e 10
c4 55 7e 10
c4 55 7e 10
```

The bootloader detects sync using a sliding 4-byte window.

Problem:

After one sync sequence matches, more raw sync bytes may already be buffered by the UART interrupt.

Then the bootloader switches to packet mode, and `comms_update()` starts reading those leftover raw bytes as packet data.

That causes invalid packet parsing and repeated `RETX`.

Fix:

```c
uart_flush_rx();
comms_setup();
comms_create_single_byte_packet(&packet, BL_PACKET_SYNC_OBSERVED_DATA0);
comms_write(&packet);
```

This happens immediately after sync is detected.

`uart_flush_rx()` discards any leftover raw sync bytes before packet mode begins.

## Issue 3: Updater Did Not Treat RETX as Already Synced

During sync, receiving `RETX` with no previous packet means:

```text
The bootloader is already in packet mode.
```

Originally, the updater ignored that and kept sending raw sync forever.

Fix in `index.ts`:

```ts
let bootloaderInPacketMode = false;
```

When a `RETX` arrives before the updater has sent any packet:

```ts
if (lastPacket) {
  writePacket(lastPacket);
} else {
  bootloaderInPacketMode = true;
}
```

Then `syncWithBootloader()` accepts that as success:

```ts
if (bootloaderInPacketMode) {
  Logger.success('Bootloader sync successful');
  return;
}
```

## Final Sync Meanings

Clean sync:

```text
[>] TX 4 bytes: c4 55 7e 10
[>] RX 18 bytes: 01 20 ...
[$] Bootloader sync successful
```

This means the updater caught the `SYNC_OBSERVED` packet.

Already-in-packet-mode sync:

```text
[>] RX 18 bytes: 01 19 ...
[>] RETX received but no last packet to resend
[$] Bootloader sync successful
```

This means the bootloader had already accepted sync and entered packet mode.

Both are valid for a sync check.

## Practical Rule

Use one UART mode at a time:

```text
PuTTY/debug text mode
or
binary updater protocol mode
```

Do not mix plain text logs with fixed-size binary packets on the same UART unless the protocol has a way to frame and ignore text.

## Symptom 3: Permanent RETX Loop, Same Bytes Echoed Forever

Updater output looked like this, repeating without end:

```text
[>] TX 18 bytes: 01 19 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 3f
[>] RX 18 bytes: 01 19 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 3f
[>] Invalid length byte 0xff (max 16) -> sending RETX
```

on and on, eventually falling back to raw sync bytes and timing out:

```text
[!] Timed out waiting for bootloader sync
```

### Reading the clue

The RX line is byte-for-byte identical to the TX line above it. But the parser reports the length byte as `0xff`, not the `0x01` actually sitting at the front of what was sent. That mismatch means the receive buffer is misaligned by exactly one byte: the parser's 18-byte window starts one byte too early, so it reads the *previous* packet's trailing `0xff` padding as this packet's length byte.

Once that happens, it never fixes itself with the old code, which discarded a whole 18-byte window before asking for a resend:

```ts
const raw = consumeFromBuffer(PACKET_LENGTH);
if (raw[0] > PACKET_DATA_BYTES) {
  writePacket(Packet.retx);
  continue;
}
```

Sending RETX assumes the frame is aligned but the *content* is bad, so retransmitting the same bytes will fix it. But here the frame isn't aligned at all — the retransmitted packet lands at the exact same broken offset and fails the exact same way. That is the loop in the log.

A single stray fragment further down the same log confirms this wasn't just an echo:

```text
[>] RX 17 bytes: 50 f4 ff ff ff ff ff ff ff ff ff ff ff ff ff ff 3f
```

Those bytes were never transmitted by the host, so the bootloader was genuinely replying — it just kept getting shredded by the same one-byte offset.

### Fix

`fw-updater/index.ts`'s `onData` now tells the two failure cases apart:

```ts
if (rxBuffer[0] > PACKET_DATA_BYTES) {
  Logger.debug(`Invalid length byte 0x${rxBuffer[0].toString(16)} (max ${PACKET_DATA_BYTES}) -> resyncing`);
  consumeFromBuffer(1);
  continue;
}
```

- **Length byte out of range** → the buffer is almost certainly misaligned, not corrupted. Drop exactly one byte and re-check, letting the parser slide forward until the real frame boundary lines up. No RETX is sent, since asking for a resend can't fix a receive-side alignment problem.
- **Length byte fine but CRC mismatch** (unchanged, still below this check) → the frame *is* aligned, just corrupted in transit. RETX is still the right response here.

With this fix, a single stray byte anywhere on the wire costs at most a few bytes of silent resyncing instead of hanging until the 30-second timeout.

### Practical note

This is a self-healing fallback, not a fix for a noisy line. If this symptom shows up often, check the physical wiring too — the exact-echo pattern above is also the classic signature of the adapter's TX and RX being crossed or shorted, which would keep injecting stray bytes for the software to clean up.

## Symptom 4: Same Packet Retransmitted Forever, Bootloader Never Accepts It

Sync succeeded (via the "already in packet mode" RETX path from Symptom 2/3), then this repeated without ever stopping:

```text
[>] TX 18 bytes: 01 31 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 6f
[>] RX 18 bytes: 01 19 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 3f
[>] RETX received -> resending last packet
```

on and on, thousands of times, well past the point a one-off glitch would explain.

### Why this is different from Symptom 3

Symptom 3 was the *host* misreading an aligned bootloader reply. This is the opposite direction: the exact same `FW_UPDATE_REQ` packet (`01 31 ff...6f`) gets resent unchanged every time (the host isn't misparsing anything — it's doing exactly what a `RETX` reply should make it do), and the *bootloader* keeps rejecting it. Since the CRC-8 algorithm and the bytes it covers are identical on both sides (`shared/src/core/crc8.c` vs. the TypeScript `crc8` in `index.ts`, both length + all 16 data bytes, same polynomial `0x07`), a real one-off CRC mismatch could not reproduce identically forever — the same input always hashes the same way. Something more structural was wrong.

### Root cause

`comms_update()` in `bootloader/src/comms.c` parses incoming bytes with a fixed-stride state machine: read 1 length byte, then unconditionally read 16 data bytes, then 1 CRC byte — every cycle, regardless of whether the length byte made any sense:

```c
case CommsState_length:
    temporary_packet.length = uart_read_byte();
    data_byte_count = 0;
    state = CommsState_Data;
    break;
...
case CommsState_CRC: {
    temporary_packet.crc = uart_read_byte();
    state = CommsState_length;

    if (temporary_packet.length > PACKET_DATA_LENGTH ||
        temporary_packet.crc != comms_compute_crc(&temporary_packet)) {
        comms_write(&retx_packet);
        break;
    }
    ...
```

The length byte is only checked at the very end, after 16 more bytes have already been consumed as if they were real data. This is the exact same class of bug as Symptom 3, just on the bootloader's C side instead of the host's TypeScript side: if the byte stream is ever off by so much as one byte (very plausible after a long session of retries, RETX storms, and repeated sync attempts without resetting the board), the parser locks onto the wrong 18-byte stride and stays there. `comms_write(&retx_packet)` doesn't help — the host resends the same bytes, they land at the same broken offset, and it fails the same way forever.

### Fix

`comms_update()` now checks the length byte the moment it's read, instead of after committing to 16 more bytes:

```c
case CommsState_length: {
    uint8_t length = uart_read_byte();
    if (length > PACKET_DATA_LENGTH) {
        /* Likely misalignment, not corruption - stay here and try the next
         * byte as a fresh length candidate instead of asking for a
         * retransmit that can't fix a receive-side offset. */
        break;
    }
    temporary_packet.length = length;
    data_byte_count = 0;
    state = CommsState_Data;
    break;
}
```

The later CRC check in `CommsState_CRC` no longer needs to re-check the length (it's valid by construction at that point) — it only checks the CRC, and still sends `RETX` for that case, since a valid length with a bad CRC means the frame really is aligned and really was corrupted, which retransmission genuinely fixes.

### Practical note

This requires a firmware change, not just a script change — `bootloader.c`/`comms.c` have to be rebuilt **and reflashed** to the board for the fix to take effect (unlike the host fixes in `index.ts`, which apply the moment the script is rerun). Rebuilt cleanly with no warnings: `make bin` in `bootloader/`.

Until the board is reflashed with this fix, the workaround for a stuck session like this one is the same as Symptom 1's: **power-cycle or reset the board.** A real reset always re-runs `comms_setup()` cleanly from `BL_State_Sync`, which clears the stuck parser state immediately, even on the old firmware.


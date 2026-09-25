# fw-updater/index.ts — Full Line-by-Line Walkthrough

This is a complete tour of [`fw-updater/index.ts`](../fw-updater/index.ts), the Node.js/TypeScript program that talks to the bootloader over UART and pushes new firmware onto the board. It builds directly on [[packet-protocol]] and [[firmware-update-state-machine]] — this file is the **host-side half** of the same protocol the bootloader implements in C.

Read it top to bottom, in the same order the file is written, since later sections depend on things defined earlier (a `Packet` class needs to exist before you can build packets, etc.).

---

## 1. Imports

```ts
import * as fs from 'fs/promises';
import { performance } from 'node:perf_hooks';
import {SerialPort} from 'serialport';
```

- `fs/promises` — the Promise-based version of Node's filesystem module. Used once, to read the firmware `.bin` file into a `Buffer`.
- `performance` — a high-resolution clock (`performance.now()` returns milliseconds as a float). Used for timeouts instead of `Date.now()` because it's monotonic — it can't jump backwards if the system clock changes.
- `SerialPort` — the third-party library (`serialport` on npm) that actually opens a COM port / `/dev/tty*` device and lets you read and write bytes.

---

## 2. Protocol constants

```ts
const PACKET_LENGTH_BYTES   = 1;
const PACKET_DATA_BYTES     = 16;
const PACKET_CRC_BYTES      = 1;
const PACKET_CRC_INDEX      = PACKET_LENGTH_BYTES + PACKET_DATA_BYTES;
const PACKET_LENGTH         = PACKET_LENGTH_BYTES + PACKET_DATA_BYTES + PACKET_CRC_BYTES;
```

These describe the fixed 18-byte packet shape from [[packet-protocol]]:

| Constant | Value | Meaning |
|---|---|---|
| `PACKET_LENGTH_BYTES` | 1 | The length field is always 1 byte |
| `PACKET_DATA_BYTES` | 16 | The data field is always 16 bytes (padded with `0xFF` if unused) |
| `PACKET_CRC_BYTES` | 1 | The CRC field is always 1 byte |
| `PACKET_CRC_INDEX` | 17 | The CRC byte sits at offset 17 within a packet (1 length + 16 data) |
| `PACKET_LENGTH` | 18 | Total size of one packet on the wire |

Writing them as sums of the smaller constants (rather than hardcoding `17` and `18`) means if the data size ever changed, only `PACKET_DATA_BYTES` needs editing — everything else recalculates.

```ts
const PACKET_ACK_DATA0      = 0x15;
const PACKET_RETX_DATA0     = 0x19;
const BL_PACKET_SYNC_OBSERVED_DATA0 = (0x20);

const BL_PACKET_FW_UPDATE_REQ_DATA0 = (0x31);
const BL_PACKET_FW_UPDATE_RES_DATA0 = (0x37);
const BL_PACKET_DEVICE_ID_REQ_DATA0 = (0x3C);
const BL_PACKET_DEVICE_ID_RES_DATA0 = (0x3F);
const BL_PACKET_FW_LENGTH_REQ_DATA0 = (0x42);
const BL_PACKET_FW_LENGTH_RES_DATA0 = (0x45);
const BL_PACKET_READY_FOR_DATA_DATA0 = (0x48);
const BL_PACKET_UPDATE_SUCCESSFUL_DATA0 = (0x54);
const BL_PACKET_NACK_DATA0 = (0x59);
```

These are the "message type" bytes that go in `data[0]` of a single-byte packet — the same values defined on the bootloader side in `bootloader.c` and `comms.h`. `DATA0` in the name is a reminder that this is *byte 0 of the data field*, not the whole packet. This file and the bootloader **must** agree on every one of these values, or the two sides will misunderstand each other even though the CRC checks out fine (the packet would be perfectly valid — just meaningless to the receiver).

The naming pattern `..._REQ_...` / `..._RES_...` marks request/response pairs — e.g. the bootloader sends `DEVICE_ID_REQ`, the host answers with `DEVICE_ID_RES`.

```ts
const DEVICE_ID = (0x42);
```

An arbitrary constant both sides agree identifies "this specific board type." The bootloader checks the ID the host sends back matches this before continuing — a basic sanity check against flashing the wrong firmware to the wrong hardware.

```ts
const SYNC_SEQ_0 = (0xc4);
const SYNC_SEQ_1 = (0x55);
const SYNC_SEQ_2 = (0x7e);
const SYNC_SEQ_3 = (0x10);
const SYNC_SEQ=Buffer.from([SYNC_SEQ_0, SYNC_SEQ_1, SYNC_SEQ_2, SYNC_SEQ_3]);
```

A 4-byte magic sequence, unlikely to appear by accident in random noise, that tells the bootloader "a real updater is here, stop waiting and start the packet protocol." `Buffer.from([...])` turns the four separate numbers into one `Buffer` (Node's byte-array type) so it can be written to the serial port in one call.

```ts
const DEFAULT_TIMEOUT = (30000);
```

30,000 ms = 30 seconds. Used as the default "give up and throw an error" time for any wait — waiting for sync, waiting for a reply packet, etc. Every wait function accepts an optional `timeout` parameter that overrides this.

```ts
const DEVICE_ID_OFFSET = 0x1b4;
```

A byte offset into the firmware `.bin` file. This isn't part of the wire protocol — it's a sanity check (see §11) that the file looks big enough to plausibly be a real firmware image, catching an obviously-wrong file (like a text file) before wasting time talking to the board.

```ts
// Must match MAX_FW_LENGTH in bootloader.c: 256 KiB flash minus the 32 KiB bootloader.
const MAX_FW_LENGTH = (256 * 1024) - 0x8000;
```

The STM32F103 on this board has 256 KiB of flash. The bottom 32 KiB (`0x8000`) is reserved for the bootloader itself, so the application can be at most `256*1024 - 0x8000` = 229,376 bytes. This mirrors `MAX_FW_LENGTH` computed in `bootloader.c` from `FLASH_END_ADDRESS - MAIN_APP_START_ADDRESS`. Checking it here means an oversized file is rejected instantly instead of failing confusingly partway through the transfer.

```ts
const firmwarePath          = process.argv[2];
const serialPath            = process.argv[3] ?? process.env.SERIAL_PORT;
const baudRate              = 115200;
```

- `process.argv` is `[node executable, script path, ...user args]`, so `argv[2]` is the first real argument — the firmware file path.
- `argv[3] ?? process.env.SERIAL_PORT` — the second argument, or (if not given) fall back to the `SERIAL_PORT` environment variable. `??` is the nullish-coalescing operator: it only falls through on `null`/`undefined`, not on falsy-but-real values like `0` or `''`.
- `baudRate` is fixed at 115200 — must match whatever the bootloader's UART is configured for (see [[uart]]).

---

## 3. CRC8

```ts
const crc8 = (data: Buffer | Array<number>) => {
  let crc = 0;

  for (const byte of data) {
    crc = (crc ^ byte) & 0xff;
    for (let i = 0; i < 8; i++) {
      if (crc & 0x80) {
        crc = ((crc << 1) ^ 0x07) & 0xff;
      } else {
        crc = (crc << 1) & 0xff;
      }
    }
  }

  return crc;
};
```

This is a textbook bitwise CRC-8 with polynomial `0x07`, processing one byte of input per outer-loop iteration and one bit per inner-loop iteration:

1. `crc ^ byte` mixes the next input byte into the running checksum.
2. The inner loop runs 8 times (once per bit of that byte): if the top bit of `crc` is set, shift left and XOR with the polynomial `0x07`; otherwise just shift left.
3. `& 0xff` after every operation keeps the value clamped to a single byte, since JavaScript numbers don't wrap at 8 bits on their own.

The type `Buffer | Array<number>` means this works whether you pass a real `Buffer` or a plain array — both are iterable with `for...of`, yielding numbers either way. This must produce **bit-for-bit identical** output to whatever CRC-8 routine the bootloader's C code uses, or every packet will look corrupted to one side or the other.

---

## 4. Small helpers

```ts
// Async delay function, which gives the event loop time to process outside input
const delay = (ms: number) => new Promise(r => setTimeout(r, ms));
```

Turns the callback-based `setTimeout` into something you can `await`. `await delay(5)` pauses this `async` function for 5ms *without blocking Node's event loop* — incoming serial data can still be processed by the `onData` handler while you're "waiting." This is the trick that lets a single-threaded Node program poll for data without a busy-loop.

```ts
class Logger {
  static info(message: string) { console.log(`[.] ${message}`); }
  static success(message: string) { console.log(`[$] ${message}`); }
  static error(message: string) { console.log(`[!] ${message}`); }
  static debug(message: string) { console.log(`[>] ${message}`); }
}
```

A tiny namespacing trick: rather than four loose functions, they're grouped as `static` methods on a class that's never instantiated. Each just prefixes a message with a symbol so the console output is easy to scan by eye — `[.]` for general progress, `[$]` for a completed step, `[!]` for errors, `[>]` for verbose wire-level tracing.

```ts
// Pretty-print a buffer as space-separated hex bytes, for tracing raw wire data.
const toHex = (data: Buffer | Array<number>) => [...data].map(b => b.toString(16).padStart(2, '0')).join(' ');
```

`[...data]` spreads the Buffer/array into a plain array of numbers. `.map(b => b.toString(16).padStart(2, '0'))` converts each byte to 2-digit hex (`5` → `'05'`, `255` → `'ff'`). `.join(' ')` glues them together with spaces, e.g. `Buffer.from([0xc4, 0x55])` → `"c4 55"`. Purely a debug-logging convenience.

---

## 5. The `Packet` class

```ts
class Packet {
  length: number;
  data: Buffer;
  crc: number;

  static retx = new Packet(1, Buffer.from([PACKET_RETX_DATA0]));
  static ack = new Packet(1, Buffer.from([PACKET_ACK_DATA0]));
```

Three instance fields mirror the three parts of the wire format. `static retx` and `static ack` are pre-built singleton packets — since a RETX or ACK packet is always the same 18 bytes, there's no reason to construct a new one every time; both are built once, immediately, using the constructor below.

```ts
  constructor(length: number, data: Buffer, crc?: number) {
    if (!Number.isInteger(length) || length < 0 || length > PACKET_DATA_BYTES ||
        data.length > PACKET_DATA_BYTES || length > data.length) {
      throw new Error('Invalid packet length');
    }
    this.length = length;
    this.data = data;
```

Guards against building a nonsensical packet: `length` must be a whole number, not negative, not bigger than 16, the actual data given can't be bigger than 16 bytes, and `length` can't claim more real bytes than were actually supplied. Throwing here means a bug elsewhere in the program fails loudly and immediately, rather than silently sending garbage over the wire.

```ts
    const bytesToPad = PACKET_DATA_BYTES - this.data.length;
    const padding = Buffer.alloc(bytesToPad).fill(0xff);
    this.data = Buffer.concat([this.data, padding]);
```

If less than 16 bytes of real data were given (e.g. a single-byte command packet), pad the rest out to exactly 16 bytes with `0xFF`, matching the fixed-size wire format. `Buffer.alloc(n)` makes an `n`-byte buffer of zeros; `.fill(0xff)` overwrites it all with `0xFF`. `Buffer.concat` joins the real data and the padding into one 16-byte buffer.

```ts
    if (typeof crc === 'undefined') {
      this.crc = this.computeCrc();
    } else {
      this.crc = crc;
    }
  }
```

`crc` is an optional third constructor argument. When **building a packet to send**, you don't know the CRC yet — you leave it out and the constructor computes it from `length` + `data`. When **parsing a packet received off the wire**, you already have a CRC byte that came with it, so you pass it in explicitly (see §7) — the constructor stores that value as-is, so it can be checked against a freshly computed one to detect corruption.

```ts
  computeCrc() {
    const allData = [this.length, ...this.data];
    return crc8(allData);
  }
```

Builds the exact byte sequence the CRC covers — the length byte followed by all 16 (already-padded) data bytes — and runs it through `crc8`. `[this.length, ...this.data]` spreads the Buffer's bytes into a plain array with `this.length` prepended.

```ts
  toBuffer() {
    return Buffer.concat([ Buffer.from([this.length]), this.data, Buffer.from([this.crc]) ]);
  }
```

Serializes the packet object back into the raw 18-byte wire format: 1 length byte + 16 data bytes + 1 CRC byte, in that order. This is what actually gets written to the serial port.

```ts
  isSingleBytePacket(byte: number) {
    if (this.length !== 1) return false;
    if (this.data[0] !== byte) return false;
    for (let i = 1; i < PACKET_DATA_BYTES; i++) {
      if (this.data[i] !== 0xff) return false;
    }
    return true;
  }
```

Checks whether this packet is a "command" packet carrying exactly one meaningful byte, equal to `byte`, with every other data slot still holding the `0xFF` padding value. This is how the code recognizes ACK, RETX, SYNC_OBSERVED, NACK, and every other one-byte protocol message — by shape and content, not by a dedicated field.

```ts
  isAck() {
    return this.isSingleBytePacket(PACKET_ACK_DATA0);
  }

  isRetx() {
    return this.isSingleBytePacket(PACKET_RETX_DATA0);
  }
```

Thin, readable wrappers around `isSingleBytePacket` for the two most commonly checked packet types.

```ts
  static createSingleBytePacket(byte: number) {
    return new Packet(1, Buffer.from([byte]));
  }
}
```

A factory for building a fresh single-byte command packet (length 1, data `[byte, 0xff, 0xff, ...]`, CRC computed automatically) — used every time the code needs to send something like `BL_PACKET_FW_UPDATE_REQ_DATA0`.

---

## 6. Serial port state and low-level writes

```ts
// Serial port instance
let uart: SerialPort;
let serialError: Error | undefined;
const checkSerialError = () => {
  if (serialError) throw serialError;
};
```

`uart` holds the open `SerialPort` object once connected (assigned in `openSerialPort`, §9). `serialError` is a module-level flag: if the serial port ever reports an error (device unplugged, write failure, etc.), it's stashed here rather than thrown immediately, because it might happen inside a callback where there's no `try/catch` listening. `checkSerialError()` is called at the top of any wait loop so a stashed error surfaces as soon as possible instead of the program hanging forever waiting for a reply that will never come.

```ts
const writeBytes = (data: Buffer) => {
  checkSerialError();
  Logger.debug(`TX ${data.length} bytes: ${toHex(data)}`);
  uart.write(data, error => {
    if (error) serialError = error;
  });
};
```

The single choke point for all outgoing bytes: check for a prior error first, log what's about to go out (with the byte count and full hex dump), then write it. The callback passed to `uart.write` captures any write-specific error into `serialError` for the next `checkSerialError()` call to catch.

```ts
// Packet buffer
let packets: Packet[] = [];
let bootloaderInPacketMode = false;
```

`packets` is a FIFO queue of fully-parsed, validated, in-order packets that have arrived from the bootloader — the `onData` handler (§8) pushes into it, and `waitForPacket` (§9) pops from it. `bootloaderInPacketMode` is a flag explained in §8/§9's discussion of the RETX edge case.

```ts
let lastPacket: Packet | undefined;
const writePacket = (packet: Packet) => {
  if (!packet.isAck() && !packet.isRetx()) lastPacket = packet;
  writeBytes(packet.toBuffer());
};
```

`writePacket` is the higher-level sibling of `writeBytes` — it takes a `Packet` object rather than raw bytes. Before sending, it remembers the packet as `lastPacket`, **unless** it's an ACK or RETX. That exclusion matters: if the bootloader ever asks "please resend" (sends RETX), the host needs to resend the last *meaningful* packet — not accidentally resend an ACK or a previous RETX, which would desync the protocol.

---

## 7. Receiving bytes and reassembling packets

```ts
// Serial data buffer, with a splice-like function for consuming data
let rxBuffer = Buffer.from([]);
const consumeFromBuffer = (n: number) => {
  const consumed = rxBuffer.subarray(0, n);
  rxBuffer = rxBuffer.subarray(n);
  return consumed;
};
```

Serial data doesn't arrive neatly split into 18-byte chunks — the OS can deliver it in any grouping (half a packet, three packets at once, one byte at a time). `rxBuffer` accumulates every byte that's arrived but hasn't been consumed into a packet yet. `consumeFromBuffer(n)` takes the first `n` bytes off the front and leaves the rest — `.subarray` is used (not `.slice`) because it returns a *view* into the same underlying memory rather than copying, which is cheaper (note: it means `consumed` and the leftover `rxBuffer` share memory with the original until one of them is written to — fine here since neither is mutated after the split).

```ts
// This function fires whenever data is received over the serial port. The whole
// packet state machine runs here.
const onData = (data: Buffer) => {
  if (serialError) return;
  Logger.debug(`RX ${data.length} bytes: ${toHex(data)}`);
  // Add the data to the packet
  rxBuffer = Buffer.concat([rxBuffer, data]);
```

This is the callback wired up to the serial port's `'data'` event (see §9) — it fires every time the OS hands Node a fresh chunk of bytes. If a serial error already happened, bail out immediately rather than keep processing. Otherwise, log the raw chunk and append it onto whatever's already sitting in `rxBuffer`.

```ts
  // Can we build a packet?
  while (rxBuffer.length >= PACKET_LENGTH) {
    const raw = consumeFromBuffer(PACKET_LENGTH);
```

Loops as long as there's at least one full 18-byte packet's worth of bytes waiting — handling the case where several packets arrived in a single chunk. Each iteration pulls exactly one packet's worth off the front.

```ts
    if (raw[0] > PACKET_DATA_BYTES) {
      Logger.debug(`Invalid length byte 0x${raw[0].toString(16)} (max ${PACKET_DATA_BYTES}) -> sending RETX`);
      writePacket(Packet.retx);
      continue;
    }
```

`raw[0]` is the length byte. If it claims more than 16 data bytes, the frame can't be trusted (either the two sides are out of sync, or this "packet" is actually noise/garbage). Rather than let the `Packet` constructor throw, this checks it up front, logs what happened, asks the bootloader to resend by sending a RETX, and `continue`s the loop to look for the next 18 bytes (which, if things are simply desynced, gives the framing a chance to realign one byte at a time... though see the caveat about this in [[firmware-updater-sync-debugging]]).

```ts
    const packet = new Packet(raw[0], raw.subarray(1, 1 + PACKET_DATA_BYTES), raw[PACKET_CRC_INDEX]);
    const computedCrc = packet.computeCrc();
```

Now safe to construct a real `Packet`: length is `raw[0]`, data is the 16 bytes after it, and the CRC is explicitly passed in as the byte at offset 17 (`PACKET_CRC_INDEX`) — this is the "I already have a CRC, don't compute one" branch of the constructor from §5. `computeCrc()` is then called separately to get what the CRC *should* be, so it can be compared against what was actually received.

```ts
    // Need retransmission?
    if (packet.crc !== computedCrc) {
      Logger.debug(`CRC mismatch: computed 0x${computedCrc.toString(16)}, got 0x${packet.crc.toString(16)} -> sending RETX`);
      writePacket(Packet.retx);
      continue;
    }
```

If the received CRC doesn't match what was computed from the received length+data, the packet was corrupted in transit. Ask for a resend and move on to the next 18 bytes in the buffer.

```ts
    // Are we being asked to retransmit?
    if (packet.isRetx()) {
      Logger.debug(lastPacket ? 'RETX received -> resending last packet' : 'RETX received but no last packet to resend');
      if (lastPacket) {
        writePacket(lastPacket);
      } else {
        bootloaderInPacketMode = true;
      }
      continue;
    }
```

The packet passed its CRC check and turned out to *be* a RETX request from the bootloader (meaning the bootloader received something corrupted from *us* and wants it resent). If there's a `lastPacket` remembered, resend it. If not — meaning the host hasn't sent any real packet yet — this must be a RETX left over from the sync handshake (the bootloader replies with RETX if it's already past the sync stage and receives more raw sync bytes it doesn't recognize as a valid packet). That's used as a **signal** that the bootloader is already in packet mode, recorded in `bootloaderInPacketMode` for `syncWithBootloader` (§9) to notice.

```ts
    // If this is an ack, move on
    if (packet.isAck()) {
      Logger.debug('ACK received');
      continue;
    }
```

An ACK just confirms the bootloader received the last data packet — nothing further to do with it besides logging, so skip to the next chunk of `rxBuffer`.

```ts
    // Otherwise write the packet in to the buffer, and send an ack
    Logger.debug(`Data packet queued: length=${packet.length} data=${toHex(packet.data.subarray(0, packet.length))} -> sending ACK`);
    packets.push(packet);
    writePacket(Packet.ack);
  }
};
```

Anything that reaches here is a genuine, meaningful packet from the bootloader (not corrupted, not a RETX, not an ACK) — e.g. `BL_PACKET_FW_UPDATE_RES_DATA0`, `BL_PACKET_DEVICE_ID_REQ_DATA0`, etc. It's pushed onto the `packets` queue for the rest of the program to consume, and an ACK is sent back to confirm receipt to the bootloader. `packet.data.subarray(0, packet.length)` in the log line trims off the `0xFF` padding so only the meaningful bytes are shown.

---

## 8. Waiting for packets

```ts
// Function to allow us to await a packet
const waitForPacket = async (timeout = DEFAULT_TIMEOUT) => {
  const deadline = performance.now() + timeout;
  while (packets.length < 1) {
    checkSerialError();
    if (performance.now() >= deadline) {
      throw Error('Timed out waiting for packet');
    }
    await delay(5);
  }
  checkSerialError();
  return packets.splice(0, 1)[0];
}
```

This bridges the event-driven `onData` callback with the rest of the code, which is written as sequential `async`/`await` logic. `deadline` is computed once, up front, as an absolute point in time (not "keep waiting 30s from *now* each time," which would never time out if data trickled in slowly). The loop polls every 5ms: check for a stashed serial error, check whether the deadline passed (throw if so), otherwise `await delay(5)` and check again. `packets.length < 1` as the loop condition means as soon as `onData` pushes something onto `packets`, the very next poll sees it and exits the loop. `packets.splice(0, 1)[0]` removes and returns the oldest queued packet (FIFO — first packet in is the first one handed out).

```ts
const waitForSingleBytePacket = (byte: number, timeout = DEFAULT_TIMEOUT) => (
  waitForPacket(timeout)
    .then(packet => {
      if (packet.isSingleBytePacket(BL_PACKET_NACK_DATA0)) {
        throw new Error('Bootloader returned NACK');
      }
      if (!packet.isSingleBytePacket(byte)) {
        throw new Error(`Unexpected packet received. Expected single byte 0x${byte.toString(16)}, got packet ${toHex(packet.toBuffer())}`);
      }
    })
);
```

A convenience wrapper built on top of `waitForPacket` for the extremely common case of "wait for one specific single-byte command packet." It's written with `.then()` instead of `async/await` here, but behaves the same way — it returns a `Promise` that resolves once the right packet shows up. Two failure modes: if the bootloader sends a NACK, that's treated as a hard failure regardless of what was being waited for (the bootloader is explicitly saying "something went wrong, abort"); otherwise, if what arrived isn't the specific single-byte packet expected, throw a descriptive error showing exactly what came in instead (using `toHex` on the full 18-byte buffer, for debugging).

---

## 9. Syncing with the bootloader

```ts
const SYNC_RETRY_INTERVAL = 1000;

// Send the sync sequence repeatedly until the bootloader answers with
// SYNC_OBSERVED (or is already in packet mode and answers with RETX).
const syncWithBootloader = async (timeout = DEFAULT_TIMEOUT) => {
  const deadline = performance.now() + timeout;
  while (true) {
    writeBytes(SYNC_SEQ);
    const retryAt = performance.now() + SYNC_RETRY_INTERVAL;
```

The board might be freshly reset and waiting for the 4-byte `SYNC_SEQ` (see `bootloader.c`'s `BL_State_Sync`), so the outer loop repeatedly sends `SYNC_SEQ` once per second until something recognizable comes back. `retryAt` marks one second from now — the point at which, if nothing useful has arrived, the sync bytes get sent again.

```ts
    while (performance.now() < retryAt) {
      checkSerialError();
      while (packets.length > 0) {
        const packet = packets.splice(0, 1)[0];
        if (packet.isSingleBytePacket(BL_PACKET_SYNC_OBSERVED_DATA0)) {
          Logger.success('Bootloader sync successful');
          return;
        }
        Logger.debug(`Unexpected packet received while syncing: length=${packet.length} data=${toHex(packet.data.subarray(0, packet.length))}`);
      }
```

For up to one second after each send, poll for any packets that have shown up. Note this drains the *entire* `packets` queue each pass (the inner `while (packets.length > 0)`), not just one packet — since multiple stray packets could have queued up while waiting. If a `SYNC_OBSERVED` packet turns up, sync succeeded; return immediately. Anything else queued is logged and discarded — it's not something sync cares about.

```ts
      if (bootloaderInPacketMode) {
        Logger.success('Bootloader sync successful');
        return;
      }
      await delay(5);
    }
```

This is the RETX-based edge case from §7: if the bootloader had already moved past its sync state (e.g. this is a retry after a partial run, or it was never reset), sending it raw `SYNC_SEQ` bytes looks like a malformed/corrupt packet to the bootloader's packet parser, so it replies with RETX instead of `SYNC_OBSERVED`. `onData` catches that in the "no `lastPacket`" branch and sets `bootloaderInPacketMode = true` as a signal. Checking that flag here lets sync succeed even in that situation, without needing to see the literal `SYNC_OBSERVED` byte. Otherwise, sleep 5ms and poll again until `retryAt`.

```ts
    if (performance.now() >= deadline) {
      throw Error('Timed out waiting for bootloader sync');
    }
  }
}
```

Once the one-second retry window closes without success, check the *overall* deadline (not the retry interval) — if the full timeout has elapsed, give up with an error. Otherwise the outer `while (true)` loops around and sends `SYNC_SEQ` again.

---

## 10. Opening and closing the port

```ts
const openSerialPort = async () => {
  if (!serialPath) {
    throw new Error('No serial port specified. Pass it as an argument or set SERIAL_PORT.');
  }

  uart = new SerialPort({ path: serialPath, baudRate, autoOpen: false });

  uart.on('data', onData);
  uart.on('error', error => { serialError = error; });

  await new Promise<void>((resolve, reject) => {
    uart.open(error => (error ? reject(error) : resolve()));
  });

  Logger.info(`Connected to ${serialPath} at ${baudRate} baud`);
}
```

Fails fast with a clear message if no port was given at all. `autoOpen: false` means constructing the `SerialPort` object doesn't immediately try to open the device — the two event listeners (`'data'` → `onData`, `'error'` → stash into `serialError`) are attached first, so nothing can slip through before they're wired up. Then `uart.open(...)` is wrapped in a `new Promise` — this is the standard pattern for converting a callback-style API into something `await`-able: resolve on success, reject with the error on failure. Only after the port genuinely opens does it log the connection.

```ts
const closeSerialPort = async () => {
  if (!uart || !uart.isOpen) return;

  await new Promise<void>(resolve => {
    uart.close(() => resolve());
  });
}
```

Safe to call even if the port was never opened (`!uart`) or already closed (`!uart.isOpen`) — used in a `finally` block later (§12), which runs whether the update succeeded or failed, so it must never itself throw.

---

## 11. Running the update

```ts
const sendPacketAndWaitFor = async (packet: Packet, expectedByte: number) => {
  writePacket(packet);
  await waitForSingleBytePacket(expectedByte);
}
```

A tiny helper for the extremely common "send this, then wait for that specific reply" pattern that makes up most of the handshake.

```ts
const runFirmwareUpdate = async (firmware: Buffer) => {
  Logger.info('Attempting to sync with bootloader...');
  await syncWithBootloader();

  await sendPacketAndWaitFor(
    Packet.createSingleBytePacket(BL_PACKET_FW_UPDATE_REQ_DATA0),
    BL_PACKET_FW_UPDATE_RES_DATA0,
  );
  Logger.success('Firmware update request accepted');
```

This function is the whole update sequence end-to-end, matching the bootloader's state machine one state at a time. First, sync (§9). Then send `FW_UPDATE_REQ` and wait for `FW_UPDATE_RES` — this is the bootloader's `BL_State_WaitForUpdateReq` state accepting the request and moving on.

```ts
  Logger.info('Waiting for device ID request...');
  await waitForSingleBytePacket(BL_PACKET_DEVICE_ID_REQ_DATA0);
  await sendPacketAndWaitFor(
    new Packet(2, Buffer.from([BL_PACKET_DEVICE_ID_RES_DATA0, DEVICE_ID])),
    BL_PACKET_FW_LENGTH_REQ_DATA0,
  );
  Logger.success(`Device ID 0x${DEVICE_ID.toString(16)} accepted`);
```

The bootloader initiates this step (it sends `DEVICE_ID_REQ` on its own, from `BL_State_DeviceIDReq`), so the host just waits for it. The reply is a 2-byte packet: `[BL_PACKET_DEVICE_ID_RES_DATA0, DEVICE_ID]` — the response tag followed by the actual device ID value, matching what `bootloader.c`'s `is_device_id_packet` expects at `data[0]`/`data[1]`.

```ts
  const lengthResponse = Buffer.alloc(5);
  lengthResponse[0] = BL_PACKET_FW_LENGTH_RES_DATA0;
  lengthResponse.writeUInt32LE(firmware.length, 1);
  // The bootloader erases the application area before replying, so the
  // first READY_FOR_DATA can take a few seconds to arrive.
  Logger.info(`Firmware length sent: ${firmware.length} bytes, waiting for erase...`);
  await sendPacketAndWaitFor(
    new Packet(5, lengthResponse),
    BL_PACKET_READY_FOR_DATA_DATA0,
  );
```

Same pattern as device ID, but for firmware length: the bootloader requests it (handled implicitly — the previous `sendPacketAndWaitFor` already waited for `FW_LENGTH_REQ`), and the host replies with a 5-byte packet: 1 tag byte + a 4-byte little-endian unsigned integer holding `firmware.length`. `writeUInt32LE(value, offset)` writes those 4 bytes starting at index 1 of the buffer, matching how `bootloader.c` reads `fw_length` back out of `packet.data[1..4]` byte-by-byte (also little-endian). The comment flags that the *next* reply, `READY_FOR_DATA`, is what confirms the bootloader has finished erasing flash — which is why this wait can legitimately take a few seconds, longer than the individual packet timeouts elsewhere.

```ts
  for (let offset = 0; offset < firmware.length; offset += PACKET_DATA_BYTES) {
    const chunk = firmware.subarray(offset, offset + PACKET_DATA_BYTES);
    writePacket(new Packet(chunk.length, chunk));

    const bytesWritten = offset + chunk.length;
    const expected = bytesWritten >= firmware.length
      ? BL_PACKET_UPDATE_SUCCESSFUL_DATA0
      : BL_PACKET_READY_FOR_DATA_DATA0;
    await waitForSingleBytePacket(expected);
    Logger.info(`Sent ${bytesWritten} of ${firmware.length} bytes`);
  }

  Logger.success('Firmware update complete!');
}
```

The actual data transfer: walk through `firmware` 16 bytes at a time. `chunk.length` matters here — every chunk is 16 bytes *except possibly the last one*, which can be shorter if `firmware.length` isn't a multiple of 16 (`Buffer.subarray` naturally truncates at the buffer's end rather than erroring). `new Packet(chunk.length, chunk)` correctly reports the real chunk size as the packet's length field, so the last, short packet doesn't falsely claim 16 valid bytes. After sending each chunk, decide what reply to expect: if this chunk reached the end of the firmware (`bytesWritten >= firmware.length`), the bootloader should respond with `UPDATE_SUCCESSFUL` (having validated the flashed image, per `application_is_valid()` in `bootloader.c`); otherwise it should ask for more with `READY_FOR_DATA`, exactly mirroring `BL_State_ReceiveFirmware`'s branch in the C code. Progress is logged after every chunk.

---

## 12. Entry point

```ts
const printUsage = () => {
  console.log('Usage: npm start -- <firmware.bin> [serial-port]');
  console.log('Example: npm start -- firmware.bin COM3');
  console.log('You can also set SERIAL_PORT instead of passing the serial port argument.');
}
```

Plain help text, printed both for `--help`/`-h` and for missing arguments.

```ts
// Validate the firmware image, then open the serial port and run the update.
const main = async () => {
  if (firmwarePath === '--help' || firmwarePath === '-h') {
    printUsage();
    return;
  }

  if (!firmwarePath) {
    printUsage();
    process.exitCode = 1;
    return;
  }
```

`main` is the top-level async function that actually runs (it's called once, at the very bottom of the file). It first checks for `--help`/`-h` as the first argument and exits cleanly (exit code 0) if so. Then it checks that a firmware path was given at all — if not, print usage again but this time set `process.exitCode = 1` to signal failure to the calling shell (useful for scripting: `npm start -- badcall.bin && echo ok`).

```ts
  // The image must be the application alone (linked at 0x08008000), not a
  // combined bootloader + application image.
  let firmware: Buffer;
  try {
    firmware = await fs.readFile(firmwarePath);
  } catch (error) {
    Logger.error(`Failed to read firmware: ${(error as Error).message}`);
    process.exitCode = 1;
    return;
  }
```

Read the entire firmware file into memory as one `Buffer`. `app/firmware.bin` is built to be loaded starting at `0x08008000` (right after the bootloader's 32 KiB, per `app/linkerscript.ld`) — this file is sent to the board exactly as read, with **no** slicing or offsetting, since it's the application image alone, not a combined image with the bootloader glued on the front. The comment exists specifically to prevent a past bug: an earlier version of this code mistakenly tried to strip off a "bootloader-sized" prefix that was never actually there.

`error as Error` is a TypeScript cast — caught exceptions are typed `unknown` by default (they could technically be anything thrown), so this asserts it has a `.message` property before reading it.

```ts
  if (firmware.length <= DEVICE_ID_OFFSET) {
    Logger.error(`Invalid firmware size: expected device ID byte at offset 0x${DEVICE_ID_OFFSET.toString(16)}`);
    process.exitCode = 1;
    return;
  }

  if (firmware.length > MAX_FW_LENGTH) {
    Logger.error(`Firmware too large: ${firmware.length} bytes (max ${MAX_FW_LENGTH})`);
    process.exitCode = 1;
    return;
  }
```

Two sanity checks on the file *before* ever touching the serial port, so an obviously bad file fails instantly with a clear message instead of burning through a 30-second sync timeout: it must be bigger than `DEVICE_ID_OFFSET` (large enough to plausibly be a real vector-table-containing firmware image), and it must not exceed the flash space actually available to the application (`MAX_FW_LENGTH`, §2).

```ts
  try {
    await openSerialPort();
    await runFirmwareUpdate(firmware);
  } catch (error) {
    Logger.error((error as Error).message);
    process.exitCode = 1;
  } finally {
    await closeSerialPort();
  }
};

main();
```

The actual work: open the port, then run the full update sequence. Any error thrown anywhere along that path — a failed `open()`, a sync timeout, an unexpected packet, a NACK, a dropped connection — is caught in one place, logged, and turned into a non-zero exit code. `finally` guarantees `closeSerialPort()` runs whether the update succeeded or an error was thrown, so the OS-level port handle is never left open. Finally, `main()` is called (without `await`, since it's the top level of the script) to actually kick everything off.

---

## Summary: the conversation this file has with the bootloader

Putting it all together, one successful run looks like this end-to-end (compare against the state diagram in [[firmware-update-state-machine]]):

```
 Host                                          Bootloader
  │                                                 │
  │──── SYNC_SEQ (4 bytes, repeated) ─────────────▶│  (BL_State_Sync)
  │◀─── SYNC_OBSERVED ──────────────────────────────│
  │                                                 │
  │──── FW_UPDATE_REQ ─────────────────────────────▶│  (BL_State_WaitForUpdateReq)
  │◀─── FW_UPDATE_RES ──────────────────────────────│
  │                                                 │
  │◀─── DEVICE_ID_REQ ───────────────────────────────│  (BL_State_DeviceIDReq)
  │──── DEVICE_ID_RES + DEVICE_ID ─────────────────▶│  (BL_State_DeviceIDRes)
  │                                                 │
  │◀─── FW_LENGTH_REQ ───────────────────────────────│  (BL_State_FWLengthReq)
  │──── FW_LENGTH_RES + length (4 bytes) ──────────▶│  (BL_State_FWLengthRes)
  │                                                 │  (BL_State_EraseApplication)
  │◀─── READY_FOR_DATA ──────────────────────────────│
  │                                                 │
  │──── 16-byte firmware chunk ────────────────────▶│  (BL_State_ReceiveFirmware)
  │◀─── READY_FOR_DATA (repeat until done) ──────────│
  │──── ... more chunks ... ───────────────────────▶│
  │◀─── UPDATE_SUCCESSFUL (on last chunk) ───────────│
  │                                                 │  jump_to_main()
```

Every arrow is itself wrapped in the ACK/RETX/CRC machinery from §7 — that machinery is *transparent* to the sequence above; `runFirmwareUpdate` never deals with ACKs or CRCs directly, because `onData` and `writePacket` handle that underneath it.

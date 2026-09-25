import * as fs from 'fs/promises';
import { performance } from 'node:perf_hooks';
import {SerialPort} from 'serialport';

// Constants for the packet protocol
const PACKET_LENGTH_BYTES   = 1;
const PACKET_DATA_BYTES     = 16;
const PACKET_CRC_BYTES      = 1;
const PACKET_CRC_INDEX      = PACKET_LENGTH_BYTES + PACKET_DATA_BYTES;
const PACKET_LENGTH         = PACKET_LENGTH_BYTES + PACKET_DATA_BYTES + PACKET_CRC_BYTES;

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
const DEVICE_ID = (0x42);
const SYNC_SEQ_0 = (0xc4);
const SYNC_SEQ_1 = (0x55);
const SYNC_SEQ_2 = (0x7e);
const SYNC_SEQ_3 = (0x10);
const SYNC_SEQ=Buffer.from([SYNC_SEQ_0, SYNC_SEQ_1, SYNC_SEQ_2, SYNC_SEQ_3]);
const DEFAULT_TIMEOUT = (30000);
const DEVICE_ID_OFFSET = 0x1b4;
// Must match MAX_FW_LENGTH in bootloader.c: 256 KiB flash minus the 32 KiB bootloader.
const MAX_FW_LENGTH = (256 * 1024) - 0x8000;

const firmwarePath          = process.argv[2];
const serialPath            = process.argv[3] ?? process.env.SERIAL_PORT;
const baudRate              = 115200;

// CRC8 implementation
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

// Async delay function, which gives the event loop time to process outside input
const delay = (ms: number) => new Promise(r => setTimeout(r, ms));

class Logger {
  static info(message: string) { console.log(`[.] ${message}`); }
  static success(message: string) { console.log(`[$] ${message}`); }
  static error(message: string) { console.log(`[!] ${message}`); }
  static debug(message: string) { console.log(`[>] ${message}`); }
}

// Pretty-print a buffer as space-separated hex bytes, for tracing raw wire data.
const toHex = (data: Buffer | Array<number>) => [...data].map(b => b.toString(16).padStart(2, '0')).join(' ');

// Class for serialising and deserialising packets
class Packet {
  length: number;
  data: Buffer;
  crc: number;

  static retx = new Packet(1, Buffer.from([PACKET_RETX_DATA0]));
  static ack = new Packet(1, Buffer.from([PACKET_ACK_DATA0]));

  constructor(length: number, data: Buffer, crc?: number) {
    if (!Number.isInteger(length) || length < 0 || length > PACKET_DATA_BYTES ||
        data.length > PACKET_DATA_BYTES || length > data.length) {
      throw new Error('Invalid packet length');
    }
    this.length = length;
    this.data = data;

    const bytesToPad = PACKET_DATA_BYTES - this.data.length;
    const padding = Buffer.alloc(bytesToPad).fill(0xff);
    this.data = Buffer.concat([this.data, padding]);

    if (typeof crc === 'undefined') {
      this.crc = this.computeCrc();
    } else {
      this.crc = crc;
    }
  }

  computeCrc() {
    const allData = [this.length, ...this.data];
    return crc8(allData);
  }

  toBuffer() {
    return Buffer.concat([ Buffer.from([this.length]), this.data, Buffer.from([this.crc]) ]);
  }

  isSingleBytePacket(byte: number) {
    if (this.length !== 1) return false;
    if (this.data[0] !== byte) return false;
    for (let i = 1; i < PACKET_DATA_BYTES; i++) {
      if (this.data[i] !== 0xff) return false;
    }
    return true;
  }

  isAck() {
    return this.isSingleBytePacket(PACKET_ACK_DATA0);
  }

  isRetx() {
    return this.isSingleBytePacket(PACKET_RETX_DATA0);
  }

  static createSingleBytePacket(byte: number) {
    return new Packet(1, Buffer.from([byte]));
  }
}

// Serial port instance
let uart: SerialPort;
let serialError: Error | undefined;
const checkSerialError = () => {
  if (serialError) throw serialError;
};
const writeBytes = (data: Buffer) => {
  checkSerialError();
  Logger.debug(`TX ${data.length} bytes: ${toHex(data)}`);
  uart.write(data, error => {
    if (error) serialError = error;
  });
};

// Packet buffer
let packets: Packet[] = [];
let bootloaderInPacketMode = false;

let lastPacket: Packet | undefined;
const writePacket = (packet: Packet) => {
  if (!packet.isAck() && !packet.isRetx()) lastPacket = packet;
  writeBytes(packet.toBuffer());
};

// Serial data buffer, with a splice-like function for consuming data
let rxBuffer = Buffer.from([]);
const consumeFromBuffer = (n: number) => {
  const consumed = rxBuffer.subarray(0, n);
  rxBuffer = rxBuffer.subarray(n);
  return consumed;
};

// This function fires whenever data is received over the serial port. The whole
// packet state machine runs here.
const onData = (data: Buffer) => {
  if (serialError) return;
  Logger.debug(`RX ${data.length} bytes: ${toHex(data)}`);
  // Add the data to the packet
  rxBuffer = Buffer.concat([rxBuffer, data]);

  // Can we build a packet?
  while (rxBuffer.length >= PACKET_LENGTH) {
    // A garbage length byte here almost always means the buffer has slid out
    // of frame alignment (e.g. one stray extra byte arrived somewhere), not
    // that a real frame got corrupted. Asking for a retransmit wouldn't fix
    // that - the reply just lands at the same broken offset and repeats
    // forever. So instead of discarding a full frame's worth, drop a single
    // byte and re-check, letting the parser slide until it finds a real
    // frame boundary again.
    if (rxBuffer[0] > PACKET_DATA_BYTES) {
      Logger.debug(`Invalid length byte 0x${rxBuffer[0].toString(16)} (max ${PACKET_DATA_BYTES}) -> resyncing`);
      consumeFromBuffer(1);
      continue;
    }

    const raw = consumeFromBuffer(PACKET_LENGTH);
    const packet = new Packet(raw[0], raw.subarray(1, 1 + PACKET_DATA_BYTES), raw[PACKET_CRC_INDEX]);
    const computedCrc = packet.computeCrc();

    // Need retransmission?
    if (packet.crc !== computedCrc) {
      Logger.debug(`CRC mismatch: computed 0x${computedCrc.toString(16)}, got 0x${packet.crc.toString(16)} -> sending RETX`);
      writePacket(Packet.retx);
      continue;
    }

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

    // If this is an ack, move on
    if (packet.isAck()) {
      Logger.debug('ACK received');
      continue;
    }

    // Otherwise write the packet in to the buffer, and send an ack
    Logger.debug(`Data packet queued: length=${packet.length} data=${toHex(packet.data.subarray(0, packet.length))} -> sending ACK`);
    packets.push(packet);
    writePacket(Packet.ack);
  }
};

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

const SYNC_RETRY_INTERVAL = 1000;

// Send the sync sequence repeatedly until the bootloader answers with
// SYNC_OBSERVED (or is already in packet mode and answers with RETX).
const syncWithBootloader = async (timeout = DEFAULT_TIMEOUT) => {
  const deadline = performance.now() + timeout;
  while (true) {
    writeBytes(SYNC_SEQ);
    const retryAt = performance.now() + SYNC_RETRY_INTERVAL;

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

      if (bootloaderInPacketMode) {
        Logger.success('Bootloader sync successful');
        return;
      }
      await delay(5);
    }

    if (performance.now() >= deadline) {
      throw Error('Timed out waiting for bootloader sync');
    }
  }
}

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

  // Discard anything the OS driver already had buffered from before this
  // process opened the port. Without this, stale bytes left over from a
  // previous (e.g. timed-out) run can be misread as a live reply the moment
  // we start listening, making sync look like it succeeded against a
  // bootloader that has actually gone silent.
  await new Promise<void>((resolve, reject) => {
    uart.flush(error => (error ? reject(error) : resolve()));
  });

  Logger.info(`Connected to ${serialPath} at ${baudRate} baud`);
}

const closeSerialPort = async () => {
  if (!uart || !uart.isOpen) return;

  await new Promise<void>(resolve => {
    uart.close(() => resolve());
  });
}

const sendPacketAndWaitFor = async (packet: Packet, expectedByte: number) => {
  writePacket(packet);
  await waitForSingleBytePacket(expectedByte);
}

const runFirmwareUpdate = async (firmware: Buffer) => {
  Logger.info('Attempting to sync with bootloader...');
  await syncWithBootloader();

  await sendPacketAndWaitFor(
    Packet.createSingleBytePacket(BL_PACKET_FW_UPDATE_REQ_DATA0),
    BL_PACKET_FW_UPDATE_RES_DATA0,
  );
  Logger.success('Firmware update request accepted');

  Logger.info('Waiting for device ID request...');
  await waitForSingleBytePacket(BL_PACKET_DEVICE_ID_REQ_DATA0);
  await sendPacketAndWaitFor(
    new Packet(2, Buffer.from([BL_PACKET_DEVICE_ID_RES_DATA0, DEVICE_ID])),
    BL_PACKET_FW_LENGTH_REQ_DATA0,
  );
  Logger.success(`Device ID 0x${DEVICE_ID.toString(16)} accepted`);

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

const printUsage = () => {
  console.log('Usage: npm start -- <firmware.bin> [serial-port]');
  console.log('Example: npm start -- firmware.bin COM3');
  console.log('You can also set SERIAL_PORT instead of passing the serial port argument.');
}

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


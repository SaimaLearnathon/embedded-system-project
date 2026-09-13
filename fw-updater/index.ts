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






const DEFAULT_TIMEOUT  = (5000);

// Details about the serial port connection
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

let lastPacket: Packet | undefined;
const writePacket = (packet: Packet) => {
  if (!packet.isAck() && !packet.isRetx()) lastPacket = packet;
  writeBytes(packet.toBuffer());
};

// Serial data buffer, with a splice-like function for consuming data
let rxBuffer = Buffer.from([]);
const consumeFromBuffer = (n: number) => {
  const consumed = rxBuffer.slice(0, n);
  rxBuffer = rxBuffer.slice(n);
  return consumed;
}

// This function fires whenever data is received over the serial port. The whole
// packet state machine runs here.
const onData = (data: Buffer) => {
  if (serialError) return;
  Logger.debug(`RX ${data.length} bytes: ${toHex(data)}`);
  // Add the data to the packet
  rxBuffer = Buffer.concat([rxBuffer, data]);

  // Can we build a packet?
  while (rxBuffer.length >= PACKET_LENGTH) {
    const raw = consumeFromBuffer(PACKET_LENGTH);
    if (raw[0] > PACKET_DATA_BYTES) {
      Logger.debug(`Invalid length byte 0x${raw[0].toString(16)} (max ${PACKET_DATA_BYTES}) -> sending RETX`);
      writePacket(Packet.retx);
      continue;
    }
    const packet = new Packet(raw[0], raw.slice(1, 1+PACKET_DATA_BYTES), raw[PACKET_CRC_INDEX]);
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
      if (lastPacket) writePacket(lastPacket);
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
      if (!packet.isSingleBytePacket(byte)) {
        const formattedPacket = [...packet.toBuffer()].map(x => x.toString(16)).join(' ');
        throw new Error(`Unexpected packet received. Expected single byte 0x${byte.toString(16)}, got packet ${formattedPacket}`);
      }
    })
);

// Open the serial port and log every packet the bootloader sends, so the
// comms layer can be tested against the board before anything is built on top of it.
const main = async () => {
  if (!serialPath) {
    Logger.error('No serial port specified. Pass it as an argument or set SERIAL_PORT.');
    process.exitCode = 1;
    return;
  }

  try {
    await new Promise<void>((resolve, reject) => {
      uart = new SerialPort({ path: serialPath, baudRate }, error => (error ? reject(error) : resolve()));
    });
  } catch (error) {
    Logger.error(`Failed to open serial port: ${(error as Error).message}`);
    process.exitCode = 1;
    return;
  }

  uart.on('data', onData);
  uart.on('error', error => { serialError = error; });

  Logger.info(`Connected to ${serialPath} at ${baudRate} baud`);

  for (;;) {
    try {
      const packet = await waitForPacket();
      const bytes = [...packet.data.subarray(0, packet.length)].map(b => b.toString(16).padStart(2, '0')).join(' ');
      Logger.success(`Received packet: length=${packet.length} data=${bytes}`);
    } catch (error) {
      Logger.error((error as Error).message);
      process.exitCode = 1;
      break;
    }
  }
};

main();


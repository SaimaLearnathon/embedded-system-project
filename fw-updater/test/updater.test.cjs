const { test } = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../dist/index.js'), 'utf8')
  .replace(/\nmain\(\)/, '\nglobalThis.completed = main()');

function packet(value) {
  const bytes = Buffer.alloc(18, 0xff);
  bytes[0] = 1;
  bytes[1] = value;
  let crc = 0;
  for (const byte of bytes.subarray(0, 17)) {
    crc ^= byte;
    for (let i = 0; i < 8; i++) crc = ((crc << 1) ^ ((crc & 128) ? 7 : 0)) & 255;
  }
  bytes[17] = crc;
  return bytes;
}

async function run(options = {}) {
  const image = Buffer.alloc(options.size ?? 449, 0xa5);
  const sent = [];
  const logs = [];
  let port;
  let phase = 'sync';
  let received = 0;
  let previous;
  let retried = false;
  class MockPort extends EventEmitter {
    constructor(config) { super(); port = this; this.config = config; this.isOpen = false; }
    open(callback) {
      this.isOpen = !options.openError;
      callback(options.openError ? new Error('Cannot open COM_TEST') : null);
    }
    reply(...values) {
      const bytes = Buffer.concat(values.map(packet));
      // Exercise framing across partial and coalesced serial reads.
      this.emit('data', bytes.subarray(0, 7));
      this.emit('data', bytes.subarray(7));
    }
    write(bytes, callback) {
      sent.push(Buffer.from(bytes));
      callback?.(null);
      if (bytes.length === 4) { this.reply(0x20); phase = 'request'; return; }
      if (bytes[0] === 1 && [0x15, 0x19].includes(bytes[1])) return;
      if (phase === 'request') {
        if (options.nack) { this.reply(0x59); return; }
        assert.equal(bytes[1], 0x31);
        phase = 'device'; this.reply(0x37, 0x3c);
      } else if (phase === 'device') {
        assert.equal(bytes[1], 0x3f);
        assert.equal(bytes[2], 0x42);
        phase = 'length'; this.reply(0x42);
      } else if (phase === 'length') {
        assert.equal(bytes.readUInt32LE(2), image.length);
        phase = 'data'; this.reply(0x48);
      } else {
        if (!retried) {
          previous = Buffer.from(bytes);
          retried = true;
          // A corrupt incoming frame triggers RETX, which must not replace
          // the outgoing data saved for the following retransmit request.
          const corrupt = packet(0x48); corrupt[17] ^= 1;
          this.emit('data', corrupt);
          this.reply(0x19);
          return;
        }
        if (previous) { assert.deepEqual(bytes, previous); previous = undefined; }
        const count = Math.min(16, image.length - received);
        assert.equal(bytes[0], count);
        assert.deepEqual(bytes.subarray(1, 1 + count), image.subarray(received, received + count));
        assert.ok(bytes.subarray(1 + count, 17).every(byte => byte === 0xff));
        received += count;
        this.reply(received === image.length ? 0x54 : 0x48);
      }
    }
    drain(callback) { callback(null); }
    flush(callback) { callback?.(null); }
    close(callback) { this.isOpen = false; this.emit('close'); callback(null); }
  }
  const context = {
    exports: {}, Buffer,
    process: { argv: ['node', 'index.js', ...(options.args ?? ['image.bin', 'COM_TEST'])], env: {} },
    console: { log: message => logs.push(message) },
    setTimeout: callback => setTimeout(callback, 0),
    require: name => name === 'serialport' ? { SerialPort: MockPort }
      : name === 'fs/promises' ? { readFile: async () => image } : require(name),
  };
  vm.runInNewContext(source, context);
  await context.completed;
  return { context, port, logs, sent, received };
}

test('transfers all bytes, including the final short packet, and retransmits data', async () => {
  const result = await run();
  assert.equal(result.context.process.exitCode, undefined, result.logs.join('\n'));
  assert.equal(result.received, 449);
  assert.equal(result.port.config.path, 'COM_TEST');
  assert.equal(result.port.isOpen, false);
  assert.ok(result.logs.includes('[$] Firmware update complete!'));
});

test('help and missing arguments do not open a serial port', async () => {
  const help = await run({ args: ['--help'] });
  assert.equal(help.port, undefined);
  assert.equal(help.context.process.exitCode, undefined);
  const missing = await run({ args: [] });
  assert.equal(missing.port, undefined);
  assert.equal(missing.context.process.exitCode, 1);
});

test('rejects undersized firmware before opening a serial port', async () => {
  const result = await run({ size: 10 });
  assert.equal(result.port, undefined);
  assert.equal(result.context.process.exitCode, 1);
  assert.match(result.logs.at(-1), /Invalid firmware size/);
});

test('reports serial open errors', async () => {
  const result = await run({ openError: true });
  assert.equal(result.context.process.exitCode, 1);
  assert.match(result.logs.at(-1), /Cannot open COM_TEST/);
});

test('reports NACK and closes the port', async () => {
  const result = await run({ nack: true });
  assert.equal(result.context.process.exitCode, 1);
  assert.equal(result.port.isOpen, false);
  assert.match(result.logs.at(-1), /NACK/);
});

# Firmware updater

Install dependencies with `npm install`, then run `npm run check` to check TypeScript
or `npm test` to run the simulated serial-port tests.

```powershell
npm start -- path/to/signed-firmware.bin COM3
```

Replace `COM3` with your board's serial port (`/dev/ttyUSB0` on Linux).
The connection uses 115200 baud. You can also set `SERIAL_PORT` and omit the port
argument. Run `npm start -- --help` for usage.

The updater expects an image with device ID metadata at offset `0x1B4` and a
bootloader implementing the sync and firmware-update commands in `index.ts`.
The size check only verifies that the metadata field fits; it does not verify
the image's signature or compatibility. The current sibling bootloader jumps
directly to the application and does not implement this update handshake, and
the sibling application linker script does not define that metadata layout.
Its raw `firmware.bin` is therefore not yet a supported update image.

No hardware flashing is performed by the tests.

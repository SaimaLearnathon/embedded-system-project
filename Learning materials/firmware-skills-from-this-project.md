# How this project prepares you for other firmware projects

Much of what you are learning here transfers directly to other firmware projects. The most useful knowledge is how to manage data, hardware, and program state reliably.

## Transferable skills

Your `comms.c` already introduces several widely used concepts:

| What you are learning | Where you will use it again |
|---|---|
| State machines | Parsing protocols, controlling motors, managing startup and operating modes |
| UART communication | Debug consoles, GPS receivers, modems, and communication between microcontrollers |
| Circular buffers | Handling incoming sensor data and moving data between interrupts and application code |
| Packet formats and serialization | Communication over UART, SPI, radio, and other transports |
| CRC checks | Detecting corruption in messages, firmware images, and stored data |
| ACK and retransmission | Recovering from communication failures |
| Pointers, structures, and fixed-width integers | Almost every embedded C project |
| Boundary checks | Preventing buffer overflow, invalid access, and accidental data loss |

The bootloader also exposes you to memory layout, linker scripts, vector tables, and transferring execution to an application. Those become useful when working on startup code and firmware updates.

## Turn the code into understanding

The main thing that determines how much you gain is whether you can explain and modify the code yourself. Some fixes were written with assistant help, so reading the finished version is only the first step.

A useful exercise is to trace one packet on paper:

1. Record `state`, `data_byte_count`, and the queue indexes before any bytes arrive.
2. Update those values after each received byte.
3. Follow the completed packet through CRC checking, queueing, and acknowledgement.
4. Predict what happens when the CRC is wrong.
5. Predict what happens when the receive queue is full.
6. Compare your predictions with the code.

You do not need to memorize this file. Aim to understand questions such as:

- Why must the receiver remember its state between calls?
- What happens when data arrives faster than the application processes it?
- Which bytes belong in the CRC calculation?
- What should happen when a message is lost?

Being able to answer those questions will help you design your next firmware project, even if it uses a different microcontroller or communication interface.

## Related project files

- [Communication implementation](../bootloader/src/comms.c)
- [Packet structure and communication interface](../bootloader/inc/comms.h)
- [Packet protocol notes](packet-protocol.md)
- [Ring buffer walkthrough](ring-buffer-walkthrough.md)
- [UART notes](uart.md)

# When to use pointers in C

This guide explains values, addresses, and pointers using examples related to this firmware project. The examples are learning snippets; they do not change the firmware.

## 1. Separate the type from the pointer

Make two decisions:

1. What kind of value do I need to store?
2. Does this code need the value itself or access to an existing object through its address?

| Declaration | Meaning |
|---|---|
| `uint8_t value;` | An unsigned 8-bit integer, from 0 to 255 |
| `uint32_t count;` | An unsigned 32-bit integer, from 0 to 4,294,967,295 |
| `uint8_t *ptr;` | A pointer to a `uint8_t` |
| `uint32_t *ptr;` | A pointer to a `uint32_t` |
| `comms_packet_t *packet;` | A pointer to a packet structure |

Use `#include <stdint.h>` for the fixed-width integer types.

Choose an integer type based on the required range and the hardware or function interface. One UART byte fits in `uint8_t`; a counter that must reach 100,000 needs a larger type.

The type before `*` describes the object being pointed to, not the size of the pointer. On this Cortex-M3 target, these object pointers occupy four bytes even when they point to a one-byte value.

## 2. Understand value, address, and pointer

```c
uint8_t value = 42;
uint8_t *ptr = &value;
```

Read the second line as: "Create a pointer to a `uint8_t`, and store the address of `value` in it."

```text
ptr ------------> value
                   42
```

| Expression | Meaning |
|---|---|
| `value` | The number stored in the variable: 42 |
| `&value` | The address where the variable lives |
| `ptr` | The address stored in the pointer |
| `*ptr` | The object at that address |

```c
uint8_t copy = *ptr;  // Read through the pointer: copy becomes 42.
*ptr = 99;           // Write through the pointer: value becomes 99.
```

`*` has two related uses:

```c
uint8_t *ptr = &value;  // In a declaration: declare a pointer.
*ptr = 10;             // In an expression: access the pointed-to object.
```

Accessing an object through a pointer is called **dereferencing**.

## 3. Use a value when the function only needs a number

You do not need a pointer for every function argument.

```c
#include <stdint.h>

uint32_t add_one(uint32_t number)
{
    return number + 1U;
}

void example(void)
{
    uint32_t count = 10;
    uint32_t next = add_one(count);
    // count is still 10. next is 11.
}
```

The function receives a copy of `count`. Returning a value is often the clearest choice when a function produces one result.

Your `uart_write_byte(uint8_t data)` follows this idea: it needs the byte to send, so the caller supplies a value.

## 4. Use a pointer when a function must modify the caller's object

Changing an ordinary parameter does not change the caller's variable:

```c
void change_copy(uint8_t number)
{
    number = 99;  // Changes only the local parameter.
}
```

To modify the original variable, pass its address:

```c
void change_original(uint8_t *number)
{
    *number = 99;
}

void example(void)
{
    uint8_t value = 42;
    change_original(&value);
    // value is now 99.
}
```

The function requires a valid pointer to a writable `uint8_t`.

```text
Caller:    change_original(&value)
                            |
                            v
Function:  number holds value's address
           *number = 99 writes into value
```

C always passes arguments by value. In this example, the copied argument is an address. Both the caller and the function can therefore access the same original object.

## 5. Your selected line: copying a packet through a pointer

The caller creates storage and passes its address:

```c
comms_packet_t received;

if (comms_packets_available()) {
    comms_read(&received);
    // received now contains the oldest queued packet.
}
```

Inside `comms_read()`:

```c
void comms_read(comms_packet_t *packet)
{
    if (packet == NULL || !comms_packets_available()) {
        return;
    }

    *packet = packet_buffer[packet_read_index];
    packet_read_index = (packet_read_index + 1U) & PACKET_BUFFER_MASK;
}
```

Break the selected assignment into three parts:

| Part | Meaning |
|---|---|
| `packet_buffer[packet_read_index]` | The queued packet to copy from |
| `*packet` | The caller's packet structure to copy into |
| `=` | Copy the whole structure from right to left |

```text
packet_buffer[packet_read_index]       received
    length       ------------------>    length
    data[16]     ------------------>    data[16]
    crc          ------------------>    crc
                                          ^
                                          |
                                        packet
```

Because `packet` points to `received`, the assignment fills `received`. Structure assignment includes the embedded `data` array. The result is an independent copy; reusing the queue slot later does not change `received`.

Compare these two assignments:

```c
*packet = packet_buffer[packet_read_index];
// Copy a packet into the caller's object.

packet = &packet_buffer[packet_read_index];
// Redirect only the local pointer. This does not fill the caller's object.
```

The current `comms_read()` leaves the destination unchanged if the queue is empty. Check availability before treating the destination as a newly received packet.

## 6. Use a pointer to give a function access to an array

A function can process a buffer when given its starting address and length:

```c
#include <stddef.h>
#include <stdint.h>

void fill_bytes(uint8_t *buffer, size_t length, uint8_t value)
{
    for (size_t i = 0; i < length; i++) {
        buffer[i] = value;
    }
}

void example(void)
{
    uint8_t data[16];
    fill_bytes(data, sizeof(data), 0xff);
    // All 16 bytes now contain 0xff.
}
```

In the call, the array name `data` converts to a pointer to its first element. Here these calls are equivalent:

```c
fill_bytes(data, 16, 0xff);
fill_bytes(&data[0], 16, 0xff);
```

Inside the function, `buffer[i]` accesses an element in the caller's array. It is equivalent to `*(buffer + i)`.

A pointer does not carry the array length. The caller must provide the correct length and enough writable storage. `sizeof(data)` works for the actual local array; `sizeof(buffer)` inside the function gives the pointer's size, not the original array's size.

Your `uart_write(bytes, PACKET_LENGTH)` uses the same address-and-length pattern to read bytes for transmission.

## 7. Use a const pointer parameter when a function only reads an object

Passing a structure's address allows inspection without passing a copy of the whole structure:

```c
bool packet_has_data(const comms_packet_t *packet)
{
    return packet->length > 0;
}
```

This example requires a valid packet pointer. `const comms_packet_t *` means the function cannot modify the packet through this pointer.

```c
packet->length = 3;  // Not allowed through a pointer to const.
```

Your `comms_is_single_byte_packet()` uses this form because it only checks packet contents.

For structure members:

```c
comms_packet_t received = {0};
comms_packet_t *packet = &received;

received.length = 3;  // Use . with the structure itself.
packet->length = 3;   // Use -> with a pointer to the structure.
(*packet).length = 3; // Same meaning as the previous line.
```

## 8. Use an output pointer when a function returns status separately

A function can return success or failure and write a result through a pointer:

```c
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool calculate_average(uint32_t total, uint32_t count, uint32_t *result)
{
    if (result == NULL || count == 0) {
        return false;
    }

    *result = total / count;
    return true;
}

void example(void)
{
    uint32_t average = 0;
    if (calculate_average(100, 4, &average)) {
        // Success: average is 25.
    }
}
```

This separates "Did the operation succeed?" from "What value did it produce?" The same pattern is useful for reading a sensor or removing an item from a buffer.

## 9. Common pointer mistakes

### Dereferencing an uninitialized pointer

```c
uint8_t *ptr;
*ptr = 5;  // WRONG: ptr has not been given a valid address.
```

Declaring a pointer does not create the object it should point to. Give it a valid target:

```c
uint8_t value = 0;
uint8_t *ptr = &value;
*ptr = 5;
```

### Mixing up an address and a value

```c
uint8_t value = 42;
change_original(value);   // WRONG: the function expects an address.
change_original(&value);  // Correct.
```

### Using a pointer to the wrong type

```c
uint8_t byte = 0;
uint32_t *ptr = &byte;  // WRONG: incompatible pointer types.
```

Do not add a cast just to hide this error. Writing through a `uint32_t *` requires a suitable `uint32_t` object; a single-byte variable does not provide that storage or necessarily the required alignment.

### Using an object after its lifetime ends

```c
uint8_t *bad_function(void)
{
    uint8_t local = 42;
    return &local;  // WRONG: local stops existing when this function returns.
}
```

Return the number itself, or let the caller provide storage through a pointer. Your `comms_read(&received)` follows the caller-provided-storage approach.

### Assuming a non-NULL pointer is always safe

`NULL` means the pointer does not point to an object. Checking for `NULL` catches that case, but it does not prove that every other address is valid. The pointed-to object must still exist, have the correct type, and provide enough accessible storage.

### Confusing two uses of &

```c
&received                   // Address-of: one operand.
(packet_read_index + 1U) & 7U // Bitwise AND: two operands.
```

The second expression wraps an index for the eight-slot queue. It is not a pointer operation.

## 10. Quick decision table

| What you need | Usual approach | Example |
|---|---|---|
| Give a function a small input value | Pass the value | `uart_write_byte(value)` |
| Produce one simple result | Return a value | `next = add_one(count)` |
| Modify an existing caller-owned variable | Pass its address | `change_original(&value)` |
| Fill a caller-owned structure | Pass a structure pointer | `comms_read(&received)` |
| Read a structure without copying it as an argument | Pass a pointer to const | `packet_has_data(&received)` |
| Process an array | Pass its start address and length | `uart_write(bytes, 18)` |
| Return status and a separate result | Return status; write through an output pointer | `calculate_average(100, 4, &average)` |

## 11. Practice: predict the result

```c
uint8_t a = 10;
uint8_t b = 20;
uint8_t *p = &a;

*p = 30;
p = &b;
*p = 40;
```

What are `a` and `b` now?

- `a` is 30: the first write through `p` changed `a`.
- `b` is 40: after `p = &b`, the next write changed `b`.
- Changing `p` changed where it points; it did not copy `b` into `a`.

When reading a pointer expression, ask: **Where does this pointer point, and am I changing the address or the object at that address?**

## Related project files

- [Communication implementation](../bootloader/src/comms.c)
- [Packet structure and communication interface](../bootloader/inc/comms.h)
- [UART interface](../shared/inc/core/uart.h)

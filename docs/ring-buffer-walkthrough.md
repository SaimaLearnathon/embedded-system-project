# Ring Buffer Walkthrough

This note explains how the ring buffer in this project works, line by line, so you can review it later without having to reconstruct the logic from scratch.

Files covered:

- `shared/inc/core/ring-buffer.h`
- `shared/src/core/ring-buffer.c`
- `shared/src/core/uart.c` (only the parts that use the ring buffer)

## What a ring buffer is

A ring buffer stores bytes in a fixed-size array and reuses the array in a circular way.

Instead of moving data around in memory, it keeps two indexes:

- `write_index`: where the next incoming byte should be stored
- `read_index`: where the next outgoing byte should be read from

When an index reaches the end of the buffer, it wraps back to the beginning. In this implementation, that wrapping is done with a bit mask.

## Big idea of this implementation

This ring buffer is designed for byte storage with these rules:

- Data is written into a user-provided array
- The buffer size should be a power of two
- One slot is intentionally left unused so that `read_index == write_index` can mean "empty"
- If the next write would collide with the current read position, the buffer is treated as full and the write fails

That means a buffer of size `64` can hold at most `63` bytes.

## Header file: `shared/inc/core/ring-buffer.h`

### Include guard

```c
#ifndef INC_RING_BUFFER_H
#define INC_RING_BUFFER_H
```

This prevents the header from being included more than once in the same translation unit.

### Shared type definitions

```c
#include  "common-defines.h"
```

This likely provides:

- `uint8_t`
- `uint32_t`
- `bool`

The ring buffer depends on those types.

### Structure definition

```c
typedef struct ring_buffer_t {
    uint8_t* buffer;
    uint32_t mask;
    uint32_t read_index;
    uint32_t write_index;
} ring_buffer_t;
```

Line by line:

```c
typedef struct ring_buffer_t {
```

Defines a struct type and gives it the public name `ring_buffer_t`.

```c
    uint8_t* buffer;
```

Pointer to the raw byte array where data is stored.

```c
    uint32_t mask;
```

Used for wraparound. If size is `64`, then `mask` becomes `63`, which is `0b00111111`.

That allows code like:

```c
(index + 1) & mask
```

to wrap the index back into range, but only when the size is a power of two.

```c
    uint32_t read_index;
```

Position of the next byte to be read.

```c
    uint32_t write_index;
```

Position of the next byte to be written.

```c
} ring_buffer_t;
```

Ends the struct definition.

### Function declarations

```c
void ring_buffer_setup(ring_buffer_t* rb, uint8_t* buffer, uint32_t size);
bool ring_buffer_empty(ring_buffer_t* rb);
bool ring_buffer_write(ring_buffer_t* rb, uint8_t byte);
bool ring_buffer_read(ring_buffer_t* rb, uint8_t* byte);
```

These declare the API:

- `ring_buffer_setup(...)`: initialize the ring buffer state
- `ring_buffer_empty(...)`: check whether there is any unread data
- `ring_buffer_write(...)`: try to add one byte
- `ring_buffer_read(...)`: try to remove one byte

### End of include guard

```c
#endif
```

Closes the include guard started at the top of the file.

## Source file: `shared/src/core/ring-buffer.c`

### Include the header

```c
#include "core/ring-buffer.h"
```

This brings in the type and function declarations from the header.

## `ring_buffer_setup`

Full code:

```c
void ring_buffer_setup(ring_buffer_t* rb , uint8_t* buffer ,uint32_t size){

    rb->buffer = buffer;
    rb->read_index = 0;
    rb->write_index=0;
    rb->mask=size-1;


}
```

Line by line:

```c
void ring_buffer_setup(ring_buffer_t* rb , uint8_t* buffer ,uint32_t size){
```

This function initializes a `ring_buffer_t`.

Parameters:

- `rb`: the ring buffer object to initialize
- `buffer`: the backing byte array that will hold the data
- `size`: the total size of that array

```c
    rb->buffer = buffer;
```

Store the caller's array pointer inside the ring buffer.

```c
    rb->read_index = 0;
```

Start reading at position `0`.

```c
    rb->write_index=0;
```

Start writing at position `0`.

At this point:

- the buffer is empty
- `read_index == write_index`

```c
    rb->mask=size-1;
```

Prepare the wraparound mask.

Example:

- if `size = 64`
- then `mask = 63`

That makes this possible later:

```c
(index + 1) & 63
```

which always stays in the range `0..63`.

Important assumption:

- `size` must be a power of two for this to work correctly

If `size` is not a power of two, the wraparound math will produce incorrect indexes.

```c
}
```

End of setup function.

## `ring_buffer_empty`

Full code:

```c
bool ring_buffer_empty( ring_buffer_t* rb){

    return rb->read_index==rb->write_index;

}
```

Line by line:

```c
bool ring_buffer_empty( ring_buffer_t* rb){
```

Function that reports whether the buffer currently contains any unread data.

```c
    return rb->read_index==rb->write_index;
```

If both indexes are equal, then there is nothing to read.

Why this works:

- `write_index` moves forward when data is written
- `read_index` moves forward when data is read
- if they meet, all written data has been consumed

```c
}
```

End of empty-check function.

## `ring_buffer_read`

Full code:

```c
bool ring_buffer_read(ring_buffer_t* rb , uint8_t* byte){

    uint32_t local_read_index=rb->read_index;
    uint32_t local_write_index=rb->write_index;

    if(local_read_index==local_write_index) return false;

    *byte = rb->buffer[local_read_index];

    local_read_index = (local_read_index+1) & rb->mask;

    rb->read_index = local_read_index;
    return true;



}
```

Line by line:

```c
bool ring_buffer_read(ring_buffer_t* rb , uint8_t* byte){
```

Try to read one byte from the buffer.

Parameters:

- `rb`: the buffer state
- `byte`: output pointer where the read byte will be stored

Return value:

- `true` if a byte was read
- `false` if the buffer was empty

```c
    uint32_t local_read_index=rb->read_index;
```

Copy the current read position into a local variable.

```c
    uint32_t local_write_index=rb->write_index;
```

Copy the current write position into a local variable.

This local-copy style is common in embedded code because it:

- makes the logic easier to reason about
- avoids repeatedly dereferencing the struct fields

```c
    if(local_read_index==local_write_index) return false;
```

If both indexes are equal, the buffer is empty, so there is nothing to read.

The function exits immediately with `false`.

```c
    *byte = rb->buffer[local_read_index];
```

Read the byte at the current read position and store it in the caller's output variable.

```c
    local_read_index = (local_read_index+1) & rb->mask;
```

Advance the read position by one, wrapping if needed.

Example with size `64` and mask `63`:

- if `local_read_index = 10`, next becomes `11`
- if `local_read_index = 63`, next becomes `0`

```c
    rb->read_index = local_read_index;
```

Commit the updated read position back into the struct.

This officially marks that byte as consumed.

```c
    return true;
```

Report success to the caller.

```c
}
```

End of read function.

## `ring_buffer_write`

Full code:

```c
bool ring_buffer_write(ring_buffer_t* rb , uint8_t byte){
    uint32_t local_write_index=rb->write_index;
    uint32_t local_read_index=rb->read_index;

    uint32_t next_write_index=(local_write_index+1)&rb->mask;

    if(next_write_index==local_read_index) return false;
    rb->buffer[local_write_index] = byte;
    rb->write_index = next_write_index;
    return true ;
    
}
```

Line by line:

```c
bool ring_buffer_write(ring_buffer_t* rb , uint8_t byte){
```

Try to add one byte into the buffer.

Parameters:

- `rb`: the buffer state
- `byte`: the new byte to store

Return value:

- `true` if the byte was written
- `false` if the buffer was full

```c
    uint32_t local_write_index=rb->write_index;
```

Copy the current write position into a local variable.

```c
    uint32_t local_read_index=rb->read_index;
```

Copy the current read position into a local variable.

```c
    uint32_t next_write_index=(local_write_index+1)&rb->mask;
```

Compute where the write index would move after storing the new byte.

This is important because the code checks for fullness before actually writing.

```c
    if(next_write_index==local_read_index) return false;
```

If the next write position would equal the current read position, the buffer is treated as full.

Why?

Because this implementation uses:

- `read_index == write_index` to mean empty

If it also allowed the write path to make them equal when full, then empty and full would look identical. To avoid that ambiguity, one slot is always left unused.

```c
    rb->buffer[local_write_index] = byte;
```

Store the new byte into the current write location.

```c
    rb->write_index = next_write_index;
```

Advance the write position.

Now the byte becomes visible to readers.

```c
    return true ;
```

Report success.

```c
}
```

End of write function.

## Visual example

Assume:

- size = `8`
- mask = `7`
- indexes start at `0`

Initial state:

- `read_index = 0`
- `write_index = 0`
- buffer is empty

Write `A`:

- store `A` at index `0`
- move `write_index` to `1`

Write `B`:

- store `B` at index `1`
- move `write_index` to `2`

Read once:

- read from index `0` -> `A`
- move `read_index` to `1`

Read again:

- read from index `1` -> `B`
- move `read_index` to `2`

Now:

- `read_index == write_index == 2`
- buffer is empty again

Wraparound example:

- if `write_index = 7`
- next write index becomes `(7 + 1) & 7 = 0`

So the buffer naturally loops back to the beginning.

## How it is used in `uart.c`

The ring buffer is not standalone in practice. In this project it is used to store bytes received by UART interrupt code.

Relevant lines:

```c
#define RING_BUFFER_SIZE (64)
static volatile uint8_t data_buffer[RING_BUFFER_SIZE] = {0U};
static ring_buffer_t rb = {0U};
```

Meaning:

- a 64-byte storage array is created
- one ring buffer object `rb` is created
- `rb` will manage access to `data_buffer`

Setup:

```c
ring_buffer_setup(&rb,data_buffer,RING_BUFFER_SIZE);
```

This connects the ring buffer object to the real array.

When a UART byte arrives in the interrupt:

```c
if(!ring_buffer_write(&rb , (uint8_t) usart_recv(USART1))){
    //handle failure 
}
```

Meaning:

- read one byte from the UART peripheral
- try to push it into the ring buffer
- if the buffer is full, the write fails

When application code wants to read bytes:

```c
if(!ring_buffer_read(&rb, &data[i])){
    return i;
}
```

Meaning:

- try to remove one byte from the ring buffer
- if the buffer is empty, stop and return how many bytes were actually read

Availability check:

```c
return !ring_buffer_empty(&rb);
```

Meaning:

- if the buffer is not empty, UART data is available to consume

## Important design notes

### 1. Buffer size must be a power of two

Because wraparound uses:

```c
(index + 1) & mask
```

the `size` must be something like:

- `2`
- `4`
- `8`
- `16`
- `32`
- `64`

Using a non-power-of-two size will break the indexing.

### 2. Actual usable capacity is `size - 1`

One slot is sacrificed to distinguish:

- empty
- full

So a `64`-byte ring buffer can store at most `63` unread bytes.

### 3. The code is simple and efficient

This implementation is useful in embedded systems because:

- no dynamic allocation is used
- read/write operations are constant time
- the wraparound logic is cheap

### 4. Concurrent access needs care

In this project, the buffer is written from the UART interrupt and read from normal code. That is a common pattern, but it also means correctness depends on how the compiler and target handle shared state.

Points worth reviewing later:

- whether `read_index` and `write_index` should be `volatile`
- whether interrupt-safe guarantees are sufficient for your MCU and compiler
- whether full-buffer failure should increment a dropped-byte counter

## Short summary

This ring buffer works by:

1. storing bytes in a fixed array
2. tracking the next read and write positions
3. wrapping those positions with a bit mask
4. refusing writes when the buffer is full
5. refusing reads when the buffer is empty

For your UART path, it acts as a small queue between:

- the interrupt that receives bytes quickly
- the main code that consumes bytes later

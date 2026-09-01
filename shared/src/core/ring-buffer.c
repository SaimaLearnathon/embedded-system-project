#include "core/ring-buffer.h"
#include <stddef.h>

bool ring_buffer_setup(ring_buffer_t* rb, volatile uint8_t* buffer, uint32_t size)
{
    if ((rb == NULL) || (buffer == NULL) || (size < 2U) ||
        ((size & (size - 1U)) != 0U)) {
        return false;
    }

    rb->buffer = buffer;
    rb->read_index = 0U;
    rb->write_index = 0U;
    rb->mask = size - 1U;
    return true;
}

bool ring_buffer_empty(ring_buffer_t* rb)
{
    return (rb == NULL) || (rb->read_index == rb->write_index);
}

bool ring_buffer_read(ring_buffer_t* rb, uint8_t* byte)
{
    if ((rb == NULL) || (rb->buffer == NULL) || (byte == NULL)) {
        return false;
    }

    uint32_t local_read_index=rb->read_index;
    uint32_t local_write_index=rb->write_index;

    if(local_read_index==local_write_index) return false;

    *byte = rb->buffer[local_read_index];

    local_read_index = (local_read_index+1) & rb->mask;

    rb->read_index = local_read_index;
    return true;



}

bool ring_buffer_write(ring_buffer_t* rb, uint8_t byte)
{
    if ((rb == NULL) || (rb->buffer == NULL)) {
        return false;
    }
    uint32_t local_write_index=rb->write_index;
    uint32_t local_read_index=rb->read_index;

    uint32_t next_write_index=(local_write_index+1)&rb->mask;

    if(next_write_index==local_read_index) return false;
    rb->buffer[local_write_index] = byte;
    rb->write_index = next_write_index;
    return true ;
    
}

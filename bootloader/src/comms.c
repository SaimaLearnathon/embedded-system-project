#include "comms.h"
#include "core/uart.h"
#include "core/crc8.h"
#include <stddef.h>

#define PACKET_BUFFER_LENGTH (8U)
#define PACKET_BUFFER_MASK (PACKET_BUFFER_LENGTH - 1U)
#define PACKET_CRC_LENGTH (1U + PACKET_DATA_LENGTH)
#define PACKET_LENGTH (PACKET_CRC_LENGTH + 1U)

typedef enum comms_state_t {
    CommsState_length,
    CommsState_Data,
    CommsState_CRC
} comms_state_t;

static comms_state_t state = CommsState_length;
static uint8_t data_byte_count = 0;
static comms_packet_t temporary_packet = {0};
static comms_packet_t retx_packet = {0};
static comms_packet_t ack_packet = {0};
static comms_packet_t last_transmitted_packet = {0};
static bool last_transmitted_packet_valid = false;

/* Leave one slot empty to distinguish a full queue from an empty one. */
static comms_packet_t packet_buffer[PACKET_BUFFER_LENGTH];
static uint32_t packet_read_index = 0;
static uint32_t packet_write_index = 0;

static bool comms_is_single_byte_packet(const comms_packet_t *packet, uint8_t byte)
{
    if (packet->length != 1 || packet->data[0] != byte) {
        return false;
    }

    for (uint8_t i = 1; i < PACKET_DATA_LENGTH; i++) {
        if (packet->data[i] != 0xff) {
            return false;
        }
    }
    return true;
}

void comms_setup(void)
{
    state = CommsState_length;
    data_byte_count = 0;
    packet_read_index = 0;
    packet_write_index = 0;
    last_transmitted_packet_valid = false;

    retx_packet.length = 1;
    retx_packet.data[0] = PACKET_RETx_DATA0;
    ack_packet.length = 1;
    ack_packet.data[0] = PACKET_ACK_DATA0;

    for (uint8_t i = 1; i < PACKET_DATA_LENGTH; i++) {
        retx_packet.data[i] = 0xff;
        ack_packet.data[i] = 0xff;
    }

    retx_packet.crc = comms_compute_crc(&retx_packet);
    ack_packet.crc = comms_compute_crc(&ack_packet);
}

void comms_update(void)
{
    while (uart_data_available()) {
        switch (state) {
        case CommsState_length:
            temporary_packet.length = uart_read_byte();
            data_byte_count = 0;
            state = CommsState_Data;
            break;

        case CommsState_Data:
            /* Every wire packet contains all 16 data bytes, including padding. */
            temporary_packet.data[data_byte_count++] = uart_read_byte();
            if (data_byte_count == PACKET_DATA_LENGTH) {
                state = CommsState_CRC;
            }
            break;

        case CommsState_CRC: {
            temporary_packet.crc = uart_read_byte();
            state = CommsState_length;

            if (temporary_packet.length > PACKET_DATA_LENGTH ||
                temporary_packet.crc != comms_compute_crc(&temporary_packet)) {
                comms_write(&retx_packet);
                break;
            }

            if (comms_is_single_byte_packet(&temporary_packet, PACKET_RETx_DATA0)) {
                if (last_transmitted_packet_valid) {
                    comms_write(&last_transmitted_packet);
                }
                break;
            }

            /* An ACK confirms receipt; it does not need a reply. */
            if (comms_is_single_byte_packet(&temporary_packet, PACKET_ACK_DATA0)) {
                break;
            }

            uint32_t next_write_index = (packet_write_index + 1U) & PACKET_BUFFER_MASK;
            if (next_write_index == packet_read_index) {
                /* Do not overwrite unread data or acknowledge a dropped packet. */
                comms_write(&retx_packet);
                break;
            }

            packet_buffer[packet_write_index] = temporary_packet;
            packet_write_index = next_write_index;
            comms_write(&ack_packet);
            break;
        }

        default:
            state = CommsState_length;
            data_byte_count = 0;
            break;
        }
    }
}

bool comms_packets_available(void)
{
    return packet_read_index != packet_write_index;
}

void comms_write(comms_packet_t *packet)
{
    if (packet == NULL || packet->length > PACKET_DATA_LENGTH) {
        return;
    }

    packet->crc = comms_compute_crc(packet);

    /* Control replies must not replace the data packet saved for retransmission. */
    if (!comms_is_single_byte_packet(packet, PACKET_RETx_DATA0) &&
        !comms_is_single_byte_packet(packet, PACKET_ACK_DATA0)) {
        last_transmitted_packet = *packet;
        last_transmitted_packet_valid = true;
    }

    /* Serialize explicitly so the wire format does not depend on struct padding. */
    uint8_t bytes[PACKET_LENGTH];
    bytes[0] = packet->length;
    for (uint8_t i = 0; i < PACKET_DATA_LENGTH; i++) {
        bytes[i + 1U] = packet->data[i];
    }
    bytes[PACKET_CRC_LENGTH] = packet->crc;
    uart_write(bytes, PACKET_LENGTH);
}

void comms_read(comms_packet_t *packet)
{
    if (packet == NULL || !comms_packets_available()) {
        return;
    }

    *packet = packet_buffer[packet_read_index];
    packet_read_index = (packet_read_index + 1U) & PACKET_BUFFER_MASK;
}

uint8_t comms_compute_crc(comms_packet_t *packet)
{
    if (packet == NULL) {
        return 0;
    }

    /* The CRC covers the length byte and all data bytes, but not the CRC itself. */
    uint8_t bytes[PACKET_CRC_LENGTH];
    bytes[0] = packet->length;
    for (uint8_t i = 0; i < PACKET_DATA_LENGTH; i++) {
        bytes[i + 1U] = packet->data[i];
    }
    return crc8(bytes, PACKET_CRC_LENGTH);
}

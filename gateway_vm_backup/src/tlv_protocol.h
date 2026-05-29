/*
 * tlv_protocol.h -- TLV (Tag-Length-Value) application protocol
 *
 * Frame format (big-endian):
 *   [Tag:1B][Length:2B][Value:Length B][CRC8:1B]
 *
 * Tags:
 *   0x01  HEARTBEAT_REQ
 *   0x02  HEARTBEAT_RSP
 *   0x10  MES_EVENT
 *   0x11  MES_ACK
 *   0x20  FRAME_DATA       video frame chunk
 *   0x21  FRAME_ACK
 *   0x22  FRAME_START      frame begin (value=4B BE total len)
 *   0x30  AI_RESULT        AI detection result (PC->Gateway, JSON)
 *   0x31  AI_READY         AI ready notification (PC->Gateway)
 *   0x32  AI_CMD           AI control command (Gateway->PC)
 */

#ifndef TLV_PROTOCOL_H
#define TLV_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Tag definitions --- */
#define TLV_HEARTBEAT_REQ  0x01
#define TLV_HEARTBEAT_RSP  0x02
#define TLV_MES_EVENT      0x10
#define TLV_MES_ACK        0x11
#define TLV_FRAME_DATA     0x20
#define TLV_FRAME_ACK      0x21
#define TLV_FRAME_START    0x22
#define TLV_AI_RESULT      0x30
#define TLV_AI_READY       0x31
#define TLV_AI_CMD         0x32

/* Frame constants */
#define TLV_HEADER_SIZE    3
#define TLV_CRC_SIZE       1
#define TLV_MAX_VALUE      65535
#define TLV_FRAME_OVERHEAD (TLV_HEADER_SIZE + TLV_CRC_SIZE)

/* --- Packet --- */
typedef struct {
    uint8_t  tag;
    uint16_t len;
    uint8_t *value;
} tlv_packet_t;

/* --- Stream parser state machine --- */
typedef enum {
    TLV_STATE_HEADER,
    TLV_STATE_BODY,
    TLV_STATE_CRC
} tlv_state_t;

typedef struct {
    tlv_state_t state;
    uint8_t     header[TLV_HEADER_SIZE];
    uint8_t     header_pos;
    uint8_t     tag;
    uint16_t    len;
    uint16_t    body_pos;
} tlv_reader_t;

/* --- API --- */
void tlv_reader_init(tlv_reader_t *r);
int tlv_reader_feed(tlv_reader_t *r, const uint8_t *data, size_t data_len,
                    uint8_t *tag, uint8_t *buf, size_t buf_size);
size_t tlv_pack(uint8_t tag, const uint8_t *value, uint16_t len,
                uint8_t *frame, size_t frame_size);
uint8_t tlv_checksum(uint8_t tag, const uint8_t *value, uint16_t len);

static inline uint16_t tlv_frame_total(uint8_t tag_byte, uint16_t len_field) {
    (void)tag_byte;
    return TLV_FRAME_OVERHEAD + len_field;
}

#ifdef __cplusplus
}
#endif

#endif
/*
 * tlv_protocol.c — TLV 协议编解码实现
 */

#include "tlv_protocol.h"
#include <string.h>

uint8_t tlv_checksum(uint8_t tag, const uint8_t *value, uint16_t len)
{
    uint8_t crc = tag ^ (uint8_t)(len >> 8) ^ (uint8_t)(len & 0xFF);
    for (uint16_t i = 0; i < len; i++)
        crc ^= value[i];
    return crc;
}

size_t tlv_pack(uint8_t tag, const uint8_t *value, uint16_t len,
                uint8_t *frame, size_t frame_size)
{
    size_t total = TLV_FRAME_OVERHEAD + len;
    if (total > frame_size) return 0;

    frame[0] = tag;
    frame[1] = (uint8_t)(len >> 8);
    frame[2] = (uint8_t)(len & 0xFF);
    if (len > 0 && value)
        memcpy(frame + TLV_HEADER_SIZE, value, len);
    frame[TLV_HEADER_SIZE + len] = tlv_checksum(tag, value, len);
    return total;
}

void tlv_reader_init(tlv_reader_t *r)
{
    r->state      = TLV_STATE_HEADER;
    r->header_pos = 0;
    r->body_pos   = 0;
    memset(r->header, 0, sizeof(r->header));
}

int tlv_reader_feed(tlv_reader_t *r, const uint8_t *data, size_t data_len,
                    uint8_t *tag, uint8_t *buf, size_t buf_size)
{
    size_t consumed = 0;

    while (consumed < data_len) {
        uint8_t byte = data[consumed++];

        switch (r->state) {
        case TLV_STATE_HEADER:
            r->header[r->header_pos++] = byte;
            if (r->header_pos == TLV_HEADER_SIZE) {
                r->tag = r->header[0];
                r->len = ((uint16_t)r->header[1] << 8) | r->header[2];
                if (r->len > TLV_MAX_VALUE) {
                    tlv_reader_init(r);
                    continue;
                }
                r->body_pos = 0;
                r->state = (r->len == 0) ? TLV_STATE_CRC : TLV_STATE_BODY;
            }
            break;

        case TLV_STATE_BODY:
            if (r->body_pos < buf_size)
                buf[r->body_pos] = byte;
            r->body_pos++;
            if (r->body_pos >= r->len)
                r->state = TLV_STATE_CRC;
            break;

        case TLV_STATE_CRC:
            {
                uint8_t expected = tlv_checksum(r->tag, buf,
                    r->body_pos < buf_size ? r->body_pos : buf_size);
                *tag = r->tag;
                tlv_reader_init(r);
                if (byte == expected)
                    return (int)r->len;
                else
                    return -1;
            }
        }
    }
    return 0;
}
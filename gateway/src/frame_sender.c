/*
 * frame_sender.c — 视频帧分包 TLV 发送
 *
 * 大幅帧 (>65535B) 策略:
 *   1. 先发 FRAME_START (0x22)，value=4B 大端总长度
 *   2. 再分多个 FRAME_DATA (0x20)，每包最多 32KB
 */

#include "frame_sender.h"
#include "tlv_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define CHUNK_SIZE  32768  /* 每包最大 32KB */

int fs_send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                usleep(1000); continue;
            }
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

int fs_send_frame(int sock_fd, const uint8_t *yuyv_data, size_t length)
{
    /* 1. 发送 FRAME_START: 4 字节大端总长度 */
    uint8_t start_val[4] = {
        (uint8_t)(length >> 24), (uint8_t)(length >> 16),
        (uint8_t)(length >> 8),  (uint8_t)(length)
    };
    uint8_t hdr[TLV_FRAME_OVERHEAD + 4];
    size_t hdr_sz = tlv_pack(TLV_FRAME_START, start_val, 4, hdr, sizeof(hdr));
    if (hdr_sz == 0 || fs_send_all(sock_fd, hdr, hdr_sz) < 0)
        return -1;

    /* 2. 分包发送 FRAME_DATA */
    size_t offset = 0;
    while (offset < length) {
        size_t chunk = length - offset;
        if (chunk > CHUNK_SIZE) chunk = CHUNK_SIZE;

        size_t frame_sz = TLV_FRAME_OVERHEAD + chunk;
        uint8_t *frame = (uint8_t *)malloc(frame_sz);
        if (!frame) return -1;

        size_t packed = tlv_pack(TLV_FRAME_DATA, yuyv_data + offset,
                                 (uint16_t)chunk, frame, frame_sz);
        int ret = (packed > 0) ? fs_send_all(sock_fd, frame, packed) : -1;
        free(frame);
        if (ret < 0) return -1;

        offset += chunk;
    }
    return 0;
}

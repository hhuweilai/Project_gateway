/*
 * frame_sender.h — 视频帧 TLV 封装与 TCP 发送
 */

#ifndef FRAME_SENDER_H
#define FRAME_SENDER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 将一帧 YUYV 数据打包为 TLV FRAME_DATA 帧并发送 */
int fs_send_frame(int sock_fd, const uint8_t *yuyv_data, size_t length);

/* 发送一帧的所有字节（非阻塞可能不完整，内部处理） */
int fs_send_all(int fd, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* FRAME_SENDER_H */

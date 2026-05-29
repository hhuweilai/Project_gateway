/*
 * video_capture.h — V4L2 视频采集接口
 *
 * 针对 i.MX6ULL + OV5640（CSI 接口，/dev/video1）。
 * 使用 mmap 零拷贝，流式 DQBUF/QBUF 循环采集。
 */

#ifndef VIDEO_CAPTURE_H
#define VIDEO_CAPTURE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VCAP_WIDTH   640
#define VCAP_HEIGHT  480
#define VCAP_PIXFMT  V4L2_PIX_FMT_YUYV   /* YUYV 4:2:2，每像素 2 字节 */
#define VCAP_FRAME_SIZE  (VCAP_WIDTH * VCAP_HEIGHT * 2)  /* 614400 字节 */
#define VCAP_BUF_COUNT   4  /* mmap 缓冲区数量 */

typedef struct vcap_ctx vcap_ctx_t;

/* 打开设备并初始化 mmap 采集 */
vcap_ctx_t *vcap_open(const char *device);

/* 启动流式采集 */
int vcap_start(vcap_ctx_t *ctx);

/* 获取一帧（阻塞直到有帧）。data 至少 VCAP_FRAME_SIZE 字节。返回实际帧大小 */
int vcap_grab(vcap_ctx_t *ctx, uint8_t *data, size_t max_size);

/* 停止采集并释放资源 */
void vcap_close(vcap_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_CAPTURE_H */

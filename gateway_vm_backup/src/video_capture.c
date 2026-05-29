/*
 * video_capture.c — V4L2 视频采集实现 (mmap 零拷贝)
 *
 * 工作流程:
 *   1. open /dev/video1
 *   2. VIDIOC_S_FMT  设置 640x480 YUYV
 *   3. VIDIOC_REQBUFS 请求 4 个 mmap 缓冲区
 *   4. mmap + VIDIOC_QBUF 全部入队
 *   5. VIDIOC_STREAMON
 *   6. 循环: DQBUF → 拷贝数据 → QBUF
 */

#include "video_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

struct vcap_ctx {
    int     fd;
    int     streaming;
    void   *buf_mmap[VCAP_BUF_COUNT];
    size_t  buf_len[VCAP_BUF_COUNT];
};

vcap_ctx_t *vcap_open(const char *device)
{
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;

    vcap_ctx_t *ctx = (vcap_ctx_t *)calloc(1, sizeof(vcap_ctx_t));
    if (!ctx) return NULL;

    /* 1. 打开设备 */
    ctx->fd = open(device, O_RDWR);
    if (ctx->fd < 0) {
        perror("vcap: open");
        free(ctx); return NULL;
    }

    /* 2. 查询能力 */
    if (ioctl(ctx->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("vcap: VIDIOC_QUERYCAP");
        goto fail;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "vcap: not a capture device\n");
        goto fail;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "vcap: does not support streaming\n");
        goto fail;
    }
    printf("[VCAP] %s: %s (%s)\n", device, cap.card, cap.driver);

    /* 3. 设置格式 */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = VCAP_WIDTH;
    fmt.fmt.pix.height      = VCAP_HEIGHT;
    fmt.fmt.pix.pixelformat = VCAP_PIXFMT;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    if (ioctl(ctx->fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("vcap: VIDIOC_S_FMT");
        goto fail;
    }
    printf("[VCAP] format: %ux%u %c%c%c%c\n",
           fmt.fmt.pix.width, fmt.fmt.pix.height,
           (int)(fmt.fmt.pix.pixelformat & 0xFF),
           (int)((fmt.fmt.pix.pixelformat >> 8) & 0xFF),
           (int)((fmt.fmt.pix.pixelformat >> 16) & 0xFF),
           (int)((fmt.fmt.pix.pixelformat >> 24) & 0xFF));

    /* 4. 请求缓冲区 */
    memset(&req, 0, sizeof(req));
    req.count  = VCAP_BUF_COUNT;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("vcap: VIDIOC_REQBUFS");
        goto fail;
    }
    printf("[VCAP] buffers requested: %u\n", req.count);

    /* 5. mmap 并全部入队 */
    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("vcap: VIDIOC_QUERYBUF");
            goto fail;
        }
        ctx->buf_mmap[i] = mmap(NULL, buf.length,
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED, ctx->fd, buf.m.offset);
        if (ctx->buf_mmap[i] == MAP_FAILED) {
            perror("vcap: mmap");
            goto fail;
        }
        ctx->buf_len[i] = buf.length;

        /* 入队 */
        if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("vcap: VIDIOC_QBUF");
            goto fail;
        }
    }

    return ctx;

fail:
    close(ctx->fd);
    free(ctx);
    return NULL;
}

int vcap_start(vcap_ctx_t *ctx)
{
    if (!ctx || ctx->streaming) return -1;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("vcap: VIDIOC_STREAMON");
        return -1;
    }
    ctx->streaming = 1;
    printf("[VCAP] streaming started\n");
    return 0;
}

int vcap_grab(vcap_ctx_t *ctx, uint8_t *data, size_t max_size)
{
    if (!ctx || !ctx->streaming) return -1;

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    /* 出队（阻塞等待） */
    if (ioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EINTR) return 0;  /* 被信号中断，不算错误 */
        perror("vcap: VIDIOC_DQBUF");
        return -1;
    }

    /* 拷贝数据 */
    size_t copy = (buf.bytesused < max_size) ? buf.bytesused : max_size;
    memcpy(data, ctx->buf_mmap[buf.index], copy);

    /* 重新入队 */
    if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
        perror("vcap: VIDIOC_QBUF");
        return -1;
    }

    return (int)copy;
}

void vcap_close(vcap_ctx_t *ctx)
{
    if (!ctx) return;

    if (ctx->streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
        ctx->streaming = 0;
    }

    for (int i = 0; i < VCAP_BUF_COUNT; i++) {
        if (ctx->buf_mmap[i] && ctx->buf_mmap[i] != MAP_FAILED)
            munmap(ctx->buf_mmap[i], ctx->buf_len[i]);
    }

    close(ctx->fd);
    free(ctx);
    printf("[VCAP] closed\n");
}

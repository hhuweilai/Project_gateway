/*
 * camera_test.c -- capture one frame from OV5640 via V4L2
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define WIDTH   640
#define HEIGHT  480
#define DEVICE  "/dev/video1"
#define BUF_COUNT 4

struct buffer_info {
    void   *start;
    size_t  length;
};

int main(void)
{
    int fd;
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    struct buffer_info buffers[BUF_COUNT];

    printf("=== OV5640 Camera Test ===\n");

    fd = open(DEVICE, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    ioctl(fd, VIDIOC_QUERYCAP, &cap);
    printf("Driver : %s\n", cap.driver);
    printf("Card   : %s\n", cap.card);

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = WIDTH;
    fmt.fmt.pix.height = HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT"); goto err;
    }
    printf("Format : %dx%d YUYV\n", fmt.fmt.pix.width, fmt.fmt.pix.height);

    memset(&req, 0, sizeof(req));
    req.count = BUF_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS"); goto err;
    }
    printf("Buffers requested: %u\n", req.count);

    for (int i = 0; i < (int)req.count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF"); goto err;
        }
        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED) {
            perror("mmap"); goto err;
        }
    }

    for (int i = 0; i < (int)req.count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF"); goto err;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON"); goto err;
    }

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        perror("VIDIOC_DQBUF"); goto err;
    }

    FILE *fp = fopen("/tmp/frame.yuv", "wb");
    fwrite(buffers[buf.index].start, buf.bytesused, 1, fp);
    fclose(fp);
    printf("Frame saved: /tmp/frame.yuv (%u bytes)\n", buf.bytesused);

    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < (int)req.count; i++)
        munmap(buffers[i].start, buffers[i].length);

err:
    close(fd);
    printf("Done!\n");
    return 0;
}
/*
 * frame_receiver.c — PC 端视频帧接收器（支持分片重组）
 *
 * 协议:
 *   FRAME_START (0x22) → 读取4B总长度，开始累积
 *   FRAME_DATA  (0x20) → 追加到累积缓冲
 *   累积满 → 保存为 YUYV 文件
 */

#include "tlv_protocol.h"
#include "mes_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define DEFAULT_PORT     8899
#define MAX_EVENTS       64
#define MAX_CLIENTS      256
#define BUF_SIZE         65536
#define SAVE_DIR         "./frames"
#define MAX_FRAME        (640*480*2)  /* YUYV 一帧 */

static volatile int g_running = 1;
static uint32_t g_frame_count = 0;
static const char *g_save_dir = SAVE_DIR;

/* ——— 帧重组状态 ——— */
typedef struct {
    int     active;        /* 是否正在接收 */
    size_t  total;         /* FRAME_START 声明的总长 */
    size_t  received;      /* 已收字节 */
    uint8_t data[MAX_FRAME];
} frame_assemble_t;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

typedef struct {
    int   fd;
    tlv_reader_t reader;
    frame_assemble_t fa;
} client_t;

static client_t g_clients[MAX_CLIENTS];

static void save_frame(const uint8_t *data, size_t len) {
    struct stat st;
    if (stat(g_save_dir, &st) == -1) mkdir(g_save_dir, 0755);

    char path[256];
    snprintf(path, sizeof(path), "%s/frame_%05u.yuv", g_save_dir, g_frame_count);
    FILE *fp = fopen(path, "wb");
    if (fp) {
        fwrite(data, 1, len, fp);
        fclose(fp);
        g_frame_count++;
        if (g_frame_count % 30 == 0)
            printf("[RECV] %u frames | latest: %s (%zu bytes)\n",
                   g_frame_count, path, len);
    }
}

static int init_listen(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen"); close(fd); return -1;
    }
    set_nonblock(fd);
    return fd;
}

static void do_accept(int epfd, int listen_fd) {
    while (1) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int cfd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("accept"); break;
        }
        if (cfd >= MAX_CLIENTS) { close(cfd); continue; }
        set_nonblock(cfd);
        g_clients[cfd].fd = cfd;
        tlv_reader_init(&g_clients[cfd].reader);
        memset(&g_clients[cfd].fa, 0, sizeof(frame_assemble_t));

        struct epoll_event ev = {0};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = cfd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
        printf("[RECV] client connected: %s:%d (fd=%d)\n",
               inet_ntoa(peer.sin_addr), ntohs(peer.sin_port), cfd);
    }
}

static void do_read(int epfd, int fd) {
    client_t *c = &g_clients[fd];
    uint8_t buf[BUF_SIZE];
    uint8_t val_buf[65536];

    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                uint8_t tag = 0;
                int ret = tlv_reader_feed(&c->reader, &buf[i], 1,
                                          &tag, val_buf, 65536);
                if (ret > 0) {
                    switch (tag) {
                    case TLV_FRAME_START:
                        /* 开始新帧 */
                        if (ret >= 4) {
                            c->fa.total = ((size_t)val_buf[0] << 24) |
                                          ((size_t)val_buf[1] << 16) |
                                          ((size_t)val_buf[2] << 8)  |
                                           (size_t)val_buf[3];
                            c->fa.received = 0;
                            c->fa.active = 1;
                        }
                        break;

                    case TLV_FRAME_DATA:
                        if (c->fa.active) {
                            size_t space = c->fa.total - c->fa.received;
                            size_t copy = (size_t)ret;
                            if (copy > space) copy = space;
                            if (c->fa.received + copy <= MAX_FRAME) {
                                memcpy(c->fa.data + c->fa.received, val_buf, copy);
                                c->fa.received += copy;
                            }
                            if (c->fa.received >= c->fa.total) {
                                save_frame(c->fa.data, c->fa.total);
                                c->fa.active = 0;
                            }
                        }
                        break;

                    case TLV_HEARTBEAT_REQ: {
                        uint8_t resp[TLV_FRAME_OVERHEAD + 8];
                        uint8_t v[] = "{\"pong\":1}";
                        size_t sz = tlv_pack(TLV_HEARTBEAT_RSP, v, sizeof(v)-1,
                                             resp, sizeof(resp));
                        if (sz > 0) write(fd, resp, sz);
                        break;
                    }

                    case TLV_MES_EVENT:
                        mes_print_event(tag, val_buf, (uint16_t)ret);
                        break;

                    default:
                        mes_print_event(tag, val_buf, (uint16_t)ret);
                        break;
                    }
                }
            }
        } else if (n == 0) {
            printf("[RECV] client disconnected (fd=%d)\n", fd);
            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            close(fd); c->fd = -1;
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("read");
            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            close(fd); c->fd = -1;
            return;
        }
    }
}

int main(int argc, char *argv[]) {
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    g_save_dir = (argc > 2) ? argv[2] : SAVE_DIR;

    signal(SIGINT, sig_handler); signal(SIGTERM, sig_handler);

    int listen_fd = init_listen(port);
    if (listen_fd < 0) return EXIT_FAILURE;

    int epfd = epoll_create1(0);
    struct epoll_event ev = {0};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    for (int i = 0; i < MAX_CLIENTS; i++) g_clients[i].fd = -1;

    printf("=== Frame Receiver (chunked) ===\n");
    printf("Listening on port %d | Save dir: %s\n", port, g_save_dir);
    printf("Press Ctrl+C to stop\n\n");

    struct epoll_event events[MAX_EVENTS];
    while (g_running) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait"); break;
        }
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            if (fd == listen_fd) do_accept(epfd, listen_fd);
            else if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR))
                do_read(epfd, fd);
        }
    }

    printf("\n[RECV] total frames: %u\n", g_frame_count);
    close(epfd); close(listen_fd);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].fd >= 0) close(g_clients[i].fd);
    return EXIT_SUCCESS;
}

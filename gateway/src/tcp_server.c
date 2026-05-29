/*
 * tcp_server.c — MES 模拟服务端 (运行在 VM)
 *
 * epoll EPOLLET, 多客户端, TLV 解析
 * 编译: gcc -o tcp_server tcp_server.c tlv_protocol.c mes_handler.c -Wall
 * 运行: ./tcp_server [port]
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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define DEFAULT_PORT     8899
#define MAX_EVENTS       64
#define MAX_CLIENTS      256
#define BUF_SIZE         4096

static volatile int g_running = 1;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

typedef struct {
    int fd;
    tlv_reader_t reader;
} client_t;

static client_t g_clients[MAX_CLIENTS];

static int init_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen"); close(fd); return -1;
    }
    if (set_nonblock(fd) < 0) {
        perror("set_nonblock"); close(fd); return -1;
    }
    return fd;
}

static void do_accept(int epfd, int listen_fd)
{
    while (1) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int cfd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("accept"); break;
        }

        printf("[SRV] client connected: %s:%d (fd=%d)\n",
               inet_ntoa(peer.sin_addr), ntohs(peer.sin_port), cfd);

        if (cfd >= MAX_CLIENTS) {
            fprintf(stderr, "[SRV] too many clients, reject\n");
            close(cfd); continue;
        }

        set_nonblock(cfd);
        g_clients[cfd].fd = cfd;
        tlv_reader_init(&g_clients[cfd].reader);

        struct epoll_event ev = {0};
        ev.events  = EPOLLIN | EPOLLET;
        ev.data.fd = cfd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
    }
}

static void do_read(int epfd, int fd)
{
    client_t *c = &g_clients[fd];
    uint8_t buf[BUF_SIZE];
    uint8_t val_buf[TLV_MAX_VALUE];

    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                uint8_t tag = 0;
                int ret = tlv_reader_feed(&c->reader, &buf[i], 1,
                                          &tag, val_buf, sizeof(val_buf));
                if (ret > 0) {
                    mes_print_event(tag, val_buf, (uint16_t)ret);

                    if (tag == TLV_HEARTBEAT_REQ) {
                        uint8_t v[] = "{\"pong\":1}";
                        uint8_t resp[TLV_FRAME_OVERHEAD + 16];
                        size_t sz = tlv_pack(TLV_HEARTBEAT_RSP, v, sizeof(v)-1,
                                             resp, sizeof(resp));
                        if (sz > 0) write(fd, resp, sz);
                    }
                    if (tag == TLV_MES_EVENT) {
                        uint8_t ack[TLV_FRAME_OVERHEAD + 16];
                        size_t sz = mes_pack_ack(ack, sizeof(ack), tag);
                        if (sz > 0) write(fd, ack, sz);
                    }
                } else if (ret == -1) {
                    fprintf(stderr, "[SRV] CRC error fd=%d\n", fd);
                }
            }
        } else if (n == 0) {
            printf("[SRV] client disconnected (fd=%d)\n", fd);
            goto close_client;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("read"); goto close_client;
        }
    }
    return;

close_client:
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    c->fd = -1;
}

int main(int argc, char *argv[])
{
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    int listen_fd = init_listen(port);
    if (listen_fd < 0) return EXIT_FAILURE;

    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); close(listen_fd); return EXIT_FAILURE; }

    struct epoll_event ev = {0};
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    for (int i = 0; i < MAX_CLIENTS; i++) g_clients[i].fd = -1;

    printf("=== MES Simulator Server ===\n");
    printf("Listening on port %d (epoll EPOLLET)\n\n", port);

    struct epoll_event events[MAX_EVENTS];

    while (g_running) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait"); break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            if (fd == listen_fd)
                do_accept(epfd, listen_fd);
            else if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR))
                do_read(epfd, fd);
        }
    }

    printf("\n[SRV] shutting down...\n");
    close(epfd); close(listen_fd);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].fd >= 0) close(g_clients[i].fd);
    return EXIT_SUCCESS;
}
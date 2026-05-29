/*
 * tcp_client.c — 边缘网关通信客户端 (i.MX6ULL)
 *
 * epoll EPOLLET + stdin, 心跳, 交互式缺陷触发, 自动重连
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
#include <time.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define DEFAULT_SERVER_IP  "192.168.5.30"
#define DEFAULT_PORT       8899
#define MAX_EVENTS         4
#define HEARTBEAT_SEC      5
#define RECONNECT_SEC      3
#define BUF_SIZE           4096

static volatile int g_running = 1;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int connect_server(const char *ip, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid IP: %s\n", ip);
        close(fd); return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return -1;
    }

    set_nonblock(fd);
    return fd;
}

static int send_all(int fd, const uint8_t *buf, size_t len)
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

static void send_startup(int fd)
{
    uint8_t frame[TLV_FRAME_OVERHEAD + MES_JSON_MAX];
    size_t sz = mes_pack_startup(frame, sizeof(frame));
    if (sz > 0) send_all(fd, frame, sz);
}

static void send_heartbeat(int fd, uint32_t uptime, uint32_t frames)
{
    uint8_t frame[TLV_FRAME_OVERHEAD + MES_JSON_MAX];
    size_t sz = mes_pack_heartbeat(frame, sizeof(frame), uptime, frames);
    if (sz > 0) send_all(fd, frame, sz);
}

static void send_defect(int fd, int station)
{
    uint8_t frame[TLV_FRAME_OVERHEAD + MES_JSON_MAX];
    const char *types[] = {"scratch", "dent", "stain", "crack", "misalign"};
    const char *dt = types[rand() % 5];
    size_t sz = mes_pack_defect(frame, sizeof(frame), station, dt);
    if (sz > 0) send_all(fd, frame, sz);
}

static void handle_response(int fd, tlv_reader_t *reader)
{
    uint8_t buf[BUF_SIZE];
    uint8_t val[TLV_MAX_VALUE];

    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                uint8_t tag = 0;
                int ret = tlv_reader_feed(reader, &buf[i], 1,
                                          &tag, val, sizeof(val));
                if (ret > 0) {
                    printf("[CLI] <- ");
                    mes_print_event(tag, val, (uint16_t)ret);
                }
            }
        } else if (n == 0) {
            printf("[CLI] server closed connection\n");
            close(fd);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("read"); close(fd); return;
        }
    }
}

static void print_usage(void)
{
    printf("\n=== Edge Gateway Client ===\n");
    printf("  d / defect  Send defect event\n");
    printf("  s / status  Show status\n");
    printf("  h / help    Show help\n");
    printf("  q / quit    Exit\n\n");
}

int main(int argc, char *argv[])
{
    const char *ip   = (argc > 1) ? argv[1] : DEFAULT_SERVER_IP;
    int         port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    srand((unsigned)time(NULL));

    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); return EXIT_FAILURE; }

    struct epoll_event stdin_ev = {0};
    stdin_ev.events  = EPOLLIN;
    stdin_ev.data.fd = STDIN_FILENO;
    epoll_ctl(epfd, EPOLL_CTL_ADD, STDIN_FILENO, &stdin_ev);

    print_usage();

    int          sock_fd = -1;
    tlv_reader_t reader;
    time_t       last_hb = 0, start_time = time(NULL);
    uint32_t     frames_processed = 0;
    int          cmd_pending = 0;

    printf("[CLI] Connecting to %s:%d ...\n", ip, port);

    while (g_running) {
        if (sock_fd < 0) {
            sock_fd = connect_server(ip, port);
            if (sock_fd < 0) {
                fprintf(stderr, "[CLI] retry in %ds...\n", RECONNECT_SEC);
                sleep(RECONNECT_SEC); continue;
            }
            printf("[CLI] connected! fd=%d\n", sock_fd);

            struct epoll_event sock_ev = {0};
            sock_ev.events  = EPOLLIN | EPOLLET;
            sock_ev.data.fd = sock_fd;
            epoll_ctl(epfd, EPOLL_CTL_ADD, sock_fd, &sock_ev);

            tlv_reader_init(&reader);
            last_hb = time(NULL);
            send_startup(sock_fd);
        }

        time_t now = time(NULL);
        if (now - last_hb >= HEARTBEAT_SEC) {
            send_heartbeat(sock_fd, (uint32_t)(now - start_time), frames_processed);
            last_hb = now;
        }

        struct epoll_event events[MAX_EVENTS];
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 100);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait"); break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == STDIN_FILENO) {
                char line[256];
                if (fgets(line, sizeof(line), stdin) == NULL) {
                    g_running = 0; break;
                }
                if (line[0] == 'd' || strncmp(line, "defect", 6) == 0)
                    cmd_pending++;
                else if (line[0] == 's')
                    printf("[CLI] uptime=%lus frames=%u connected=%s\n",
                           (unsigned long)(now - start_time), frames_processed,
                           sock_fd >= 0 ? "yes" : "no");
                else if (line[0] == 'h')
                    print_usage();
                else if (line[0] == 'q')
                    { g_running = 0; break; }
            } else if (fd == sock_fd) {
                if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                    printf("[CLI] connection lost\n");
                    epoll_ctl(epfd, EPOLL_CTL_DEL, sock_fd, NULL);
                    close(sock_fd); sock_fd = -1;
                } else {
                    handle_response(sock_fd, &reader);
                }
            }
        }

        while (cmd_pending > 0 && sock_fd >= 0) {
            send_defect(sock_fd, 3);
            frames_processed++;
            cmd_pending--;
        }
    }

    printf("\n[CLI] shutting down...\n");
    if (sock_fd >= 0) close(sock_fd);
    close(epfd);
    return EXIT_SUCCESS;
}
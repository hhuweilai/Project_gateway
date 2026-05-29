/*
 * gateway_app.cpp -- Edge Gateway Main (C++11 multi-threaded)
 *
 * Architecture (3 threads):
 *   [Capture]   V4L2 mmap -> RingBuffer.push()
 *   [Network]   RingBuffer.pop() -> TLV FRAME_DATA -> TCP send
 *               + non-blocking read for AI_RESULT -> GPIO control
 *   [Main]      epoll: MES heartbeat/events/cmd + stdin
 *
 * Build: cross-compile via CMake + arm-linux-gnueabihf toolchain
 * Usage: ./gateway_app [server_ip] [port] [video_dev]
 */

#include "ring_buffer.h"
#include "video_capture.h"
#include "frame_sender.h"
#include "tlv_protocol.h"
#include "mes_handler.h"
#include "gpio_ops.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <ctime>
#include <thread>
#include <atomic>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* --- Constants --- */
#define DEFAULT_SERVER_IP  "192.168.5.30"
#define DEFAULT_PORT       8899
#define DEFAULT_VIDEO_DEV  "/dev/video0"
#define MAX_EVENTS         4
#define HEARTBEAT_SEC      5
#define RECONNECT_SEC      3
#define BUF_SIZE           65536

/* --- Global state --- */
static std::atomic<bool> g_running(true);
static RingBuffer g_ringbuf;

/* --- Signal handler --- */
static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
    g_ringbuf.close();
}

/* --- Set non-blocking --- */
static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* --- Connect to MES server --- */
static int connect_server(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
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

/* --- Send all bytes --- */
static int send_all(int fd, const uint8_t *buf, size_t len) {
    return fs_send_all(fd, buf, len);
}

/* --- Send startup event --- */
static void send_startup(int fd) {
    uint8_t frame[TLV_FRAME_OVERHEAD + MES_JSON_MAX];
    size_t sz = mes_pack_startup(frame, sizeof(frame));
    if (sz > 0) send_all(fd, frame, sz);
}

/* --- Send heartbeat --- */
static void send_heartbeat(int fd, uint32_t uptime, uint32_t frames) {
    uint8_t frame[TLV_FRAME_OVERHEAD + MES_JSON_MAX];
    size_t sz = mes_pack_heartbeat(frame, sizeof(frame), uptime, frames);
    if (sz > 0) send_all(fd, frame, sz);
}

/* --- Handle server response (MES connection) --- */
static void handle_response(int fd, tlv_reader_t *reader) {
    uint8_t buf[BUF_SIZE];
    uint8_t val[TLV_MAX_VALUE];

    while (g_running) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                uint8_t tag = 0;
                int ret = tlv_reader_feed(reader, &buf[i], 1,
                                          &tag, val, sizeof(val));
                if (ret > 0) {
                    printf("[GW] <- ");
                    mes_print_event(tag, val, (uint16_t)ret);
                }
            }
        } else if (n == 0) {
            printf("[GW] server closed connection\n");
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("read");
            break;
        }
    }
}

/* --- Handle AI result from network connection --- */
static void handle_ai_result(int sock_fd, tlv_reader_t *reader) {
    uint8_t buf[4096];
    uint8_t val[TLV_MAX_VALUE];
    ssize_t n = read(sock_fd, buf, sizeof(buf));
    if (n > 0) {
        printf("[NET] read %zd bytes:", n);
        for (ssize_t i = 0; i < n && i < 10; i++) printf(" %02x", buf[i]);
        printf("\n");
        for (ssize_t i = 0; i < n; i++) {
            uint8_t tag = 0;
            int ret = tlv_reader_feed(reader, &buf[i], 1, &tag, val, sizeof(val));
            if (ret != 0) {
                printf("[NET] feed ret=%d tag_byte=0x%02x\n", ret, tag);
            }
            if (ret > 0) {
                printf("[NET] parsed: tag=0x%02x len=%d\n", tag, ret);
                if (tag == TLV_AI_RESULT) {
                    int has_defect = 0;
                    char dtype[32] = "";
                    float conf = 0.0f;
                    mes_parse_ai_result(val, (uint16_t)ret, &has_defect, dtype,
                                        sizeof(dtype), &conf);
                    printf("[NET] AI_RESULT: defect=%d type=%s conf=%.2f\n",
                           has_defect, dtype, conf);
                    if (has_defect) {
                        trigger_alarm();
                    }
                }
            }
        }
    }
}

/* --- Capture thread --- */
static void capture_thread_func(const char *video_dev) {
    printf("[CAP] capture thread started\n");

    vcap_ctx_t *cap = vcap_open(video_dev);
    if (!cap) {
        fprintf(stderr, "[CAP] failed to open %s\n", video_dev);
        g_running = false;
        g_ringbuf.close();
        return;
    }

    if (vcap_start(cap) < 0) {
        fprintf(stderr, "[CAP] failed to start streaming\n");
        vcap_close(cap);
        g_running = false;
        g_ringbuf.close();
        return;
    }

    uint8_t *frame = new uint8_t[VCAP_FRAME_SIZE];
    uint32_t frame_count = 0;
    time_t last_report = time(NULL);

    while (g_running) {
        int len = vcap_grab(cap, frame, VCAP_FRAME_SIZE);
        if (len <= 0) {
            if (len == 0) continue;
            fprintf(stderr, "[CAP] grab failed\n");
            break;
        }

        if (!g_ringbuf.push(frame, (size_t)len)) {
            break;
        }

        frame_count++;

        time_t now = time(NULL);
        if (now != last_report) {
            printf("[CAP] %u fps\n", frame_count);
            frame_count = 0;
            last_report = now;
        }
    }

    delete[] frame;
    vcap_close(cap);
    printf("[VCAP] closed\n");
    printf("[CAP] capture thread exit\n");
}

/* --- Network thread --- */
static void network_thread_func(const char *server_ip, int server_port) {
    printf("[NET] network thread started\n");

    int sock_fd = -1;
    while (g_running && sock_fd < 0) {
        sock_fd = connect_server(server_ip, server_port);
        if (sock_fd < 0) {
            fprintf(stderr, "[NET] connect failed, retry in %ds...\n", RECONNECT_SEC);
            sleep(RECONNECT_SEC);
        }
    }
    if (sock_fd < 0) return;

    printf("[NET] connected! fd=%d\n", sock_fd);

    tlv_reader_t ai_reader;
    tlv_reader_init(&ai_reader);

    uint8_t *frame = new uint8_t[VCAP_FRAME_SIZE];
    size_t  frame_len;
    uint32_t sent_count = 0;
    time_t   last_report = time(NULL);

    while (g_running) {
        if (!g_ringbuf.pop(frame, &frame_len)) {
            break;
        }

        if (fs_send_frame(sock_fd, frame, frame_len) < 0) {
            fprintf(stderr, "[NET] send failed, reconnecting...\n");
            close(sock_fd);

            sock_fd = -1;
            while (g_running && sock_fd < 0) {
                sock_fd = connect_server(server_ip, server_port);
                if (sock_fd < 0) sleep(RECONNECT_SEC);
            }
            if (sock_fd < 0) break;
            tlv_reader_init(&ai_reader);
            printf("[NET] reconnected! fd=%d\n", sock_fd);
        }

        /* Check AI detection results (non-blocking) */
        handle_ai_result(sock_fd, &ai_reader);

        sent_count++;

        time_t now = time(NULL);
        if (now != last_report) {
            printf("[NET] %u fps sent\n", sent_count);
            sent_count = 0;
            last_report = now;
        }
    }

    delete[] frame;
    if (sock_fd >= 0) close(sock_fd);
    printf("[NET] network thread exit\n");
}

/* --- Help --- */
static void print_help(void) {
    printf("\n=== Edge Gateway ===\n");
    printf("  d / defect   Send defect event\n");
    printf("  s / status   Show status\n");
    printf("  h / help     Show this help\n");
    printf("  q / quit     Exit\n\n");
}

/* --- Main --- */
int main(int argc, char *argv[]) {
    const char *server_ip  = (argc > 1) ? argv[1] : DEFAULT_SERVER_IP;
    int         port       = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;
    const char *video_dev  = (argc > 3) ? argv[3] : DEFAULT_VIDEO_DEV;

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    gpio_export_all();

    printf("=== iMX6ULL Edge Gateway ===\n");
    printf("Server: %s:%d | Camera: %s\n", server_ip, port, video_dev);
    printf("Resolution: %dx%d YUYV\n\n", VCAP_WIDTH, VCAP_HEIGHT);

    /* Start capture thread */
    std::thread capture_th(capture_thread_func, video_dev);

    /* Start network send thread */
    std::thread network_th(network_thread_func, server_ip, port);

    /* ===== Main thread: MES + stdin ===== */
    int mes_fd = connect_server(server_ip, port);
    int mes_in_epoll = 0;

    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); return EXIT_FAILURE; }

    struct epoll_event stdin_ev = {};
    stdin_ev.events  = EPOLLIN;
    stdin_ev.data.fd = STDIN_FILENO;
    epoll_ctl(epfd, EPOLL_CTL_ADD, STDIN_FILENO, &stdin_ev);

    if (mes_fd >= 0) {
        struct epoll_event sock_ev = {};
        sock_ev.events  = EPOLLIN | EPOLLET;
        sock_ev.data.fd = mes_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, mes_fd, &sock_ev);
        mes_in_epoll = 1;
        send_startup(mes_fd);
    }

    tlv_reader_t reader;
    tlv_reader_init(&reader);

    time_t   last_hb    = time(NULL);
    time_t   start_time = time(NULL);
    uint32_t defect_count = 0;
    int      cmd_pending  = 0;

    print_help();

    struct epoll_event events[MAX_EVENTS];

    while (g_running) {
        if (mes_fd < 0) {
            mes_fd = connect_server(server_ip, port);
            if (mes_fd >= 0) {
                struct epoll_event sock_ev = {};
                sock_ev.events  = EPOLLIN | EPOLLET;
                sock_ev.data.fd = mes_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, mes_fd, &sock_ev);
                mes_in_epoll = 1;
                tlv_reader_init(&reader);
                send_startup(mes_fd);
                printf("[GW] MES reconnected\n");
            }
        }

        time_t now = time(NULL);
        if (mes_fd >= 0 && now - last_hb >= HEARTBEAT_SEC) {
            send_heartbeat(mes_fd,
                          (uint32_t)(now - start_time),
                          defect_count);
            last_hb = now;
        }

        int nfds = epoll_wait(epfd, events, MAX_EVENTS,
                              mes_fd >= 0 ? 1000 : RECONNECT_SEC * 1000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait"); break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == STDIN_FILENO) {
                char line[256];
                if (fgets(line, sizeof(line), stdin) == NULL) {
                    g_running = false;
                    break;
                }
                if (line[0] == 'd' || strncmp(line, "defect", 6) == 0) {
                    cmd_pending++;
                } else if (line[0] == 's' || strncmp(line, "status", 6) == 0) {
                    printf("[GW] uptime=%lus defects=%u mes=%s\n",
                           (unsigned long)(now - start_time), defect_count,
                           mes_fd >= 0 ? "connected" : "disconnected");
                } else if (line[0] == 'h' || strncmp(line, "help", 4) == 0) {
                    print_help();
                } else if (line[0] == 'q' || strncmp(line, "quit", 4) == 0) {
                    g_running = false;
                    break;
                }
            } else if (fd == mes_fd) {
                if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                    printf("[GW] MES connection lost\n");
                    epoll_ctl(epfd, EPOLL_CTL_DEL, mes_fd, NULL);
                    close(mes_fd);
                    mes_fd = -1;
                    mes_in_epoll = 0;
                } else {
                    handle_response(mes_fd, &reader);
                }
            }
        }

        while (cmd_pending > 0 && mes_fd >= 0) {
            uint8_t frame[TLV_FRAME_OVERHEAD + MES_JSON_MAX];
            const char *types[] = {"NG"};
            const char *dt = types[0];
            size_t sz = mes_pack_defect(frame, sizeof(frame), 3, dt);
            if (sz > 0) {
                send_all(mes_fd, frame, sz);
                defect_count++;
            }
            cmd_pending--;
        }
    }

    printf("\n[GW] shutting down...\n");
    g_running = false;
    g_ringbuf.close();

    if (mes_fd >= 0) {
        if (mes_in_epoll) epoll_ctl(epfd, EPOLL_CTL_DEL, mes_fd, NULL);
        close(mes_fd);
    }
    close(epfd);

    capture_th.join();
    network_th.join();

    printf("[GW] exit\n");
    return EXIT_SUCCESS;
}
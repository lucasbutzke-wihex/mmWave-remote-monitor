#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <stdalign.h>
#include <termios.h>

#include "serial.h"
#include "watchdog.h"
#include "tcp_server.h"
#include "common.h"

volatile int g_needs_reconfig = 0;
char g_cmd_line_buf[CMD_LINE_BUF_SIZE];
size_t g_cmd_line_len = 0;
uint32_t g_tx_sequence = 0;
int g_client_fd = -1;

static void handle_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}


int main() {
    RadarWatchdog wdt;
    watchdog_start(&wdt, 21, 1.0); //  pino 21, 1s de timeout

    signal(SIGINT, handle_sigint);

    int fd1 = configure_serial_port("/dev/ttyUSB1", B115200);
    int fd2 = configure_serial_port("/dev/ttyUSB0", B921600);
    const char *file_path  = configFile; // file to send to Port 1

    int listen_fd = setup_tcp_listener(TCP_SERVER_PORT);
    if (listen_fd < 0) {
        fprintf(stderr, "Failed to start TCP listener, exiting.\n");
        return 1;
    }

    FILE *file = fopen(file_path, "r");
    if (!file) {
        perror("Error opening input file");
    }

    // Port 1 state
    port1_state_t state = PORT1_IDLE;
    char response_buffer[BUFFER_SIZE];
    size_t response_len = 0;
    long response_deadline_ms = 0;
    long cooldown_until_ms = 0; // non-blocking replacement for usleep(100ms)

    // Port 2 state
    // ARM ALIGNMENT FIX: Explicitly direct the compiler to align the static char stream accumulation buffer
    alignas(16) static char port2_accum[PORT2_ACCUM_SIZE];
    size_t port2_accum_len = 0;

    printf("Protocol Engine Server (TCP) running on port %d...\n", TCP_SERVER_PORT);

    while (!g_stop) {
        if (g_needs_reconfig) {
            printf("\n[RECONFIG] Reiniciando rotina de configuração do radar...\n");
            
            tcflush(fd1, TCIOFLUSH);
            tcflush(fd2, TCIOFLUSH);
            port2_accum_len = 0;
            memset(port2_accum, 0, sizeof(port2_accum));

            fseek(file, 0, SEEK_SET);

            // Reseta maquina de estados 
            state = PORT1_IDLE;
            cooldown_until_ms = now_ms() + 1000; // aguarda 1s 
            response_len = 0;

            watchdog_feed(&wdt);

            g_needs_reconfig = 0;
            printf("[RECONFIG] Iniciando retransmissão do arquivo de configuração...\n");
        }

        struct pollfd fds[4];
        int nfds = 0;

        fds[nfds].fd = fd1; fds[nfds].events = POLLIN; nfds++;
        fds[nfds].fd = fd2; fds[nfds].events = POLLIN; nfds++;
        fds[nfds].fd = listen_fd; fds[nfds].events = POLLIN; nfds++;

        int client_idx = -1;
        if (g_client_fd >= 0) {
            client_idx = nfds;
            fds[nfds].fd = g_client_fd; fds[nfds].events = POLLIN; nfds++;
        }

        int ready = poll(fds, nfds, 100);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("poll failed");
            break;
        }

        if (fds[2].revents & POLLIN) {
            accept_new_client(listen_fd);
        }

        if (client_idx >= 0 && (fds[client_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            handle_client_data(fd1, fd2);
        }

        if (fds[0].revents & POLLIN) {
            char rx_buffer[BUFFER_SIZE];
            ssize_t n = read(fd1, rx_buffer, sizeof(rx_buffer));
            if (n > 0) {
                send_async_packet(PKT_TYPE_CLI_RESP, rx_buffer, n);
            }
            watchdog_feed(&wdt);
        }

        if (fds[1].revents & POLLIN) {
            char rx_buffer[BUFFER_SIZE];
            ssize_t n = read(fd2, rx_buffer, sizeof(rx_buffer));
            if (n > 0) {
                send_async_packet(PKT_TYPE_RADAR, rx_buffer, n);
                port2_feed(port2_accum, &port2_accum_len, rx_buffer, (size_t)n);
            }
            watchdog_feed(&wdt);
        }
    }

    close_client();
    close(listen_fd);
    if (fd1 >= 0) close(fd1);
    if (fd2 >= 0) close(fd2);
    watchdog_stop(&wdt);
    return 0;
}

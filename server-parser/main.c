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
#include "logger.h"


char g_cmd_line_buf[CMD_LINE_BUF_SIZE];
size_t g_cmd_line_len = 0;
uint32_t g_tx_sequence = 0;
int g_client_fd = -1;

static void handle_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}


int main() {
    logger_init("/tmp/test.log", 1024 * 1024, 5);

    RadarWatchdog wdt;
    watchdog_start(&wdt, 21, 1.0); //  pino 21, 1s de timeout

    signal(SIGINT, handle_sigint);

    int fd1 = configure_serial_port("/dev/ttyUSB1", B115200);
    int fd2 = configure_serial_port("/dev/ttyUSB0", B921600);

    int listen_fd = setup_tcp_listener(TCP_SERVER_PORT);
    if (listen_fd < 0) {
        fprintf(stderr, "Failed to start TCP listener, exiting.\n");
        return 1;
    }

    // ARM ALIGNMENT FIX: Explicitly direct the compiler to align the static char stream accumulation buffer
    alignas(16) static char port2_accum[PORT2_ACCUM_SIZE];
    size_t port2_accum_len = 0;

    printf("Protocol Engine Server (TCP) running on port %d...\n", TCP_SERVER_PORT);

    while (!g_stop) {
        struct pollfd fds[4];
        int nfds = 0;

        fds[nfds].fd = fd1; 
        fds[nfds].events = POLLIN; 
        nfds++;

        fds[nfds].fd = fd2; 
        fds[nfds].events = POLLIN; 
        nfds++;

        fds[nfds].fd = listen_fd; 
        fds[nfds].events = POLLIN; 
        nfds++;

        int client_idx = -1;
        if (g_client_fd >= 0) {
            client_idx = nfds;

            fds[nfds].fd = g_client_fd;
            fds[nfds].events = POLLIN;
            nfds++;
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

        if (client_idx >= 0 && 
            (fds[client_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
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
                logger_log(LOG_INFO, rx_buffer);
                send_async_packet(PKT_TYPE_RADAR, rx_buffer, n);
                port2_feed(port2_accum, &port2_accum_len, rx_buffer, (size_t)n);
            }
            watchdog_feed(&wdt);
        }
    }

    logger_shutdown();

    close_client();
    close(listen_fd);
    if (fd1 >= 0) close(fd1);
    if (fd2 >= 0) close(fd2);
    watchdog_stop(&wdt);
    
    return EXIT_SUCCESS;
}

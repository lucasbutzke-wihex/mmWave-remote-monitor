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
#include <stdbool.h>

#include "serial.h"
#include "watchdog.h"
#include "tcp_server.h"
#include "common.h"
#include "logger.h"

char g_cmd_line_buf[CMD_LINE_BUF_SIZE];
size_t g_cmd_line_len = 0;
uint32_t g_tx_sequence = 0;
int g_client_fd = -1;

const char * FILE_BACKUP = "/tmp/test.log";

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;

    g_running = 0;
    g_stop = 1;
}

int main(int argc, char *argv[])
{
    int ret = EXIT_FAILURE;

    int fd1 = -1;
    int fd2 = -1;
    int listen_fd = -1;

    bool logger_started = false;
    bool watchdog_started = false;

    RadarWatchdog wdt;

    // test arguments
    if (argc != 3)
    {
        fprintf(stderr,
                "Usage: %s <serial-port> <serial-port>\n",
                argv[0]);

        return EXIT_FAILURE;
    }

    const char *port1 = argv[1];
    const char *port2 = argv[2];

    // config extern signal
    struct sigaction sa = {0};

    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // init logger
    if (logger_init(FILE_BACKUP, 1024 * 1024, 5) < 0)
    {
        fprintf(stderr, "Failed to initialize logger\n");
        return EXIT_FAILURE;
    }

    logger_started = true;

    LOG_INFO("Starting application");

    if (watchdog_start(&wdt, 21, 1.0) < 0)
    {
        LOG_ERROR("Failed to start watchdog");
        goto cleanup;
    }

    watchdog_started = true;

    LOG_INFO("[Serial] Opening port '%s'", port1);

    fd1 = configure_serial_port(port1, B115200);

    if (fd1 < 0)
    {
        LOG_ERROR("[Serial] Failed to configure '%s'", port1);
        goto cleanup;
    }

    LOG_INFO("[Serial] Opening port '%s'", port2);

    fd2 = configure_serial_port(port2, B921600);

    if (fd2 < 0)
    {
        LOG_ERROR("[Serial] Failed to configure '%s'", port2);
        goto cleanup;
    }

    listen_fd = setup_tcp_listener(TCP_SERVER_PORT);

    if (listen_fd < 0)
    {
        LOG_ERROR("Failed to start TCP listener");
        goto cleanup;
    }

    alignas(16) static char port2_accum[PORT2_ACCUM_SIZE];

    size_t port2_accum_len = 0;

    LOG_INFO("Protocol Engine Server running on port %d", TCP_SERVER_PORT);

    while (g_running && !g_stop)
    {
        struct pollfd fds[4];
        int nfds = 0;

        fds[nfds].fd = fd1;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;

        fds[nfds].fd = fd2;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;

        fds[nfds].fd = listen_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;

        int client_idx = -1;

        if (g_client_fd >= 0)
        {
            client_idx = nfds;

            fds[nfds].fd = g_client_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;

            nfds++;
        }

        int ready = poll(fds, nfds, 100);

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;

            LOG_ERROR("poll failed: %s", strerror(errno));
            break;
        }

        if (fds[2].revents & POLLIN)
        {
            accept_new_client(listen_fd);
        }

        if (client_idx >= 0 &&
            (fds[client_idx].revents &
             (POLLIN | POLLHUP | POLLERR)))
        {
            handle_client_data(fd1, fd2);
        }

        /*
         * CLI serial
         */
        if (fds[0].revents & POLLIN)
        {
            char rx_buffer[BUFFER_SIZE];

            ssize_t n = read(fd1,
                             rx_buffer,
                             sizeof(rx_buffer));

            if (n > 0)
            {
                send_async_packet(PKT_TYPE_CLI_RESP,
                                  rx_buffer,
                                  (size_t)n);
            }
            else if (n < 0 &&
                     errno != EAGAIN &&
                     errno != EWOULDBLOCK &&
                     errno != EINTR)
            {
                LOG_ERROR("[Serial] CLI read failed: %s",
                          strerror(errno));
            }

            watchdog_feed(&wdt);
        }

        /*
         * Radar serial
         */
        if (fds[1].revents & POLLIN)
        {
            char rx_buffer[BUFFER_SIZE];

            ssize_t n = read(fd2,
                             rx_buffer,
                             sizeof(rx_buffer));

            if (n > 0)
            {
                send_async_packet(PKT_TYPE_RADAR,
                                  rx_buffer,
                                  (size_t)n);

                port2_feed(port2_accum,
                           &port2_accum_len,
                           rx_buffer,
                           (size_t)n);
            }
            else if (n < 0 &&
                     errno != EAGAIN &&
                     errno != EWOULDBLOCK &&
                     errno != EINTR)
            {
                LOG_ERROR("[Serial] Radar read failed: %s",
                          strerror(errno));
            }

            watchdog_feed(&wdt);
        }
    }

    LOG_INFO("Shutdown requested");

    ret = EXIT_SUCCESS;

cleanup:

    /*
     * Cleanup in REVERSE initialization order.
     */

    if (g_client_fd >= 0)
    {
        close_client();
    }

    if (listen_fd >= 0)
    {
        close(listen_fd);
        listen_fd = -1;
    }

    if (fd1 >= 0)
    {
        close(fd1);
        fd1 = -1;
    }

    if (fd2 >= 0)
    {
        close(fd2);
        fd2 = -1;
    }

    if (watchdog_started)
    {
        watchdog_stop(&wdt);
        watchdog_started = false;
    }

    if (logger_started)
    {
        LOG_INFO("Application stopped");
        logger_shutdown();
        logger_started = false;
    }

    return ret;
}

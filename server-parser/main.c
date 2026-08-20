#define _DEFAULT_SOURCE  // Ativa funções POSIX/BSD como usleep no C11

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
#include <pthread.h>

#include "serial.h"
#include "watchdog.h"
#include "tcp_server.h"
#include "common.h"
#include "logger.h"
#include "ring_buffer.h"

#define CONFIG_DELAY_MS 100

static long config_next_send_ms = 0;

char g_cmd_line_buf[CMD_LINE_BUF_SIZE];
size_t g_cmd_line_len = 0;
uint32_t g_tx_sequence = 0;
int g_client_fd = -1;

alignas(16) static uint8_t port2_accum[PORT2_ACCUM_SIZE];
size_t port2_accum_len = 0;

static volatile sig_atomic_t g_running;
static volatile sig_atomic_t g_stop;

RadarWatchdog wdt;
RadarRingBuffer radar_ring;

static pthread_t radar_thread;
static pthread_t tcp_thread;

static void signal_handler(int sig)
{
    (void)sig;

    g_running = 0;
    g_stop = 1;
}

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (long)(ts.tv_sec * 1000L +
                  ts.tv_nsec / 1000000L);
}

#include "ring_buffer.h"

static size_t ring_available( RadarRingBuffer *rb) {

    if (rb->head >= rb->tail)
        return rb->head - rb->tail;

    return rb->size - rb->tail + rb->head;

}

static size_t ring_free(RadarRingBuffer *rb)
{
    return rb->size -
           ring_available(rb) -
           1;
}

static size_t ring_write(
    RadarRingBuffer *rb,
    const uint8_t *data,
    size_t len)
{
    pthread_mutex_lock(&rb->mutex);

    size_t free_space = ring_free(rb);

    if (len > free_space) {
        len = free_space;
    }

    for (size_t i = 0; i < len; i++) {
        rb->data[rb->head] = data[i];

        rb->head++;

        if (rb->head == rb->size)
            rb->head = 0;
    }

    pthread_cond_signal(&rb->data_available);

    pthread_mutex_unlock(&rb->mutex);

    return len;
}

#define TX_QUEUE_SIZE 128

typedef struct {
    uint32_t type;
    size_t len;
    uint8_t data[BUFFER_SIZE];
} TxPacket;

static TxPacket tx_queue[TX_QUEUE_SIZE];

static size_t tx_head = 0;
static size_t tx_tail = 0;

static pthread_mutex_t tx_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t tx_cond =
    PTHREAD_COND_INITIALIZER;

static bool tx_running = true;

static size_t tx_queue_count(void)
{
    if (tx_head >= tx_tail)
        return tx_head - tx_tail;

    return TX_QUEUE_SIZE - tx_tail + tx_head;
}

static bool tx_queue_full(void)
{
    return tx_queue_count() >= TX_QUEUE_SIZE - 1;
}

static bool tx_queue_empty(void)
{
    return tx_head == tx_tail;
}

void *tcp_tx_thread(void *arg)
{
    (void)arg;

    while (tx_running && g_running && !g_stop) {

        pthread_mutex_lock(&tx_mutex);

        while (tx_queue_empty() &&
               tx_running &&
               g_running &&
               !g_stop) {

            pthread_cond_wait(
                &tx_cond,
                &tx_mutex);
        }

        if (!tx_running ||
            !g_running ||
            g_stop) {

            pthread_mutex_unlock(&tx_mutex);
            break;
        }

        TxPacket packet = tx_queue[tx_tail];

        tx_tail++;

        if (tx_tail == TX_QUEUE_SIZE)
            tx_tail = 0;

        pthread_mutex_unlock(&tx_mutex);

        /*
         * TCP work happens OUTSIDE the mutex.
         */
        if (g_client_fd >= 0) {

            AsyncProtocolHeader header;

            header.packet_type =
                htonl(packet.type);

            header.sequence_num =
                htonl(g_tx_sequence++);

            header.payload_len =
                htonl((uint32_t)packet.len);

            uint8_t tx_buffer[
                sizeof(AsyncProtocolHeader) +
                BUFFER_SIZE
            ];

            memcpy(
                tx_buffer,
                &header,
                sizeof(header));

            memcpy(
                tx_buffer + sizeof(header),
                packet.data,
                packet.len);

            size_t total =
                sizeof(header) + packet.len;

            size_t sent = 0;

            while (sent < total &&
                   g_client_fd >= 0) {

                ssize_t n = send(
                    g_client_fd,
                    tx_buffer + sent,
                    total - sent,
                    MSG_NOSIGNAL);

                if (n > 0) {
                    sent += (size_t)n;
                    continue;
                }

                if (n < 0 &&
                    errno == EINTR) {
                    continue;
                }

                if (n < 0 &&
                    (errno == EAGAIN ||
                     errno == EWOULDBLOCK)) {

                    /*
                     * TCP socket is full.
                     *
                     * Don't lose the packet here.
                     * Wait briefly and retry.
                     */
                    usleep(1000);
                    continue;
                }

                LOG_ERROR(
                    "[TCP] send failed: %s",
                    strerror(errno));

                close_client();

                break;
            }
        }
    }

    return NULL;
}

void send_async_packet(
    uint32_t type,
    const void *payload,
    size_t payload_len)
{
    if (!payload || payload_len == 0)
        return;

    if (payload_len > BUFFER_SIZE) {
        LOG_ERROR(
            "[TCP] Packet too large: %zu",
            payload_len);
        return;
    }

    pthread_mutex_lock(&tx_mutex);

    /*
     * Do not block the UART thread.
     *
     * If this happens, TCP is falling behind.
     */
    if (tx_queue_full()) {

        pthread_mutex_unlock(&tx_mutex);

        LOG_ERROR(
            "[TCP] TX queue FULL - packet dropped");

        return;
    }

    TxPacket *packet = &tx_queue[tx_head];

    packet->type = type;
    packet->len = payload_len;

    memcpy(
        packet->data,
        payload,
        payload_len);

    tx_head++;

    if (tx_head == TX_QUEUE_SIZE)
        tx_head = 0;

    pthread_cond_signal(&tx_cond);

    pthread_mutex_unlock(&tx_mutex);
}

static void *radar_rx_thread(void *arg) {
    int fd2 = *(int *)arg;

    uint8_t rx_buffer[BUFFER_SIZE];

    while (g_running && !g_stop) {

        ssize_t n = read(
            fd2,
            rx_buffer,
            sizeof(rx_buffer));

        if (n > 0) {

            /*
             * Immediately forward to the
             * asynchronous TCP queue.
             */
            send_async_packet(
                PKT_TYPE_RADAR,
                rx_buffer,
                (size_t)n);

            /*
             * Keep your parser here initially.
             */
            port2_feed(
                port2_accum,
                &port2_accum_len,
                rx_buffer,
                (size_t)n);
        }
        else if (n < 0) {

            if (errno == EINTR)
                continue;

            if (errno == EAGAIN ||
                errno == EWOULDBLOCK)
                continue;

            LOG_ERROR(
                "[Radar] read failed: %s",
                strerror(errno));

            break;
        }
    }

    return NULL;
}


int main(int argc, char *argv[])
{
    int ret = EXIT_FAILURE;

    g_running = 1;
    g_stop = 0;

    int fd1 = -1;
    int fd2 = -1;
    int listen_fd = -1;
    FILE *config_file = NULL;

    bool logger_started = false;
    bool watchdog_started = false;

    if (argc != 3)
    {
        fprintf(stderr,
                "Usage: %s <serial-port> <serial-port>\n",
                argv[0]);

        return EXIT_FAILURE;
    }

    const char *port1 = argv[1];
    const char *port2 = argv[2];

    struct sigaction sa = {0};

    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (logger_init("/tmp/test.log", 1024 * 1024, 5) < 0)
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

    if (pthread_create(
            &radar_thread,
            NULL,
            radar_rx_thread,
            &fd2) != 0) {

        LOG_ERROR(
            "Failed to create radar RX thread");

        goto cleanup;
    }

    listen_fd = setup_tcp_listener(TCP_SERVER_PORT);

    if (listen_fd < 0)
    {
        LOG_ERROR("Failed to start TCP listener");
        goto cleanup;
    }

    /*
     * CLI configuration state.
     */
    config_file = fopen(configFile, "r");

    if (config_file == NULL)
    {
        LOG_ERROR("Failed to open config file '%s': %s",
                  configFile,
                  strerror(errno));
        goto cleanup;
    }

    bool config_done = false;
    bool config_waiting = false;

    char config_line[BUFFER_SIZE];
    char config_response[BUFFER_SIZE];

    size_t config_response_len = 0;
    long config_deadline_ms = 0;

    LOG_INFO("Loading radar configuration from '%s'",
             configFile);

    LOG_INFO("Protocol Engine Server running on port %d",
             TCP_SERVER_PORT);

    while (g_running && !g_stop)
    {
        /*
         * Send next configuration command when the CLI
         * is not waiting for a response.
         */
        if (!config_done && 
            !config_waiting && 
            now_ms() >= config_next_send_ms)
        {
            if (fgets(config_line,
                      sizeof(config_line),
                      config_file) != NULL)
            {
                size_t len = strlen(config_line);

                if (len > 0)
                {
                    ssize_t written = write(fd1,
                                            config_line,
                                            len);

                    if (written < 0)
                    {
                        LOG_ERROR(
                            "[Config] Failed to write command: %s",
                            strerror(errno));

                        fclose(config_file);
                        config_file = NULL;

                        goto cleanup;
                    }

                    config_response_len = 0;
                    memset(config_response,
                           0,
                           sizeof(config_response));

                    config_deadline_ms =
                        now_ms() + RESPONSE_TIMEOUT_MS;

                    config_waiting = true;

                    LOG_INFO("[Config] Sent: %s",
                             config_line);

                    config_waiting = false;
                    config_next_send_ms = now_ms() + CONFIG_DELAY_MS;
                }
            }
            else
            {
                config_done = true;

                LOG_INFO(
                    "[Config] Configuration complete");
            }
        }

        struct pollfd fds[3];
        int nfds = 0;

        fds[nfds].fd = fd1;
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

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;

            LOG_ERROR("poll failed: %s",
                      strerror(errno));

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
            if (config_done)
            {
                handle_client_data(&fd1, &fd2);
            }
        }

        /*
         * CLI serial.
         */
        if (fds[0].revents & POLLIN)
        {
            char rx_buffer[BUFFER_SIZE];

            ssize_t n = read(fd1,
                             rx_buffer,
                             sizeof(rx_buffer));

            if (n > 0)
            {
                if (config_waiting)
                {
                    if (config_response_len +
                        (size_t)n <
                        sizeof(config_response))
                    {
                        memcpy(config_response +
                                   config_response_len,
                               rx_buffer,
                               (size_t)n);

                        config_response_len +=
                            (size_t)n;

                        config_response[
                            config_response_len] = '\0';
                    }

                    int line_count = 0;

                    for (size_t i = 0;
                         i < config_response_len;
                         i++)
                    {
                        if (config_response[i] == '\n')
                        {
                            line_count++;
                        }
                    }

                    if (line_count >= 2)
                    {
                        LOG_INFO(
                            "[Config] Response received: %s",
                            config_response);

                        config_response_len = 0;
                        config_waiting = false;
                    }
                }
                else
                {
                    send_async_packet(
                        PKT_TYPE_CLI_RESP,
                        rx_buffer,
                        (size_t)n);
                }
            }
            else if (n < 0 &&
                     errno != EAGAIN &&
                     errno != EWOULDBLOCK &&
                     errno != EINTR)
            {
                LOG_ERROR(
                    "[Serial] CLI read failed: %s",
                    strerror(errno));
            }

            watchdog_feed(&wdt);
        }

        /*
         * Configuration response timeout.
         */
        if (config_waiting &&
            now_ms() >= config_deadline_ms)
        {
            LOG_WARN(
                "[Config] Response timeout, continuing");

            config_response_len = 0;
            config_waiting = false;
        }

        /*
         * Radar serial.
         */
        if (fds[1].revents & POLLIN)
        {
            uint8_t rx_buffer[BUFFER_SIZE];

            ssize_t n = read(fd2,
                             rx_buffer,
                             sizeof(rx_buffer));

            if (n > 0)
            {
                send_async_packet(
                    PKT_TYPE_RADAR,
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
                LOG_ERROR(
                    "[Serial] Radar read failed: %s",
                    strerror(errno));
            }

            watchdog_feed(&wdt);
        }
    }

    LOG_INFO("Shutdown requested");

    ret = EXIT_SUCCESS;

cleanup:

    if (config_file != NULL)
    {
        fclose(config_file);
        config_file = NULL;
    }

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
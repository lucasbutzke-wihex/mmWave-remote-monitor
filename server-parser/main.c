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
#include <sys/time.h>
#include <gpiod.h>

#include "serial.h"
#include "watchdog.h"
#include "tcp_server.h"
#include "common.h"
#include "logger.h"

#define CONFIG_DELAY_MS 100

static long config_next_send_ms = 0;

char g_cmd_line_buf[CMD_LINE_BUF_SIZE];
size_t g_cmd_line_len = 0;
uint32_t g_tx_sequence = 0;
int g_client_fd = -1;

static volatile sig_atomic_t g_running = 1;

static struct gpiod_chip *chip = NULL;
static struct gpiod_line_settings *settings = NULL;
static struct gpiod_line_config *line_cfg = NULL;
static struct gpiod_request_config *req_cfg = NULL;
static struct gpiod_line_request *request = NULL;

void gpio_export()
{
    chip = gpiod_chip_open("/dev/gpiochip4");
    if (!chip) {
        chip = gpiod_chip_open("/dev/gpiochip0");
        if (!chip) {
            LOG_ERROR("[LED TOGGLE] Erro ao abrir chip de GPIO");
            return;
        }
    }
}

void gpio_config(unsigned int offset)
{
    if (!chip) {
        return; 
    }
    
    settings = gpiod_line_settings_new();
    if (!settings) {
        LOG_ERROR("[LED TOGGLE] Erro ao criar configurações");
        return;
    }

    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT); // define como saida
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_ACTIVE); // define nivel logico alto

    line_cfg = gpiod_line_config_new();
    if (!line_cfg) {
        LOG_ERROR("[LED TOGGLE] Erro ao criar configuração da linha");
        gpiod_line_settings_free(settings);
        return;
    }

    if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings) < 0) {
        LOG_ERROR("[LED TOGGLE] Erro ao adicionar configurações da linha");
        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);
        return;
    }

    //offset de pino
    req_cfg = gpiod_request_config_new();
    if (!req_cfg) {
         LOG_ERROR("[WATCHDOG] Erro ao criar request config");
         gpiod_line_config_free(line_cfg);
         gpiod_line_settings_free(settings);
         return;
    }
    gpiod_request_config_set_consumer(req_cfg, "ToggleLED");

    // solicita o controle do pino
    request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    if (!request) {
        LOG_ERROR("[LED TOGGLE] Erro ao requisitar linha GPIO");
        gpiod_request_config_free(req_cfg);
        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);
        return;
    }
}

void gpio_write(unsigned int offset, enum gpiod_line_value value) 
{
    if (request) {
        gpiod_line_request_set_value(request, offset, value);
    }
}

double get_current_time() // retorna tempo atual (s)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

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

int main(int argc, char *argv[])
{
    int ret = EXIT_FAILURE;

    int fd1 = -1;
    int fd2 = -1;
    int listen_fd = -1;
    FILE *config_file = NULL;

    bool logger_started = false;
    bool watchdog_started = false;

    RadarWatchdog wdt;

    unsigned int led_pin = 2;
    bool led_state = false;
    const double time_interval = 1.0; //define intervalo de 1s entre toggle do led
    double last_toggle = get_current_time();

    gpio_export();
    gpio_config(led_pin);

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

    alignas(16) static char port2_accum[PORT2_ACCUM_SIZE];

    size_t port2_accum_len = 0;

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
        double now = get_current_time();
        
        if (now - last_toggle >= time_interval) 
        {    
            // printf("%.2f, %.2f\n", now, last_toggle);
            led_state = !led_state; // Inverte o estado
            
            enum gpiod_line_value value = led_state ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
            gpio_write(led_pin, value);
            
            last_toggle = now; 
            // printf("toggle led\n");
        }

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
                handle_client_data(fd1, fd2);
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
            char rx_buffer[BUFFER_SIZE];

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
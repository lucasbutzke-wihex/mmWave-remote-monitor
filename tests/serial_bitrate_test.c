// gcc -O2 -Wall -Wextra -o mmwave_uart_test mmwave_uart_test.c
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define BAUD_RATE       921600
#define BUFFER_SIZE     (64 * 1024)

/*
 * Set to 1 only when the sender transmits a known test pattern.
 *
 * For a real mmWave radar stream, leave this at 0 because the radar
 * sends binary TLV data, not a predictable sequence.
 */
#define CHECK_PATTERN   0

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

static double now_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (double)ts.tv_sec +
           (double)ts.tv_nsec / 1e9;
}

static int configure_serial(int fd)
{
    struct termios tty;

    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        return -1;
    }

    /*
     * Raw binary mode.
     */
    cfmakeraw(&tty);

    /*
     * Enable receiver.
     *
     * CLOCAL:
     *   Ignore modem control lines.
     *
     * CREAD:
     *   Enable receiver.
     */
    tty.c_cflag |= CLOCAL | CREAD;

    /*
     * 8 data bits.
     */
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;

    /*
     * No parity.
     */
    tty.c_cflag &= ~PARENB;

    /*
     * One stop bit.
     */
    tty.c_cflag &= ~CSTOPB;

    /*
     * No hardware flow control.
     */
    tty.c_cflag &= ~CRTSCTS;

    /*
     * No software flow control.
     */
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);

    /*
     * Blocking read.
     *
     * read() waits until at least one byte is available.
     */
    tty.c_cc[VMIN]  = 1;
    tty.c_cc[VTIME] = 0;

    /*
     * 921600 baud.
     *
     * This is the default DATA port baud rate for the
     * TI mmWave SDK.
     */
    if (cfsetispeed(&tty, B921600) != 0) {
        perror("cfsetispeed");
        return -1;
    }

    if (cfsetospeed(&tty, B921600) != 0) {
        perror("cfsetospeed");
        return -1;
    }

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        return -1;
    }

    /*
     * Remove any bytes that arrived before the test.
     */
    if (tcflush(fd, TCIFLUSH) != 0) {
        perror("tcflush");
        return -1;
    }

    return 0;
}

#if CHECK_PATTERN

/*
 * Expected test pattern:
 *
 * 00 01 02 03 ... FE FF 00 01 ...
 */
static uint8_t expected_byte = 0;

static uint64_t pattern_errors = 0;

static void check_data(const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; i++) {

        if (data[i] != expected_byte) {
            pattern_errors++;

            /*
             * Resynchronize at the received byte.
             *
             * This makes the test useful even after one lost byte.
             */
            expected_byte = data[i];
        }

        expected_byte++;
    }
}

#endif

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr,
                "Usage: %s <serial_device>\n\n"
                "Example:\n"
                "  %s /dev/ttyUSB0\n",
                argv[0],
                argv[0]);

        return EXIT_FAILURE;
    }

    const char *device = argv[1];

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /*
     * Open read-only.
     */
    int fd = open(device, O_RDONLY | O_NOCTTY);

    if (fd < 0) {
        fprintf(stderr,
                "Cannot open %s: %s\n",
                device,
                strerror(errno));

        return EXIT_FAILURE;
    }

    if (configure_serial(fd) != 0) {
        close(fd);
        return EXIT_FAILURE;
    }

    printf("\n");
    printf("========================================\n");
    printf("       mmWave UART bitrate test\n");
    printf("========================================\n");
    printf("Device       : %s\n", device);
    printf("Baud         : %d\n", BAUD_RATE);
    printf("Format       : 8N1\n");
    printf("Flow control : none\n");
    printf("Buffer       : %d KiB\n", BUFFER_SIZE / 1024);
    printf("Expected RX  : %.2f KiB/s\n",
           BAUD_RATE / 10.0 / 1024.0);
    printf("Expected bits: %d bit/s\n", BAUD_RATE);

#if CHECK_PATTERN
    printf("Integrity    : ENABLED\n");
#else
    printf("Integrity    : disabled (radar mode)\n");
#endif

    printf("========================================\n");
    printf("Waiting for radar data...\n");
    printf("Press Ctrl+C to stop.\n\n");

    uint8_t buffer[BUFFER_SIZE];

    uint64_t total_bytes = 0;
    uint64_t interval_bytes = 0;

    uint64_t read_errors = 0;

    double start_time = now_seconds();
    double last_report = start_time;

    while (running) {

        ssize_t n = read(fd, buffer, sizeof(buffer));

        if (n > 0) {

            total_bytes += (uint64_t)n;
            interval_bytes += (uint64_t)n;

#if CHECK_PATTERN
            check_data(buffer, (size_t)n);
#endif
        }
        else if (n < 0) {

            if (errno == EINTR)
                continue;

            read_errors++;

            fprintf(stderr,
                    "\nread(): %s\n",
                    strerror(errno));

            break;
        }

        double current_time = now_seconds();

        if ((current_time - last_report) >= 1.0) {

            double interval_time =
                current_time - last_report;

            double total_time =
                current_time - start_time;

            double bytes_per_second =
                interval_bytes / interval_time;

            double total_bytes_per_second =
                total_bytes / total_time;

            /*
             * 8 data bits + 1 start + 1 stop = 10 bits/byte.
             */
            double effective_uart_bps =
                bytes_per_second * 10.0;

            printf("\r"
                   "RX: %9.0f B/s | "
                   "%7.2f KiB/s | "
                   "%8.0f bit/s | "
                   "total: %llu bytes",
                   bytes_per_second,
                   bytes_per_second / 1024.0,
                   effective_uart_bps,
                   (unsigned long long)total_bytes);

#if CHECK_PATTERN
            printf(" | errors: %llu",
                   (unsigned long long)pattern_errors);
#endif

            fflush(stdout);

            interval_bytes = 0;
            last_report = current_time;
        }
    }

    double elapsed = now_seconds() - start_time;

    printf("\n\n========================================\n");
    printf("Test finished\n");
    printf("========================================\n");

    if (elapsed > 0.0) {

        double bytes_per_second =
            total_bytes / elapsed;

        printf("Duration       : %.2f s\n", elapsed);
        printf("Total bytes    : %llu\n",
               (unsigned long long)total_bytes);

        printf("Average        : %.2f B/s\n",
               bytes_per_second);

        printf("Average        : %.2f KiB/s\n",
               bytes_per_second / 1024.0);

        printf("Effective UART : %.0f bit/s\n",
               bytes_per_second * 10.0);

        printf("Read errors    : %llu\n",
               (unsigned long long)read_errors);

#if CHECK_PATTERN
        printf("Data errors    : %llu\n",
               (unsigned long long)pattern_errors);
#endif
    }

    close(fd);

    return EXIT_SUCCESS;
}

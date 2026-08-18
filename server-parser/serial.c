#define _DEFAULT_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

#include "serial.h"
#include "logger.h"
#include "common.h"
#include "tcp_server.h"


int configure_serial_port(const char *port_name, speed_t baud_rate)
{
    if (port_name == NULL) {
        LOG_ERROR("[Serial] port_name is NULL\n");
        return -1;
    }

    int fd = open(port_name, O_RDWR | O_NOCTTY | O_NONBLOCK);

    if (fd < 0) {
        LOG_ERROR("[Serial] Failed to open %s: %s\n",
                  port_name,
                  strerror(errno));
        return -1;
    }

    struct termios tty;

    /*
     * Start from a completely clean configuration.
     *
     * This is preferable for a binary high-speed protocol because
     * we don't inherit terminal settings from the previous owner.
     */
    memset(&tty, 0, sizeof(tty));

    /*
     * 921600, 8N1
     */
    if (cfsetispeed(&tty, baud_rate) != 0 ||
        cfsetospeed(&tty, baud_rate) != 0) {

        LOG_ERROR("[Serial] Failed to set baud rate: %s\n",
                  strerror(errno));

        close(fd);
        return -1;
    }

    /*
     * 8 data bits
     * no parity
     * 1 stop bit
     * receiver enabled
     * ignore modem control lines
     */
    tty.c_cflag = CS8 | CREAD | CLOCAL;

    /*
     * Explicitly disable hardware flow control.
     */
#ifdef CRTSCTS
    tty.c_cflag &= ~CRTSCTS;
#endif

    /*
     * Raw input.
     */
    tty.c_iflag = 0;

    /*
     * Raw output.
     */
    tty.c_oflag = 0;

    /*
     * Non-canonical mode.
     *
     * No echo
     * No signals
     * No line processing
     */
    tty.c_lflag = 0;

    /*
     * read() returns immediately with whatever data is available.
     */
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {

        LOG_ERROR("[Serial] tcsetattr failed: %s\n",
                  strerror(errno));

        close(fd);
        return -1;
    }

    /*
     * Discard anything that might have arrived before
     * the application started.
     */
    if (tcflush(fd, TCIOFLUSH) != 0) {

        LOG_ERROR("[Serial] tcflush failed: %s\n",
                  strerror(errno));

        close(fd);
        return -1;
    }

    /*
     * Verify the configuration.
     */
    struct termios verify;

    if (tcgetattr(fd, &verify) == 0) {

        speed_t actual_speed = cfgetispeed(&verify);

        LOG_INFO("[Serial] Opened %s\n", port_name);
        LOG_INFO("[Serial] Configured baud rate: %lu\n",
                 (unsigned long)actual_speed);
        LOG_INFO("[Serial] Configuration: 8N1, raw, no flow control\n");
    }

    return fd;
}

static void _handle_range_profile(
    const uint8_t *payload,
    uint32_t length)
{
    uint32_t num_elements = length / sizeof(uint16_t);

    if (num_elements > MAX_RANGE_PROFILE_ELEMENTS)
        num_elements = MAX_RANGE_PROFILE_ELEMENTS;

    static uint16_t range_profile[
        MAX_RANGE_PROFILE_ELEMENTS];

    memcpy(
        range_profile,
        payload,
        num_elements * sizeof(uint16_t));

    LOG_DEBUG(
        "[TLV Type 2] Range profile: %u elements\n",
        num_elements);
}

static void _handle_noise_profile(
    const uint8_t *payload,
    uint32_t length)
{
    uint32_t num_elements = length / sizeof(uint16_t);

    if (num_elements > MAX_RANGE_PROFILE_ELEMENTS)
        num_elements = MAX_RANGE_PROFILE_ELEMENTS;

    static uint16_t noise_profile[
        MAX_RANGE_PROFILE_ELEMENTS];

    memcpy(
        noise_profile,
        payload,
        num_elements * sizeof(uint16_t));

    LOG_DEBUG(
        "[TLV Type 3] Noise profile: %u elements\n",
        num_elements);
}

static void _handle_range_doppler_heatmap(
    const uint8_t *payload,
    uint32_t length)
{
    const uint32_t expected_bytes =
        MAX_HEATMAP_ELEMENTS * sizeof(uint16_t);

    if (length != expected_bytes) {

        LOG_WARN(
            "[TLV Type 5] Invalid heatmap size: "
            "%u bytes, expected %u\n",
            length,
            expected_bytes);

        return;
    }

    /*
     * Static buffer avoids allocating a large matrix on the
     * stack for every radar frame.
     */
    static uint16_t heatmap[MAX_HEATMAP_ELEMENTS];

    memcpy(
        heatmap,
        payload,
        expected_bytes);

    /*
     * Do NOT print the entire matrix here.
     *
     * 512 x 16 = 8192 values per frame.
     *
     * Logging this while receiving at 921600 baud can make the
     * application unable to drain the UART fast enough.
     */

    uint16_t min_value = UINT16_MAX;
    uint16_t max_value = 0;

    uint32_t max_index = 0;

    for (uint32_t i = 0;
         i < MAX_HEATMAP_ELEMENTS;
         i++) {

        uint16_t value = heatmap[i];

        if (value < min_value)
            min_value = value;

        if (value > max_value) {
            max_value = value;
            max_index = i;
        }
    }

    uint32_t range_bin =
        max_index / NUM_DOPPLER_BINS;

    uint32_t doppler_bin =
        max_index % NUM_DOPPLER_BINS;

    LOG_DEBUG(
        "[TLV Type 5] Heatmap %ux%u, "
        "min=%u max=%u peak=(range=%u,doppler=%u)\n",
        NUM_RANGE_BINS,
        NUM_DOPPLER_BINS,
        min_value,
        max_value,
        range_bin,
        doppler_bin);
}

static void _parse_radar_tlv(uint32_t type, uint32_t length, const uint8_t *payload) {
    switch (type) {
        case 2:
            _handle_range_profile(payload, length);
            break;
        case 3:
            _handle_noise_profile(payload, length);
            break;
        case 5:
            _handle_range_doppler_heatmap(payload, length);
            break;
        default:
            LOG_WARN("  [TLV Type %u] Found other data layer. Length: %u bytes\n", type, length);
            break;
    }
}

static void _process_radar_frame(const uint8_t *frame_data, size_t size) {
    if (size < sizeof(RadarFrameHeader)) 
        return;

    RadarFrameHeader header;
    memcpy(&header, frame_data, sizeof(RadarFrameHeader));

    if (header.totalPacketLen != size) {
        LOG_WARN("[Radar] Header length %u != supplied size %zu\n",
                 header.totalPacketLen,
                 size);

        return;
    }

    if (header.totalPacketLen < sizeof(RadarFrameHeader)) {
        LOG_WARN("[Radar] Invalid packet length %u\n",
                 header.totalPacketLen);

        return;
    }

    LOG_INFO("\n--- [Radar Frame #%u] ---\n", header.frameNum);
    LOG_INFO(" Total Packet Length: %u bytes\n", header.totalPacketLen);
    LOG_INFO(" Detected Objects   : %u\n", header.numDetectedObj);
    LOG_INFO(" Total TLV Blocks   : %u\n", header.numTLVs);

    size_t offset = sizeof(RadarFrameHeader);

    for (uint32_t i = 0; i < header.numTLVs; i++) {
        if (offset + sizeof(RadarTLVHeader) > header.totalPacketLen) break;

        RadarTLVHeader tlv;
        memcpy(&tlv, frame_data + offset, sizeof(RadarTLVHeader));
        offset += sizeof(RadarTLVHeader);

        if (offset + tlv.length > header.totalPacketLen) {
            LOG_WARN("Warning: TLV structural length overflowed frame bound.\n");
            break;
        }

        _parse_radar_tlv(tlv.type, tlv.length, frame_data + offset);
        offset += tlv.length;

        // TLV ALIGNMENT FIX: TI mmWave SDK structure pads next TLV to 4-byte alignment boundary
        if (offset % 4 != 0) {
            offset += (4 - (offset % 4));
        }
    }
}

void port2_feed(
    uint8_t *accum,
    size_t *accum_len,
    const uint8_t *chunk,
    size_t n)
{
    if (accum == NULL ||
        accum_len == NULL ||
        chunk == NULL ||
        n == 0) {
        return;
    }

    /*
     * Never allow the input chunk to overflow the accumulator.
     */
    if (n > PORT2_ACCUM_SIZE - *accum_len) {

        LOG_WARN("[Port2] Accumulator overflow. "
                 "Dropping current buffered data.\n");

        *accum_len = 0;

        /*
         * If the chunk itself is larger than the complete
         * accumulator, retain only its tail.
         */
        if (n > PORT2_ACCUM_SIZE) {
            chunk += n - PORT2_ACCUM_SIZE;
            n = PORT2_ACCUM_SIZE;
        }
    }

    memcpy(accum + *accum_len, chunk, n);
    *accum_len += n;

    while (*accum_len >= sizeof(RadarFrameHeader)) {

        /*
         * Search for TI magic word.
         */
        size_t magic_idx = SIZE_MAX;

        for (size_t i = 0;
             i + sizeof(RADAR_MAGIC_WORD) <= *accum_len;
             i++) {

            if (memcmp(
                    accum + i,
                    RADAR_MAGIC_WORD,
                    sizeof(RADAR_MAGIC_WORD)) == 0) {

                magic_idx = i;
                break;
            }
        }

        /*
         * Magic word not found.
         *
         * Keep the last 7 bytes because the next read could
         * complete an 8-byte magic word spanning the boundary.
         */
        if (magic_idx == SIZE_MAX) {

            const size_t preserve =
                sizeof(RADAR_MAGIC_WORD) - 1;

            if (*accum_len > preserve) {

                memmove(
                    accum,
                    accum + (*accum_len - preserve),
                    preserve);

                *accum_len = preserve;
            }

            return;
        }

        /*
         * Remove garbage before the magic word.
         */
        if (magic_idx > 0) {

            memmove(
                accum,
                accum + magic_idx,
                *accum_len - magic_idx);

            *accum_len -= magic_idx;
        }

        /*
         * Need complete header.
         */
        if (*accum_len < sizeof(RadarFrameHeader))
            return;

        /*
         * totalPacketLen is at byte offset 12:
         *
         * magic      0..7
         * version    8..11
         * packetLen 12..15
         */
        uint32_t total_packet_len;

        memcpy(
            &total_packet_len,
            accum + 12,
            sizeof(total_packet_len));

        /*
         * Sanity checks.
         */
        if (total_packet_len < sizeof(RadarFrameHeader)) {

            LOG_WARN(
                "[Port2] Invalid packet length: %u\n",
                total_packet_len);

            /*
             * Skip one byte rather than the entire magic word.
             *
             * This allows recovery if a false magic-word match
             * occurred inside corrupted data.
             */
            memmove(
                accum,
                accum + 1,
                *accum_len - 1);

            --(*accum_len);

            continue;
        }

        if (total_packet_len > PORT2_ACCUM_SIZE) {

            LOG_WARN(
                "[Port2] Packet too large: %u bytes\n",
                total_packet_len);

            /*
             * Drop the current synchronization point.
             */
            memmove(
                accum,
                accum + sizeof(RADAR_MAGIC_WORD),
                *accum_len -
                    sizeof(RADAR_MAGIC_WORD));

            *accum_len -= sizeof(RADAR_MAGIC_WORD);

            continue;
        }

        /*
         * Wait for the complete radar packet.
         */
        if (*accum_len < total_packet_len)
            return;

        /*
         * Complete frame available.
         */
        _process_radar_frame(
            accum,
            total_packet_len);

        /*
         * Remove processed frame.
         */
        size_t remaining =
            *accum_len - total_packet_len;

        if (remaining > 0) {

            memmove(
                accum,
                accum + total_packet_len,
                remaining);
        }

        *accum_len = remaining;
    }
}

static void handle_client_command(
    const char *line,
    size_t len,
    int *fd1,
    int *fd2)
{
    if (line == NULL || len == 0)
        return;

    if (strncmp(line, "RESET", 5) == 0) {

        LOG_INFO(
            "[SYSTEM] Received Reset Request! "
            "Purging queues...\n");

        tcflush(*fd1, TCIOFLUSH);
        tcflush(*fd2, TCIOFLUSH);

        send_async_packet(
            PKT_TYPE_SYSTEM,
            "RESET_ACK",
            9);

        return;
    }

    char cmd_buf[CMD_LINE_BUF_SIZE + 1];

    size_t copy_len =
        len < CMD_LINE_BUF_SIZE
            ? len
            : CMD_LINE_BUF_SIZE;

    memcpy(
        cmd_buf,
        line,
        copy_len);

    cmd_buf[copy_len] = '\n';

    size_t total = 0;
    size_t wanted = copy_len + 1;

    while (total < wanted) {

        ssize_t written = write(
            *fd1,
            cmd_buf + total,
            wanted - total);

        if (written > 0) {

            total += (size_t)written;
            continue;
        }

        if (written < 0 &&
            errno == EINTR) {
            continue;
        }

        if (written < 0 &&
            (errno == EAGAIN ||
             errno == EWOULDBLOCK)) {

            /*
             * The serial TX buffer is full.
             * Don't spin forever.
             */
            break;
        }

        LOG_ERROR(
            "[Serial] Failed to write command: %s\n",
            strerror(errno));

        break;
    }
}

void handle_client_data(int *fd1, int *fd2)
{
    uint8_t rx_buffer[BUFFER_SIZE];

    ssize_t n = recv(
        g_client_fd,
        rx_buffer,
        sizeof(rx_buffer),
        0);

    if (n == 0) {
        close_client();
        return;
    }

    if (n < 0) {

        if (errno == EAGAIN ||
            errno == EWOULDBLOCK) {
            return;
        }

        LOG_ERROR("[TCP] recv failed: %s\n",
                  strerror(errno));

        close_client();
        return;
    }

    for (ssize_t i = 0; i < n; i++) {

        uint8_t c = rx_buffer[i];

        if (c == '\n' || c == '\r') {

            if (g_cmd_line_len > 0) {

                handle_client_command(
                    g_cmd_line_buf,
                    g_cmd_line_len,
                    fd1,
                    fd2);

                g_cmd_line_len = 0;
            }

        } else if (
            g_cmd_line_len <
            CMD_LINE_BUF_SIZE - 1) {

            g_cmd_line_buf[g_cmd_line_len++] =
                (char)c;

        } else {

            LOG_WARN(
                "[TCP] Command line too long, "
                "discarding\n");

            g_cmd_line_len = 0;
        }
    }
}

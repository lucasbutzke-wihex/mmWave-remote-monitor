#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <string.h>

#include "tcp_server.h"
#include "serial.h"
#include "common.h"
#include "logger.h"

// TI mmWave Radar Magic Word Constant
static const uint8_t RADAR_MAGIC_WORD[8] = {0x02, 0x01, 0x04, 0x03, 0x06, 0x05, 0x08, 0x07};

int configure_serial_port(const char *port_name, speed_t baud_rate) {
    if (port_name == NULL) {
        LOG_ERROR("[Serial] port_name is NULL");
        return -1;
    }

    int fd = open(port_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    
    if (fd < 0) {
        LOG_ERROR("Error opening serial port");
        return -1;
    }

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        LOG_ERROR("Error from tcgetattr");
        close(fd);
        return -1;
    }

    cfsetospeed(&tty, baud_rate);
    cfsetispeed(&tty, baud_rate);

    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_cflag &= ~CRTSCTS;

    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);
    tty.c_oflag &= ~OPOST;

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        LOG_ERROR("Error from tcsetattr");
        close(fd);
        return -1;
    }
    
    return fd;
}

static void _handle_range_profile(const uint8_t *payload, uint32_t length) {
    uint32_t num_elements = length / 2;
    if (num_elements > MAX_RANGE_PROFILE_ELEMENTS) {
        num_elements = MAX_RANGE_PROFILE_ELEMENTS;
    }
    
    // ARM ALIGNMENT FIX: Copy unaligned serial payload into a safely aligned stack buffer
    uint16_t range_profile[MAX_RANGE_PROFILE_ELEMENTS];
    memcpy(range_profile, payload, num_elements * 2);

    LOG_DEBUG("  [TLV Type 2] Parsed Range Profile. Elements: %u\n", num_elements);

    LOG_DEBUG("Range Bin | ");
    for (uint32_t i = 0; i < num_elements; i++) {
        LOG_DEBUG("%u ", range_profile[i]);
    }
    LOG_DEBUG("\n");
}

static void _handle_range_doppler_heatmap(const uint8_t *payload, uint32_t length) {
    uint32_t total_elements = length / 2;
    if (total_elements != MAX_HEATMAP_ELEMENTS) {
        fprintf(stderr, "  [TLV Type 5] Warning: Heatmap data size (%u elements) mismatched expected size (%d).\n",
                total_elements, MAX_HEATMAP_ELEMENTS);
        return;
    }
    
    // ARM ALIGNMENT FIX: Use aligned static buffer for handling high-throughput matrix bytes safely
    static uint16_t heatmap_flat[MAX_HEATMAP_ELEMENTS];
    memcpy(heatmap_flat, payload, length);

    LOG_DEBUG("  [TLV Type 5] Parsed Range-Doppler Heatmap matrix (%d x %d).\n", NUM_RANGE_BINS, NUM_DOPPLER_BINS);
    for (int r = 0; r < NUM_RANGE_BINS; r++) {
        LOG_DEBUG("RangeDoppler %d | ", r);
        for (int d = 0; d < NUM_DOPPLER_BINS; d++) {
            uint16_t intensity = heatmap_flat[r * NUM_DOPPLER_BINS + d];
            LOG_DEBUG("%u ", intensity);
        }
        LOG_DEBUG("\n");
    }
}

static void _parse_radar_tlv(uint32_t type, uint32_t length, const uint8_t *payload) {
    switch (type) {
        case 2:
            _handle_range_profile(payload, length);
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

void port2_feed(char *accum, size_t *accum_len, const char *chunk, size_t n) {
    if (*accum_len + n >= PORT2_ACCUM_SIZE) {
        LOG_WARN("[Port2] Buffer saturated! Clearing alignment state.\n");
        *accum_len = 0;
        return;
    }

    memcpy(accum + *accum_len, chunk, n);
    *accum_len += n;

    while (*accum_len >= sizeof(RadarFrameHeader)) {
        size_t magic_idx = 0;
        int found_magic = 0;

        for (size_t i = 0; i <= *accum_len - 8; i++) {
            if (memcmp(accum + i, RADAR_MAGIC_WORD, 8) == 0) {
                magic_idx = i;
                found_magic = 1;
                break;
            }
        }

        if (!found_magic) {
            size_t preserve = 7;
            if (*accum_len > preserve) {
                memmove(accum, accum + (*accum_len - preserve), preserve);
                *accum_len = preserve;
            }
            return;
        }

        if (magic_idx > 0) {
            memmove(accum, accum + magic_idx, *accum_len - magic_idx);
            *accum_len -= magic_idx;
        }

        if (*accum_len < sizeof(RadarFrameHeader)) return;

        // ARM ALIGNMENT FIX: Safe extraction of length using memcpy instead of pointer type casting
        uint32_t total_packet_len;
        memcpy(&total_packet_len, accum + 12, sizeof(total_packet_len));

        if (total_packet_len < sizeof(RadarFrameHeader)) {
            LOG_WARN("[Port2] Invalid packet length: %u\n",
                    total_packet_len);

            // Drop the current magic word and search again
            memmove(accum,
                    accum + 8,
                    *accum_len - 8);

            *accum_len -= 8;
            continue;
        }

        if (total_packet_len > PORT2_ACCUM_SIZE) {
            LOG_WARN("[Port2] Packet too large: %u bytes\n",
                    total_packet_len);

            memmove(accum,
                    accum + 8,
                    *accum_len - 8);

            *accum_len -= 8;
            continue;
        }

        if (*accum_len < total_packet_len)
            return;

        _process_radar_frame((uint8_t *)accum, total_packet_len);

        size_t consumed = total_packet_len;
        memmove(accum, accum + consumed, *accum_len - consumed);
        *accum_len -= consumed;
    }
}

static void _handle_client_command(const char *line, size_t len, int fd1, int fd2) {
    if (len == 0) return;

    if (strncmp(line, "RESET", 5) == 0) {
        LOG_INFO("\n[SYSTEM] Received Reset Request! Purging queues...\n");
        tcflush(fd1, TCIOFLUSH);
        tcflush(fd2, TCIOFLUSH);
        send_async_packet(PKT_TYPE_SYSTEM, "RESET_ACK", 9);
    } else {
        char cmd_buf[CMD_LINE_BUF_SIZE + 1];
        size_t copy_len = 
                    (len < CMD_LINE_BUF_SIZE) ? len : CMD_LINE_BUF_SIZE;
        
        memcpy(cmd_buf, line, copy_len);
        cmd_buf[copy_len] = '\n';

        size_t total = 0;
        size_t wanted = copy_len + 1;

        while (total < wanted)
        {
            ssize_t written = write(fd1,
                                    cmd_buf + total,
                                    wanted - total);

            if (written > 0) {
                total += (size_t)written;
                continue;
            }

            if (written < 0 &&
                (errno == EINTR))
                continue;

            if (written < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK))
                break;

            LOG_ERROR("[Serial] Failed to write command: %s",
                    strerror(errno));
            break;
        }
    }
}

void handle_client_data(int fd1, int fd2) {
    char rx_buffer[BUFFER_SIZE];
    ssize_t n = recv(g_client_fd, rx_buffer, sizeof(rx_buffer), 0);

    if (n == 0) {
        close_client();
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        LOG_ERROR("[TCP] recv failed");
        close_client();
        return;
    }

    for (ssize_t i = 0; i < n; i++) {
        char c = rx_buffer[i];
        if (c == '\n' || c == '\r') {
            if (g_cmd_line_len > 0) {
                _handle_client_command(g_cmd_line_buf, g_cmd_line_len, fd1, fd2);
                g_cmd_line_len = 0;
            }
        } else if (g_cmd_line_len < CMD_LINE_BUF_SIZE - 1) {
            g_cmd_line_buf[g_cmd_line_len++] = c;
        } else {
            LOG_WARN("[TCP] Command line too long, discarding.\n");
            g_cmd_line_len = 0;
        }
    }
}

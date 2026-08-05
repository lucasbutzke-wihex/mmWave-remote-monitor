#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>
#include <errno.h>
#include <time.h>

#include "parser.h"
#include "server.h"

static void handle_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

int configure_serial_port(const char *port_name, speed_t baud_rate) {
    int fd = open(port_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror("Error opening serial port");
        return -1;
    }

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        perror("Error from tcgetattr");
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
        perror("Error from tcsetattr");
        close(fd);
        return -1;
    }

    return fd;
}

// ---------------------------------------------------------------------
// Port 2: passive data parsing (called as complete lines arrive)
// ---------------------------------------------------------------------

static void parse_port2_data(const char *frame, size_t len) {
    char tmp[BUFFER_SIZE];
    size_t copy_len = (len < sizeof(tmp) - 1) ? len : sizeof(tmp) - 1;
    memcpy(tmp, frame, copy_len);
    tmp[copy_len] = '\0';
    while (copy_len > 0 && (tmp[copy_len - 1] == '\n' || tmp[copy_len - 1] == '\r')) {
        tmp[--copy_len] = '\0';
    }
    if (copy_len == 0) return;

    printf("[Port2] Parsed: %s\n", tmp);

    // Example of adding real logic:
    // if (strncmp(tmp, "DATA,", 5) == 0) {
    //     int value = atoi(tmp + 5);
    //     printf("[Port2] Value=%d\n", value);
    // }
}

// Process actual Range Profile (TLV Type 2)
static void handle_range_profile(const uint8_t *payload, uint32_t length) {
    uint32_t num_elements = length / 2; // Each uint16_t occupies 2 bytes
    
    if (num_elements > MAX_RANGE_PROFILE_ELEMENTS) {
        num_elements = MAX_RANGE_PROFILE_ELEMENTS;
    }

    // Direct Zero-Copy casting from raw buffer (Replaces np.frombuffer)
    const uint16_t *range_profile = (const uint16_t *)payload;

    printf("  [TLV Type 2] Parsed Range Profile. Elements: %u\n", num_elements);
    for (uint32_t i = 0; i < num_elements; i++) {
        printf("Range Bin %u: %u\n", i, range_profile[i]);
    }
    // Example: Read sample points out of the profile array
    // printf("  First element intensity: %u\n", range_profile[0]);
}

// Process Range-Doppler Heatmap (TLV Type 5)
static void handle_range_doppler_heatmap(const uint8_t *payload, uint32_t length) {
    uint32_t total_elements = length / 2;

    if (total_elements != MAX_HEATMAP_ELEMENTS) {
        fprintf(stderr, "  [TLV Type 5] Warning: Heatmap data size (%u elements) mismatched expected size (%d).\n", 
                total_elements, MAX_HEATMAP_ELEMENTS);
        return;
    }

    // Map raw data onto a typed pointer array
    const uint16_t *heatmap_flat = (const uint16_t *)payload;

    printf("  [TLV Type 5] Parsed Range-Doppler Heatmap matrix (%d x %d).\n", NUM_RANGE_BINS, NUM_DOPPLER_BINS);
    for (int r = 0; r < NUM_RANGE_BINS; r++) {
        printf("Doppler Bin %d: ", r);
        for (int d = 0; d < NUM_DOPPLER_BINS; d++) {
            uint16_t intensity = heatmap_flat[r * NUM_DOPPLER_BINS + d];
            printf("%u ", intensity);
        }
        printf("\n");
    }
    // --- REPLICATING NUMPY 2D RESHAPE ---
    // In C, a flat array matrix maps structurally using index formula: [range * NUM_DOPPLER_BINS + doppler]
    
    // Example layout reading the first 3 indices:
    /*
    for (int r = 0; r < 3; r++) {
        printf("    Range Bin %d -> ", r);
        for (int d = 0; d < NUM_DOPPLER_BINS; d++) {
            uint16_t intensity = heatmap_flat[r * NUM_DOPPLER_BINS + d];
            printf("%u ", intensity);
        }
        printf("\n");
    }
    */
    
    // PLACE YOUR CUSTOM TELEMETRY/TARGET DETECTION ALGORITHMS HERE
    // e.g. run a CFAR detector loop or output a vector over UDP/IPC
}

// Updated main structural dynamic dispatcher hook
static void parse_radar_tlv(uint32_t type, uint32_t length, const uint8_t *payload) {
    switch(type) {
        case 2: // Range Profile
            handle_range_profile(payload, length);
            break;
            
        case 5: // Range-Doppler Heatmap
            handle_range_doppler_heatmap(payload, length);
            break;
            
        default:
            printf("  [TLV Type %u] Found other data layer. Length: %u bytes\n", type, length);
            break;
    }
}

// Full Frame Data integrity parser
static void process_radar_frame(const uint8_t *frame_data, size_t size) {
    if (size < sizeof(RadarFrameHeader)) return;

    RadarFrameHeader header;
    memcpy(&header, frame_data, sizeof(RadarFrameHeader));

    printf("\n--- [Radar Frame #%u] ---\n", header.frameNum);
    printf(" Total Packet Length: %u bytes\n", header.totalPacketLen);
    printf(" Detected Objects   : %u\n", header.numDetectedObj);
    printf(" Total TLV Blocks   : %u\n", header.numTLVs);

    // Skip past the main header to begin reading TLVs
    size_t offset = sizeof(RadarFrameHeader);

    for (uint32_t i = 0; i < header.numTLVs; i++) {
        if (offset + sizeof(RadarTLVHeader) > header.totalPacketLen) break;

        RadarTLVHeader tlv;
        memcpy(&tlv, frame_data + offset, sizeof(RadarTLVHeader));
        offset += sizeof(RadarTLVHeader);

        if (offset + tlv.length > header.totalPacketLen) {
            fprintf(stderr, "Warning: TLV structural length overflowed frame bound.\n");
            break;
        }

        // Pass pointer to this specific TLV payload segment
        parse_radar_tlv(tlv.type, tlv.length, frame_data + offset);
        offset += tlv.length;
    }
}

// Asynchronously aligns raw stream bytes into intact radar packets
static void port2_feed(char *accum, size_t *accum_len, const char *chunk, size_t n) {
    // Prevent memory boundary overflows
    if (*accum_len + n >= PORT2_ACCUM_SIZE) {
        fprintf(stderr, "[Port2] Buffer saturated! Clearing alignment state.\n");
        *accum_len = 0;
        return;
    }

    // Append raw bytes to ring buffer
    memcpy(accum + *accum_len, chunk, n);
    *accum_len += n;

    while (*accum_len >= sizeof(RadarFrameHeader)) {
        // Look for the binary Magic Word layout sequence
        size_t magic_idx = 0;
        int found_magic = 0;

        for (size_t i = 0; i <= *accum_len - 8; i++) {
            if (memcmp(accum + i, RADAR_MAGIC_WORD, 8) == 0) {
                magic_idx = i;
                found_magic = 1;
                break;
            }
        }

        // If no magic word exists, drop unusable bytes except the trailing fragment
        if (!found_magic) {
            size_t preserve = 7; 
            if (*accum_len > preserve) {
                memmove(accum, accum + (*accum_len - preserve), preserve);
                *accum_len = preserve;
            }
            return; 
        }

        // Shift alignment array up to match structural start boundary
        if (magic_idx > 0) {
            memmove(accum, accum + magic_idx, *accum_len - magic_idx);
            *accum_len -= magic_idx;
        }

        if (*accum_len < sizeof(RadarFrameHeader)) return;

        // Read packet header data safely
        uint32_t total_packet_len;
        memcpy(&total_packet_len, accum + 12, sizeof(uint32_t)); // Offset 12: totalPacketLen position

        // Wait until poll() populates the complete expected size sequence
        if (*accum_len < total_packet_len) {
            return; 
        }

        // Dispatch complete valid packet out for data extraction
        process_radar_frame((uint8_t *)accum, total_packet_len);

        // Advance buffer state tracking past the dispatched frame bounds
        size_t consumed = total_packet_len;
        memmove(accum, accum + consumed, *accum_len - consumed);
        *accum_len -= consumed;
    }
}


int main() {
    const char *port1_path = cliPort; // config/command port
    const char *port2_path = dataPort; // passive data listener port
    const char *file_path  = configFile; // file to send to Port 1
    speed_t baud1 = B115200;
    speed_t baud2 = B921600;

    signal(SIGINT, handle_sigint);

    int fd1 = configure_serial_port(port1_path, baud1);
    int fd2 = configure_serial_port(port2_path, baud2);
    int sock_fd = setup_udp_server(UDP_SERVER_PORT);

    if (fd1 < 0 || 
        fd2 < 0 || 
        sock_fd < 0) {
        fprintf(stderr, "Initialization failed.\n");
        return EXIT_FAILURE;
    }

    printf("Async Server Active! Listening for UDP commands on port %d...\n", UDP_SERVER_PORT);

    FILE *file = fopen(file_path, "r");
    if (!file) {
        perror("Error opening input file");
        close(fd1);
        close(fd2);
        close(sock_fd);
        return EXIT_FAILURE;
    }

    // Port 1 state
    port1_state_t state = PORT1_IDLE;
    char response_buffer[BUFFER_SIZE];
    size_t response_len = 0;
    long response_deadline_ms = 0;
    long cooldown_until_ms = 0; // non-blocking replacement for usleep(100ms)

    // Port 2 state
    static char port2_accum[PORT2_ACCUM_SIZE];
    size_t port2_accum_len = 0;

    char line_buffer[BUFFER_SIZE];

    printf("Starting file transmission to %s (Port 2 listening on %s)...\n",
           port1_path, port2_path);

    // Dynamic multiplexer array monitoring 3 independent streams
    struct pollfd fds[3];
    fds[0].fd = fd1;     fds[0].events = POLLIN; // Serial Port 1 (CLI RX)
    fds[1].fd = fd2;     fds[1].events = POLLIN; // Serial Port 2 (Radar Data RX)
    fds[2].fd = sock_fd; fds[2].events = POLLIN; // UDP Socket RX

    char network_buffer[BUFFER_SIZE];

    while (!g_stop) {
        int poll_count = poll(fds, 3, RESPONSE_TIMEOUT_MS);
        if (poll_count < 0) {
            if (errno == EINTR) continue;
            perror("Poll error");
            break;
        }

        // --- STREAM 1: Incoming Network Commands via UDP ---
        if (fds[2].revents & POLLIN) {
            ssize_t n = recvfrom(sock_fd, network_buffer, sizeof(network_buffer) - 1, 0,
                                 (struct sockaddr *)&g_client_addr, &g_client_len);
            if (n > 0) {
                network_buffer[n] = '\0';
                g_client_registered = 1; // Lock onto client address space
                
                printf("[UDP Network Command]: %s", network_buffer);
                
                // Route command directly into CLI hardware port
                int ret = write(fd1, network_buffer, n);
                if (ret < 0) {
                    perror("Failed to write to CLI port");
                }
            }
        }

        // --- Advance Port 1 state machine when idle: send the next line ---
        if (state == PORT1_IDLE && now_ms() >= cooldown_until_ms) {
            if (fgets(line_buffer, sizeof(line_buffer), file) != NULL) {
                printf("\nSending: %s", line_buffer);
                size_t len = strlen(line_buffer);
                ssize_t written = write(fd1, line_buffer, len);
                if (written < 0) {
                    perror("Failed to write to Port 1");
                    break;
                }
                response_len = 0;
                memset(response_buffer, 0, sizeof(response_buffer));
                response_deadline_ms = now_ms() + RESPONSE_TIMEOUT_MS;
                state = PORT1_WAITING_RESPONSE;
                printf("Waiting for response...\n");
            } else {
                state = PORT1_DONE; // File exhausted, but do NOT break!
                printf("\n[Port1] File transmission complete. Continuing to listen on Port 2...\n");
            }
        }

        int timeout_ms = POLL_IDLE_MS;
        if (state == PORT1_WAITING_RESPONSE) {
            long remaining = response_deadline_ms - now_ms();
            if (remaining < 0) remaining = 0;
            timeout_ms = (int)(remaining < POLL_IDLE_MS ? remaining : POLL_IDLE_MS);
        } else if (state == PORT1_IDLE && now_ms() < cooldown_until_ms) {
            long remaining = cooldown_until_ms - now_ms();
            if (remaining < 0) remaining = 0;
            timeout_ms = (int)(remaining < POLL_IDLE_MS ? remaining : POLL_IDLE_MS);
        }

        // --- Port 2: parse whatever arrived, independent of Port 1 state ---
        if (fds[1].revents & POLLIN) {
            char chunk[128];
            ssize_t n = read(fd2, chunk, sizeof(chunk));
            if (n > 0) {
                port2_feed(port2_accum, &port2_accum_len, chunk, (size_t)n);
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("[Port2] Read error");
            }
        }
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "[Port2] Port error/hangup detected.\n");
        }

        // --- Port 1: accumulate response while waiting ---
        if (state == PORT1_WAITING_RESPONSE) {
            if (fds[0].revents & POLLIN) {
                char chunk[64];
                ssize_t n = read(fd1, chunk, sizeof(chunk) - 1);
                if (n > 0) {
                    if (response_len + (size_t)n < sizeof(response_buffer)) {
                        memcpy(response_buffer + response_len, chunk, (size_t)n);
                        response_len += (size_t)n;
                        response_buffer[response_len] = '\0';
                    }

                    // Count the response lines to make sure we match the device's prompt
                    int line_count = 0;
                    for (size_t idx = 0; idx < response_len; idx++) {
                        if (response_buffer[idx] == '\n') line_count++;
                    }

                    // Radar CLI chips return multiple acknowledgment echoes
                    if (line_count >= 2) { 
                        printf("[Port1 Response]:\n%s", response_buffer);
                        state = PORT1_IDLE;
                        cooldown_until_ms = now_ms() + 50; // brief delay
                    }
                }
            }

            // Timeout check
            if (state == PORT1_WAITING_RESPONSE && now_ms() >= response_deadline_ms) {
                fprintf(stderr, "Timeout! No response from Port 1.\n");
                printf("Skipping or retrying due to timeout...\n");
                state = PORT1_IDLE;
                cooldown_until_ms = now_ms() + 100;
            }
        }
    }

    fclose(file);
    close(fd1);
    close(fd2);
    printf("\nFile transfer complete. Ports closed.\n");
    return EXIT_SUCCESS;
}

#ifndef SERIAL_H_
#define SERIAL_H_

#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

/* DEFINE FOR DEBUG PARSERS */
// #define RADAR_DEBUG_PRINT

#define NUM_RANGE_BINS 512
#define NUM_DOPPLER_BINS 16

// Maximum tracking limits for static memory safety allocation
#define MAX_RANGE_PROFILE_ELEMENTS NUM_RANGE_BINS
#define MAX_HEATMAP_ELEMENTS (NUM_RANGE_BINS * NUM_DOPPLER_BINS)

#define BUFFER_SIZE 2048
#define RESPONSE_TIMEOUT_MS 3000   // Max time to wait for a response on Port 1
#define PORT2_ACCUM_SIZE (BUFFER_SIZE * 16)
#define POLL_IDLE_MS 200           // How long poll() may block when Port 1 is idle

#define cliPort "/dev/ttyAMA0" // Port for sending commands/configuration - USB1
#define dataPort "/dev/ttyAMA2" // Port for receiving passive data - USB0
#define configFile "config_heatmap.cfg" // File to send to Port 1

#define PKT_TYPE_CLI_RESP 1
#define PKT_TYPE_RADAR    2
#define PKT_TYPE_SYSTEM   99


// ---------------------------------------------------------------------
// Port 1: config/command state machine (non-blocking)
// ---------------------------------------------------------------------
typedef enum { PORT1_IDLE, PORT1_WAITING_RESPONSE, PORT1_DONE } port1_state_t;
// TI mmWave Radar Magic Word Constant
static const uint8_t RADAR_MAGIC_WORD[8] = {0x02, 0x01, 0x04, 0x03, 0x06, 0x05, 0x08, 0x07};

// Standard TLV Header (Type-Length-Value)
typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t length;
} RadarTLVHeader;

// Standard mmWave Frame Header (Matches Python 'Q8I' pattern)
typedef struct __attribute__((packed)) {
    uint64_t magic;
    uint32_t version;
    uint32_t totalPacketLen;
    uint32_t platform;
    uint32_t frameNum;
    uint32_t timeCPUCycles;
    uint32_t numDetectedObj;
    uint32_t numTLVs;
    uint32_t subFrameNum;
} RadarFrameHeader;

void port2_feed(uint8_t *accum, size_t *accum_len, const uint8_t *chunk, size_t n);
void handle_client_data(int *fd1, int *fd2);
int configure_serial_port(const char *port_name, speed_t baud_rate);

#endif

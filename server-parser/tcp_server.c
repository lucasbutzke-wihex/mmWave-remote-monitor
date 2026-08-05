#include <stdio.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "tcp_server.h"
#include "common.h"
#include "logger.h"

int setup_tcp_listener(int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        LOG_ERROR("socket() failed");
        return -1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in servaddr = {0};
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        LOG_ERROR("bind() failed");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 1) < 0) {
        LOG_ERROR("listen() failed");
        close(listen_fd);
        return -1;
    }

    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL, 0) | O_NONBLOCK);
    return listen_fd;
}

void send_async_packet(uint32_t type, const void *payload, size_t payload_len) {
    if (g_client_fd < 0) return;

    AsyncProtocolHeader header;
    header.packet_type = htonl(type);
    header.sequence_num = htonl(g_tx_sequence++);
    header.payload_len = htonl((uint32_t)payload_len);

    uint8_t tx_buffer[sizeof(AsyncProtocolHeader) + BUFFER_SIZE];
    memcpy(tx_buffer, &header, sizeof(AsyncProtocolHeader));
    if (payload && payload_len > 0) {
        memcpy(tx_buffer + sizeof(AsyncProtocolHeader), payload, payload_len);
    }

    size_t total_len = sizeof(AsyncProtocolHeader) + payload_len;
    size_t sent = 0;

    while (sent < total_len) {
        ssize_t n = send(g_client_fd, tx_buffer + sent, total_len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                LOG_WARN(stderr, "[TCP] Send would block, dropping packet type %u\n", type);
                return;
            }
            LOG_ERROR("[TCP] send failed");
            close_client();
            return;
        }
        sent += (size_t)n;
    }
}

void close_client(void) {
    if (g_client_fd >= 0) {
        close(g_client_fd);
        g_client_fd = -1;
        g_cmd_line_len = 0;
        LOG_WARN("[TCP] Client disconnected.\n");
    }
}

void accept_new_client(int listen_fd) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int new_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
    if (new_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_WARN("accept() failed");
        }
        return;
    }

    if (g_client_fd >= 0) {
        LOG_WARN("[TCP] New client connecting, dropping previous client.\n");
        close_client();
    }

    fcntl(new_fd, F_SETFL, fcntl(new_fd, F_GETFL, 0) | O_NONBLOCK);

    int nodelay = 1;
    setsockopt(new_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    g_client_fd = new_fd;
    g_cmd_line_len = 0;

    LOG_WARN("[TCP] Client connected: %s:%d\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
}

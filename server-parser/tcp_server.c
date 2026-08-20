#include <stdio.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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

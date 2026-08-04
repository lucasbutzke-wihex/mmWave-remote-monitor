#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define UDP_SERVER_PORT 5001

// Global Network Client Tracking State
struct sockaddr_in g_client_addr;
socklen_t g_client_len = sizeof(g_client_addr);
int g_client_registered = 0;

// Helper to transmit data over UDP to our connected client
void forward_to_client(int sock_fd, const void *data, size_t len) {
    if (!g_client_registered) return; // Drop if no client has sent a command yet
    
    ssize_t sent = sendto(sock_fd, data, len, 0, (struct sockaddr *)&g_client_addr, g_client_len);
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("UDP broadcast forwarding failed");
    }
}

int setup_udp_server(int port) {
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        perror("Socket creation failed");
        return -1;
    }

    // Set non-blocking so it integrates cleanly with our poll loop
    int flags = fcntl(sock_fd, F_GETFL, 0);
    fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(port);

    if (bind(sock_fd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("Socket bind failed");
        close(sock_fd);
        return -1;
    }
    return sock_fd;
}

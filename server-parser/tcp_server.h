#ifndef TCP_SERVER_H_
#define TCP_SERVER_H_

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define TCP_SERVER_PORT 5001

extern socklen_t g_client_len;

typedef struct __attribute__((packed)) {
    uint32_t packet_type;
    uint32_t sequence_num;
    uint32_t payload_len;
} AsyncProtocolHeader;

void close_client(void);
int setup_tcp_listener(int port);
void accept_new_client(int listen_fd);
void send_async_packet(uint32_t type, const void *payload, size_t payload_len);

#endif

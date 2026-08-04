#ifndef COMMON_H_
#define COMMON_H_

#include <stdlib.h>
#include <stdio.h>

#define BUFFER_SIZE 2048
#define CMD_LINE_BUF_SIZE 512

extern char g_cmd_line_buf[CMD_LINE_BUF_SIZE];
extern size_t g_cmd_line_len;

extern uint32_t g_tx_sequence;
extern int g_client_fd;

#endif
#ifndef COMMON_H_
#define COMMON_H_

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#define BUFFER_SIZE 2048
#define CMD_LINE_BUF_SIZE 512

extern char g_cmd_line_buf[CMD_LINE_BUF_SIZE];
extern size_t g_cmd_line_len;

extern uint32_t g_tx_sequence;
extern int g_client_fd;

static pthread_mutex_t tx_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t tx_cond =
    PTHREAD_COND_INITIALIZER;

static pthread_mutex_t client_mutex =
    PTHREAD_MUTEX_INITIALIZER;

#endif

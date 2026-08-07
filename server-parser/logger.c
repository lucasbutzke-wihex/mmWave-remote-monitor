#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/syscall.h>

#include "logger.h"

// Struct now uses a flexible array member or a heap pointer for the message
typedef struct LogNode {
    char *message;          // Heap-allocated string for huge messages
    struct LogNode *next;   // Linked list pointer for queue management
    size_t length;          // Track message length
} LogNode;

// Queue pointers for a linked-list-based queue
static LogNode *head = NULL;
static LogNode *tail = NULL;
static size_t queue_count = 0;
static size_t max_queue_size = 1000; // Limit total queued nodes to prevent memory exhaustion

static LogLevel current_level = LOG_DEBUG;
static pthread_t logger_thread;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static bool running = true;
static int fd = -1;
static char logfile[256];
static size_t current_size = 0;
static size_t max_log_size = 0;
static int max_log_backups = 0;

void logger_set_level(LogLevel level)
{
    current_level = level;
}

static const char *level_string(LogLevel level)
{
    switch(level)
    {
        case LOG_TRACE: return "TRACE";
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO ";
        case LOG_WARN:  return "WARN ";
        case LOG_ERROR: return "ERROR";
        case LOG_FATAL: return "FATAL";
    }
    return "UNKNOWN";
}

static void rotate_logs(void)
{
    close(fd);
    char old_name[256];
    char new_name[256];

    for (int i = max_log_backups - 1; i >= 1; i--)
    {
        sprintf(old_name, "%s.%d", logfile, i);
        sprintf(new_name, "%s.%d", logfile, i + 1);
        rename(old_name, new_name);
    }

    sprintf(new_name, "%s.1", logfile);
    rename(logfile, new_name);

    fd = open(logfile, O_CREAT | O_APPEND | O_WRONLY, 0644);
    current_size = 0;
}

static void *logger_worker(void *arg)
{
    (void)arg;

    while (1)
    {
        pthread_mutex_lock(&mutex);

        while (head == NULL && running)
            pthread_cond_wait(&cond, &mutex);

        if (!running && head == NULL)
        {
            pthread_mutex_unlock(&mutex);
            break;
        }

        // Pop from the head of the linked list
        LogNode *node = head;
        head = head->next;
        if (head == NULL)
        {
            tail = NULL;
        }
        queue_count--;

        pthread_mutex_unlock(&mutex);

        // Write to file and free memory
        write(fd, node->message, node->length);
        write(fd, "\n", 1);

        current_size += node->length + 1;

        free(node->message);
        free(node);

        if (current_size >= max_log_size)
            rotate_logs();
    }

    close(fd);
    return NULL;
}

int logger_init(const char *filename, size_t max_size, int backups)
{
    strcpy(logfile, filename);
    max_log_size = max_size;
    max_log_backups = backups;

    fd = open(filename, O_CREAT | O_APPEND | O_WRONLY, 0644);
    if (fd < 0) {
        perror("Failed to open log file");
        return -1;
    }

    struct stat st;
    if (stat(filename, &st) == 0)
        current_size = st.st_size;

    pthread_create(&logger_thread, NULL, logger_worker, NULL);
    return 0;
}

void logger_log(LogLevel level, const char *fmt, ...)
{
    if (level < current_level)
        return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME_COARSE, &ts);

    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    // For large logs ($2^{14}$ = 16384 bytes), allocate dynamically or use a larger local buffer
    size_t alloc_size = 16384; 
    char *buffer = malloc(alloc_size);
    char *final = malloc(alloc_size + 128);
    char timestamp[64];

    if (!buffer || !final) {
        free(buffer);
        free(final);
        return; // Allocation failure guard
    }
    
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, alloc_size, fmt, args);
    va_end(args);

    unsigned long tid = (unsigned long)pthread_self();

    snprintf(timestamp, sizeof(timestamp),
            "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec,
            ts.tv_nsec / 1000000);

    snprintf(final, alloc_size + 128,
         "%s [%s] [TID:%lu] %s",
         timestamp, level_string(level), tid, buffer);

    free(buffer); // Buffer is no longer needed once formatted into 'final'

    size_t final_len = strlen(final);

    // Allocate the queue node
    LogNode *node = malloc(sizeof(LogNode));
    if (!node) {
        free(final);
        return;
    }
    
    node->message = final;
    node->length = final_len;
    node->next = NULL;

    pthread_mutex_lock(&mutex);

    // Optional: Protect against boundless memory growth if queue gets jammed
    if (queue_count >= max_queue_size) {
        pthread_mutex_unlock(&mutex);
        free(node->message);
        free(node);
        return;
    }

    if (tail)
        tail->next = node;
    else
        head = node;

    tail = node;
    queue_count++;

    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);
}

void logger_shutdown()
{
    pthread_mutex_lock(&mutex);
    running = false;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);

    pthread_join(logger_thread, NULL);
}
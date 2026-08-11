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
#include <sys/uio.h>

#include "logger.h"

#define BATCH_SIZE 64 // max batch size per loop
#define MAX_LOG_MESSAGE 4096

// Struct now uses a flexible array member or a heap pointer for the message
typedef struct LogNode {
    struct LogNode *next;           // Linked list pointer for queue management
    size_t length;                  // Track message length
    char message[MAX_LOG_MESSAGE + 128];  // Stack-allocated string for huge messages
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
    if (fd >= 0)
        close(fd);

    if (max_log_backups > 0)
    {
        char old_name[512];
        char new_name[512];

        for (int i = max_log_backups; i >= 1; i--)
        {
            snprintf(old_name, sizeof(old_name),
                     "%s.%d", logfile, i);

            if (i == max_log_backups)
            {
                unlink(old_name);
            }
            else
            {
                snprintf(new_name, sizeof(new_name),
                         "%s.%d", logfile, i + 1);

                rename(old_name, new_name);
            }
        }

        snprintf(new_name, sizeof(new_name),
                 "%s.1", logfile);

        rename(logfile, new_name);
    }

    fd = open(logfile, O_CREAT | O_APPEND | O_WRONLY, 0644);

    if (fd >= 0)
        current_size = 0;
    else
        current_size = 0;
}

static void enqueue_log(LogLevel level, const char *location_prefix, const char *msg) 
{
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);

    char timestamp[64];
    snprintf(timestamp, sizeof(timestamp),
             "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             ts.tv_nsec / 1000000);

    unsigned long tid = (unsigned long)pthread_self();
    char final_str[MAX_LOG_MESSAGE + 128];

    if (location_prefix && location_prefix[0] != '\0') 
    {
        snprintf(final_str, sizeof(final_str),
                 "%s [%s] [TID:%lu] [%s] %s",
                 timestamp, level_string(level), tid, location_prefix, msg);
    } 
    else 
    {
        snprintf(final_str, sizeof(final_str),
                 "%s [%s] [TID:%lu] %s",
                 timestamp, level_string(level), tid, msg);
    }

    LogNode *node = malloc(sizeof(LogNode));

    if (node == NULL) return;

    strncpy(node->message, final_str, sizeof(node->message) - 1);
    node->message[sizeof(node->message) - 1] = '\0';

    node->length = strlen(node->message);
    node->next = NULL;

    pthread_mutex_lock(&mutex);

    if (tail) 
    {
        tail->next = node;
    }
    else
    {
        head = node;
    }

    tail = node;
    queue_count++;

    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);
}

static void *logger_worker(void *arg)
{
    (void)arg;

    struct iovec iov[BATCH_SIZE * 2]; // *2 because each log has a message + a newline string
    LogNode *nodes_to_free[BATCH_SIZE];

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

        // --- BATCH EXTRACTION ---
        size_t count = 0;
        while (head != NULL && count < BATCH_SIZE)
        {
            LogNode *node = head;
            head = head->next;
            queue_count--;

            nodes_to_free[count] = node;

            // Map message buffer
            iov[count * 2].iov_base = node->message;
            iov[count * 2].iov_len = node->length;

            // Map corresponding newline character
            iov[count * 2 + 1].iov_base = (void *)"\n";
            iov[count * 2 + 1].iov_len = 1;

            count++;
        }

        if (head == NULL)
        {
            tail = NULL;
        }

        pthread_mutex_unlock(&mutex);

        // --- SINGLE BATCH WRITE USING writev ---
        // This writes up to BATCH_SIZE messages (double the elements in iov) with ONE system call
        ssize_t total_bytes_written = writev(fd, iov, count * 2);

        if (total_bytes_written > 0)
        {
            current_size += total_bytes_written;
        }

        // --- CLEANUP MEMORY ---
        for (size_t i = 0; i < count; i++)
        {
            free(nodes_to_free[i]);
        }

        // Check log rotation threshold
        if (current_size >= max_log_size)
            rotate_logs();
    }

    close(fd);
    return NULL;
}

int logger_init(const char *filename, size_t max_size, int backups)
{
    snprintf(logfile,
         sizeof(logfile),
         "%s",
         filename);
    
    max_log_size = max_size;
    max_log_backups = backups;

    fd = open(filename, O_CREAT | O_APPEND | O_WRONLY, 0644);
    if (fd < 0) {
        LOG_ERROR("Failed to open file, exiting.\n");
        return 0;
    }

    struct stat st;
    if (stat(filename, &st) == 0)
        current_size = st.st_size;

    if(pthread_create(&logger_thread, NULL, logger_worker, NULL) != 0){
        close(fd);
        return 0;
    }
    
    return 1;
}

void logger_log(LogLevel level, const char *fmt, ...)
{
    if (level < current_level)
        return;

    unsigned long tid = (unsigned long)pthread_self();
        
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME_COARSE, &ts);
    
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
        
    char final[MAX_LOG_MESSAGE + 128];

    int pos = snprintf(
        final,
        sizeof(final),
        "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s] [TID:%lu] ",
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec,
        ts.tv_nsec / 1000000,
        level_string(level),
        tid
    );

    va_list args;
    va_start(args, fmt);
    int msg_len = vsnprintf(final + pos,
                            sizeof(final) - pos,
                            fmt,
                            args);
    va_end(args);

    int total_len = (msg_len < 0) ? pos : pos + msg_len;
    if (total_len >= (int)sizeof(final))
        total_len = sizeof(final) - 1;

    LogNode *node = malloc(sizeof(LogNode));
    if (!node)
        return;

    memcpy(node->message, final, total_len + 1);
    node->length = total_len;
    node->next = NULL;

    pthread_mutex_lock(&mutex);

    // Optional: Protect against boundless memory growth if queue gets jammed
    if (queue_count >= max_queue_size) {
        pthread_mutex_unlock(&mutex);
        // free(node->message);
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

void logger_log_loc(LogLevel level, const char *file, int line, const char *func, const char *fmt, ...) 
{
    (void)func;

    if (level < current_level) return;

    char buffer[MAX_LOG_MESSAGE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    fprintf(stderr, "%s", buffer);
    fflush(stderr);

    char loc[128];
    snprintf(loc, sizeof(loc), "%s:%d", file, line);

    enqueue_log(level, loc, buffer);
}

void logger_shutdown()
{
    pthread_mutex_lock(&mutex);
    running = false;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);

    pthread_join(logger_thread, NULL);
}
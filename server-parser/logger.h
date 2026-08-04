#ifndef LOGGER_H_
#define LOGGER_H_

#include <pthread.h>

#define MAX_PATH 512
#define MAX_MSG 256

// Global synchronization structures
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// Configuration context structure to pass data to worker threads
typedef struct {
    const char *log_file;
    long max_size;
    int max_backups;
} LogConfig;


void write_log(const char *base_path, long max_bytes, int max_backups, const char *message);

#endif

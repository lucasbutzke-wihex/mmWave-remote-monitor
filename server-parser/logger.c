#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>

#include "logger.h"


/**
 * Rotates log files safely. Assumes log_mutex is locked or called sequentially.
 */
int _rotate_logs(const char *base_path, int max_backups) {
    char old_path[MAX_PATH];
    char new_path[MAX_PATH];

    for (int i = max_backups - 1; i >= 1; i--) {
        snprintf(old_path, sizeof(old_path), "%s.%d", base_path, i);
        snprintf(new_path, sizeof(new_path), "%s.%d", base_path, i + 1);

        if (access(old_path, F_OK) == 0) {
            if (rename(old_path, new_path) != 0) {
                perror("Failed to rotate backup log");
                return -1;
            }
        }
    }

    if (access(base_path, F_OK) == 0) {
        snprintf(new_path, sizeof(new_path), "%s.1", base_path);
        if (rename(base_path, new_path) != 0) {
            perror("Failed to rename primary log file");
            return -1;
        }
    }

    FILE *fp = fopen(base_path, "w");
    if (fp == NULL) {
        perror("Failed to create new log file");
        return -1;
    }
    fclose(fp);
    return 0;
}

/**
 * Thread-safe logging function that checks file size, rotates if needed, 
 * and writes the message from any thread.
 */
void write_log(const char *base_path, long max_bytes, int max_backups, const char *message) {
    pthread_mutex_lock(&log_mutex); // Protect critical section

    // 1. Check file size and rotate if threshold is reached
    struct stat st;
    if (stat(base_path, &st) == 0) {
        if (st.st_size >= max_bytes) {
            printf("[System] Log size limit reached. Rotating...\n");
            _rotate_logs(base_path, max_backups);
        }
    }

    // 2. Append the message to the active log file
    FILE *fp = fopen(base_path, "a");
    if (fp != NULL) {
        fprintf(fp, "%s\n", message);
        fclose(fp);
    } else {
        perror("Failed to open log file for writing");
    }

    pthread_mutex_unlock(&log_mutex); // Release lock
}


/**
 * Example Worker Thread function
 */
void *worker_thread_routine(void *arg) {
    LogConfig *config = (LogConfig *)arg;

    for (int i = 1; i <= 50; i++) {
        char buffer[MAX_MSG];
        snprintf(buffer, sizeof(buffer), "Worker thread: processing item %d", i);
        
        // Safely log data from this background thread
        write_log(config->log_file, config->max_size, config->max_backups, buffer);
        
        usleep(1); // Simulate workload
    }

    return NULL;
}

int main(void) {
    pthread_t worker_tid;
    LogConfig config = {
        .log_file = "/tmp/application.log",
        .max_size = 500,       // Small size limit (500 bytes) to trigger quick rotation demo
        .max_backups = 3
    };

    // Initialize clean primary log file
    FILE *fp = fopen(config.log_file, "w");
    if (fp) {
        fprintf(fp, "=== Log Session Started ===\n");
        fclose(fp);
    }

    // Spawn the background worker thread, passing configuration data pointer
    if (pthread_create(&worker_tid, NULL, worker_thread_routine, &config) != 0) {
        perror("Failed to create worker thread");
        return EXIT_FAILURE;
    }

    // Main thread can also log concurrently
    for (int i = 1; i <= 30; i++) {
        char buffer[MAX_MSG];
        snprintf(buffer, sizeof(buffer), "Main thread: heartbeat check %d", i);
        write_log(config.log_file, config.max_size, config.max_backups, buffer);
        usleep(1000);
    }

    // Wait for the worker thread to finish execution
    pthread_join(worker_tid, NULL);

    // Cleanup mutex resources
    pthread_mutex_destroy(&log_mutex);

    printf("Logging demonstration completed successfully.\n");
    return EXIT_SUCCESS;
}

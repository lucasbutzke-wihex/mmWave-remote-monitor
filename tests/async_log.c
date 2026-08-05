#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>

#define MAX_PATH 512
#define MAX_MSG 256
#define QUEUE_SIZE 1024 // Size of the in-memory ring buffer

// Log message structure
typedef struct {
    char data[MAX_MSG];
} LogMessage;

// Thread-safe Ring Buffer Queue
typedef struct {
    LogMessage messages[QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int shutdown;
} LogQueue;

// Global queue and configuration context
static LogQueue log_queue;

typedef struct {
    const char *log_file;
    long max_size;
    int max_backups;
} LogConfig;

// Initialize the queue
void queue_init(LogQueue *q) {
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->shutdown = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

// Destroy the queue
void queue_destroy(LogQueue *q) {
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

/**
 * Non-blocking push for the worker thread. 
 * If the queue is full, it drops the log or handles it gracefully 
 * to ensure execution never stalls.
 */
int queue_push(LogQueue *q, const char *msg) {
    pthread_mutex_lock(&q->mutex);

    if (q->count >= QUEUE_SIZE) {
        // Queue is full. Drop log or handle overflow to prevent blocking critical I/O
        pthread_mutex_unlock(&q->mutex);
        return -1; 
    }

    snprintf(q->messages[q->tail].data, MAX_MSG, "%s", msg);
    q->tail = (q->tail + 1) % QUEUE_SIZE;
    q->count++;

    // Signal the background logger thread that work is available
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

// Pop message for the background logger thread
int queue_pop(LogQueue *q, LogMessage *msg) {
    pthread_mutex_lock(&q->mutex);

    while (q->count == 0 && !q->shutdown) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }

    if (q->count == 0 && q->shutdown) {
        pthread_mutex_unlock(&q->mutex);
        return -1; // Shutdown requested and queue is empty
    }

    *msg = q->messages[q->head];
    q->head = (q->head + 1) % QUEUE_SIZE;
    q->count--;

    pthread_mutex_unlock(&q->mutex);
    return 0;
}

/**
 * Rotates log files safely (executed exclusively by the background thread).
 */
int rotate_logs(const char *base_path, int max_backups) {
    char old_path[MAX_PATH];
    char new_path[MAX_PATH];

    for (int i = max_backups - 1; i >= 1; i--) {
        snprintf(old_path, sizeof(old_path), "%s.%d", base_path, i);
        snprintf(new_path, sizeof(new_path), "%s.%d", base_path, i + 1);

        if (access(old_path, F_OK) == 0) {
            rename(old_path, new_path);
        }
    }

    if (access(base_path, F_OK) == 0) {
        snprintf(new_path, sizeof(new_path), "%s.1", base_path);
        rename(base_path, new_path);
    }

    FILE *fp = fopen(base_path, "w");
    if (fp) fclose(fp);
    return 0;
}

/**
 * Background Logger Thread Routine (Handles all disk I/O and rotation)
 */
void *logger_background_routine(void *arg) {
    LogConfig *config = (LogConfig *)arg;
    LogMessage msg;

    while (1) {
        if (queue_pop(&log_queue, &msg) != 0) {
            break; // Exit loop on shutdown
        }

        // Check file size and rotate if necessary
        struct stat st;
        if (stat(config->log_file, &st) == 0) {
            if (st.st_size >= config->max_size) {
                rotate_logs(config->log_file, config->max_backups);
            }
        }

        // Write log to disk safely in the background
        FILE *fp = fopen(config->log_file, "a");
        if (fp) {
            fprintf(fp, "%s\n", msg.data);
            fclose(fp);
        }
    }

    return NULL;
}

/**
 * Worker Thread: Performs heavy I/O without blocking on disk logging
 */
void *critical_worker_routine(void *arg) {
    for (int i = 1; i <= 10; i++) {
        char log_buf[MAX_MSG];
        snprintf(log_buf, sizeof(log_buf), "Critical worker event ID: %d", i);

        // Non-blocking log submission (copies to memory ring buffer instantly)
        if (queue_push(&log_queue, log_buf) != 0) {
            // Handle queue saturation if needed
        }

        // Simulate heavy, continuous real-time I/O execution
        usleep(100000); // 100ms work loop
    }
    return NULL;
}

int main(void) {
    pthread_t worker_tid, logger_tid;
    LogConfig config = {
        .log_file = "/tmp/async_application.log",
        .max_size = 400,       // Small size for testing rotation
        .max_backups = 3
    };

    queue_init(&log_queue);

    // Initialize initial log file
    FILE *fp = fopen(config.log_file, "w");
    if (fp) {
        fprintf(fp, "=== Asynchronous Log Session Started ===\n");
        fclose(fp);
    }

    // 1. Start the dedicated background logger thread
    pthread_create(&logger_tid, NULL, logger_background_routine, &config);

    // 2. Start the critical worker thread executing heavy I/O
    pthread_create(&worker_tid, NULL, critical_worker_routine, NULL);

    // Wait for the worker thread to finish its execution workload
    pthread_join(worker_tid, NULL);

    // Gracefully shutdown the background logger
    pthread_mutex_lock(&log_queue.mutex);
    log_queue.shutdown = 1;
    pthread_cond_signal(&log_queue.cond);
    pthread_mutex_unlock(&log_queue.mutex);

    pthread_join(logger_tid, NULL);
    queue_destroy(&log_queue);

    printf("Application execution completed seamlessly without I/O log blocking.\n");
    return EXIT_SUCCESS;
}

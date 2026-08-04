#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdatomic.h>

#define CACHE_LINE_SIZE 64
#define QUEUE_SIZE 4096 // Must be a power of 2
#define MAX_MSG 256
#define MAX_PATH 512

// Each slot needs a sequence number to track whether it's ready to write or read
typedef struct {
    atomic_size_t sequence;
    char data[MAX_MSG];
} Cell;

typedef struct {
    Cell buffer[QUEUE_SIZE];
    alignas(CACHE_LINE_SIZE) atomic_size_t enqueue_pos; // Claimed by producers
    alignas(CACHE_LINE_SIZE) atomic_size_t dequeue_pos; // Claimed by consumers
} MPMCQueue;

static MPMCQueue log_queue;

typedef struct {
    const char *log_file;
    long max_size;
    int max_backups;
    volatile int running;
} LogConfig;

// Initialize queue cells with their respective sequence numbers
void mpmc_init(MPMCQueue *q) {
    atomic_init(&q->enqueue_pos, 0);
    atomic_init(&q->dequeue_pos, 0);
    for (size_t i = 0; i < QUEUE_SIZE; i++) {
        atomic_init(&q->buffer[i].sequence, i);
    }
}

/**
 * Non-blocking push used by MULTIPLE worker threads safely.
 */
int mpmc_push(MPMCQueue *q, const char *msg) {
    Cell *cell;
    size_t pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);

    for (;;) {
        cell = &q->buffer[pos & (QUEUE_SIZE - 1)];
        size_t seq = atomic_load_explicit(&cell->sequence, memory_order_acquire);
        intptr_t dif = (intptr_t)seq - (intptr_t)pos;

        if (dif == 0) {
            // Slot is free, try to claim it atomically
            if (atomic_compare_exchange_weak_explicit(&q->enqueue_pos, &pos, pos + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed)) {
                break;
            }
        } else if (dif < 0) {
            // Queue is full, drop or yield to prevent blocking critical path
            return -1;
        } else {
            pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
        }
    }

    // Copy data into the claimed cell slot
    snprintf(cell->data, MAX_MSG, "%s", msg);

    // Publish the slot as ready for consumption
    atomic_store_explicit(&cell->sequence, pos + 1, memory_order_release);
    return 0;
}

/**
 * Pop used by MULTIPLE background consumer threads.
 */
int mpmc_pop(MPMCQueue *q, char *dest_msg) {
    Cell *cell;
    size_t pos = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);

    for (;;) {
        cell = &q->buffer[pos & (QUEUE_SIZE - 1)];
        size_t seq = atomic_load_explicit(&cell->sequence, memory_order_acquire);
        intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);

        if (dif == 0) {
            // Slot has data, try to claim it
            if (atomic_compare_exchange_weak_explicit(&q->dequeue_pos, &pos, pos + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed)) {
                break;
            }
        } else if (dif < 0) {
            // Queue is empty
            return -1;
        } else {
            pos = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);
        }
    }

    // Copy out message data
    snprintf(dest_msg, MAX_MSG, "%s", cell->data);

    // Reset sequence for the next wrap-around cycle
    atomic_store_explicit(&cell->sequence, pos + QUEUE_SIZE, memory_order_release);
    return 0;
}

// Log rotation routine
void rotate_logs(const char *base_path, int max_backups) {
    char old_path[MAX_PATH], new_path[MAX_PATH];
    for (int i = max_backups - 1; i >= 1; i--) {
        snprintf(old_path, sizeof(old_path), "%s.%d", base_path, i);
        snprintf(new_path, sizeof(new_path), "%s.%d", base_path, i + 1);
        if (access(old_path, F_OK) == 0) rename(old_path, new_path);
    }
    if (access(base_path, F_OK) == 0) {
        snprintf(new_path, sizeof(new_path), "%s.1", base_path);
        rename(base_path, new_path);
    }
    FILE *fp = fopen(base_path, "w");
    if (fp) fclose(fp);
}

/**
 * Background Consumer Thread Routine (Multiple instances can run safely)
 */
void *multi_consumer_logger(void *arg) {
    LogConfig *config = (LogConfig *)arg;
    char msg[MAX_MSG];
    int batch_count = 0;

    FILE *fp = fopen(config->log_file, "a");

    while (config->running || mpmc_pop(&log_queue, msg) == 0) {
        if (mpmc_pop(&log_queue, msg) != 0) {
            usleep(500); // Sleep briefly if queue is empty
            continue;
        }

        if (!fp) fp = fopen(config->log_file, "a");
        if (fp) {
            fprintf(fp, "%s\n", msg);
            batch_count++;

            // Periodically check size and rotate safely
            if (batch_count >= 64) {
                fflush(fp);
                batch_count = 0;

                struct stat st;
                if (stat(config->log_file, &st) == 0 && st.st_size >= config->max_size) {
                    fclose(fp);
                    rotate_logs(config->log_file, config->max_backups);
                    fp = fopen(config->log_file, "a");
                }
            }
        }
    }

    if (fp) fclose(fp);
    return NULL;
}

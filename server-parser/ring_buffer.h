#ifndef RING_BUFFER_H_
#define RING_BUFFER_H_

#include <pthread.h>
#include <stdint.h>

#define RADAR_RING_SIZE (1024 * 1024)

typedef struct {
    uint8_t *data;
    size_t size;

    size_t head;
    size_t tail;

    pthread_mutex_t mutex;
    pthread_cond_t data_available;

    bool stop;
} RadarRingBuffer;


#endif

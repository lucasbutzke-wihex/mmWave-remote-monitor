#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>

#define BUFFER_CAPACITY 10

typedef struct {
    void    **items;                // array of pointers to items
    size_t  capacity;
    size_t  head;                // next slot to read
    size_t  tail;               // next slot to write
    size_t  count;              // number of itens stored

    pthread_mutex_t lock;
    pthread_cond_t  not_empty;  // signaled when 
    pthread_cond_t  not_full;
    bool            shutdown;
} CircularBuffer;

int cb_init(CircularBuffer *cb, size_t capacity){
    cb->items = calloc(capacity, sizeof(void *));
    if (!cb->items) return -1;
    
    cb->capacity = capacity;
    cb->head = cb->tail = cb->count = 0;
    cb->shutdown = false;

    pthread_mutex_init(&cb->lock, NULL);
    pthread_cond_init(&cb->not_empty, NULL);
    pthread_cond_init(&cb->not_full, NULL);
    return 0;
}

int cb_push(CircularBuffer *cb, void *item){
    pthread_mutex_lock(&cb->lock);

    while (cb->count == cb->capacity && !cb->shutdown){
        pthread_cond_wait(&cb->not_full, &cb->lock);
    }

    if (cb->shutdown){
        pthread_mutex_unlock(&cb->lock);
        return -1;
    }

    cb->items[cb->tail] = item;
    cb->tail = (cb->tail +1) % cb->capacity;
    cb->count++;

    pthread_cond_signal(&cb->not_empty);
    pthread_mutex_unlock(&cb->lock);
    return 0;
}

void *cb_pop(CircularBuffer *cb){
    pthread_mutex_lock(&cb->lock);

    while(cb->count == 0 && !cb->shutdown){
        pthread_cond_wait(&cb->not_empty, &cb->lock);
    }

    if (cb->count == 0 && cb->shutdown){
        pthread_mutex_unlock(&cb->lock);
        return NULL;
    }

    void *item = cb->items[cb->head];
    cb->head = (cb->head + 1) % cb->capacity;
    cb->count--;

    pthread_cond_signal(&cb->not_full);
    pthread_mutex_unlock(&cb->lock);
    return item;
}

int cb_try_push(CircularBuffer *cb, void *item){
    pthread_mutex_lock(&cb->lock);

    if (cb->count == cb->capacity){
        pthread_mutex_unlock(&cb->lock);
        return -1;
    }

    cb->items[cb->tail] = item;
    cb->tail = (cb->tail +1) % cb->capacity;
    cb->count++;

    pthread_cond_signal(&cb->not_empty);
    pthread_mutex_unlock(&cb->lock);
    return 0;
}

int cb_try_pop(CircularBuffer *cb, void **out_item) {
    pthread_mutex_lock(&cb->lock);
    if (cb->count == 0) {
        pthread_mutex_unlock(&cb->lock);
        return -1;   // now -1 is a legitimate int status, no pointer confusion
    }
    *out_item = cb->items[cb->head];
    cb->head = (cb->head + 1) % cb->capacity;
    cb->count--;
    pthread_cond_signal(&cb->not_full);
    pthread_mutex_unlock(&cb->lock);
    return 0;
}

void cb_shutdown(CircularBuffer *cb){
    pthread_mutex_lock(&cb->lock);
    cb->shutdown = true;
    pthread_cond_broadcast(&cb->not_empty);
    pthread_cond_broadcast(&cb->not_full);
    pthread_mutex_unlock(&cb->lock);
}

void cb_destroy(CircularBuffer *cb){
    pthread_mutex_destroy(&cb->lock);
    pthread_cond_destroy(&cb->not_empty);
    pthread_cond_destroy(&cb->not_full);
    free(cb->items);
}


static CircularBuffer buffer;

void *producer(void *arg) {
    int id = *(int *)arg;
    for (int i = 0; i < 5; i++) {
        int *val = malloc(sizeof(int));
        *val = id * 100 + i;
        printf("Producer %d pushing %d\n", id, *val);
        cb_try_push(&buffer, val);
    }
    return NULL;
}

void *consumer(void *arg) {
    int *val;
    for (int i = 0; i < 5; i++) {
        cb_try_pop(&buffer, val);
        if (val) {
            printf("Consumer got %d\n", *val);
            free(val);
        }
    }
    return NULL;
}


int main(void){

    printf("%u \n", (1024*1024*1024 * 4) - 1 );
    printf("%u \n", (2 << 32) - 1 );

    printf("%x \n", (1 << 64) - 1);
    printf("%x \n", sizeof(void *));


    cb_init(&buffer, BUFFER_CAPACITY);

    pthread_t producers[2], consumers[2];
    int ids[2] = {1, 2};

    // for (int i = 0; i < 2; i++)
    pthread_create(&producers[0], NULL, producer, &ids[0]);
    // for (int i = 0; i < 2; i++)
    pthread_create(&consumers[0], NULL, consumer, NULL);

    // for (int i = 0; i < 2; i++) 
    pthread_join(producers[0], NULL);
    // for (int i = 0; i < 2; i++) 
    pthread_join(consumers[0], NULL);

    cb_destroy(&buffer);

    return 0;
}

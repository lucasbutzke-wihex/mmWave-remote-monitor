#ifndef WDT_H
#define WDT_H

#include <pthread.h>

typedef struct {
    double timeout;
    double last_heartbeat;
    int running;
    pthread_mutex_t lock;
    pthread_t thread_id;
    unsigned int gpio_offset;
} RadarWatchdog;


int watchdog_start(RadarWatchdog *wdt, unsigned int gpio_offset, double timeout_val);
void watchdog_feed(RadarWatchdog *wdt);
void watchdog_stop(RadarWatchdog *wdt);

#endif // WDT_H
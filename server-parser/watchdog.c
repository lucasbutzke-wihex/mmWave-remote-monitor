#define _DEFAULT_SOURCE
#include <unistd.h>
#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>

#include "watchdog.h"
#include "logger.h"
#include "pin-control.h"


void watchdog_feed(RadarWatchdog *wdt) //grava tempo da ultima comunicação
{
    pthread_mutex_lock(&wdt->lock);
    wdt->last_heartbeat = get_current_time();
    pthread_mutex_unlock(&wdt->lock);
}

void _watchdog_force_reset(unsigned int offset) 
{
    LOG_WARN("[WATCHDOG] !!! Alerta: sem comunicação. Resetando radar !!!\n");
    
    gpio_init_pin(offset);
    
    LOG_INFO("[WATCHDOG] reset enviado com sucesso\n");
}

void* _watchdog_monitor(void *arg) 
{
    RadarWatchdog *wdt = (RadarWatchdog*)arg;
    
    while (wdt->running) 
    {
        usleep(100000); // Checa o status a cada 100ms
        
        pthread_mutex_lock(&wdt->lock);
        double time_since_last_feed = get_current_time() - wdt->last_heartbeat;
        pthread_mutex_unlock(&wdt->lock);
        
        if (time_since_last_feed > wdt->timeout) 
        {
            _watchdog_force_reset(wdt->gpio_offset);
            
            pthread_mutex_lock(&wdt->lock);
            wdt->last_heartbeat = get_current_time() + 3.0; //3s ate proximo reset ser possivel
            pthread_mutex_unlock(&wdt->lock);
        }
    }

    return NULL;
}

int watchdog_start(RadarWatchdog *wdt, unsigned int gpio_offset, double timeout_val) {
    wdt->gpio_offset = gpio_offset;

    gpio_init_pin(gpio_offset);

    // Inicializa a estrutura
    wdt->timeout = timeout_val;
    wdt->last_heartbeat = get_current_time();
    wdt->running = 1;
    pthread_mutex_init(&wdt->lock, NULL);
    
    // Cria a thread de background
    pthread_create(&wdt->thread_id, NULL, _watchdog_monitor, (void*)wdt);

    return 1;
}

void watchdog_stop(RadarWatchdog *wdt) 
{
    wdt->running = 0;
    pthread_join(wdt->thread_id, NULL); // espera a thread finalizar
    pthread_mutex_destroy(&wdt->lock);
    gpiod_line_request_release(request);
    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);
    gpiod_chip_close(chip);
    LOG_WARN("[WATCHDOG] Monitor parado\n");
}

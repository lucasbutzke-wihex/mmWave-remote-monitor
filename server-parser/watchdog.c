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

static struct gpiod_chip *chip = NULL;
static struct gpiod_line_settings *settings = NULL;
static struct gpiod_line_config *line_cfg = NULL;
static struct gpiod_request_config *req_cfg = NULL;
static struct gpiod_line_request *request = NULL;

void _gpio_export()
{
    chip = gpiod_chip_open("/dev/gpiochip4");
    if (!chip) {
        chip = gpiod_chip_open("/dev/gpiochip0");
        if (!chip) {
            LOG_ERROR("[WATCHDOG] Erro ao abrir chip de GPIO");
            return;
        }
    }
}

void _gpio_config(unsigned int offset)
{
    if (!chip) {
        return; 
    }
    
    settings = gpiod_line_settings_new();
    if (!settings) {
        LOG_ERROR("[WATCHDOG] Erro ao criar configurações");
        return;
    }

    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT); // define como saida
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_ACTIVE); // define nivel logico alto

    line_cfg = gpiod_line_config_new();
    if (!line_cfg) {
        LOG_ERROR("[WATCHDOG] Erro ao criar configuração da linha");
        gpiod_line_settings_free(settings);
        return;
    }

    if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings) < 0) {
        LOG_ERROR("[WATCHDOG] Erro ao adicionar configurações da linha");
        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);
        return;
    }

    //offset de pino
    req_cfg = gpiod_request_config_new();
    if (!req_cfg) {
         LOG_ERROR("[WATCHDOG] Erro ao criar request config");
         gpiod_line_config_free(line_cfg);
         gpiod_line_settings_free(settings);
         return;
    }
    gpiod_request_config_set_consumer(req_cfg, "RadarWatchdog");

    // solicita o controle do pino
    request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    if (!request) {
        LOG_ERROR("[WATCHDOG] Erro ao requisitar linha GPIO");
        gpiod_request_config_free(req_cfg);
        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);
        return;
    }
}

void _gpio_write(unsigned int offset, enum gpiod_line_value value) 
{
    if (request) {
        gpiod_line_request_set_value(request, offset, value);
    }
}

double _get_current_time() // retorna tempo atual (s)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

void watchdog_feed(RadarWatchdog *wdt) //grava tempo da ultima comunicação
{
    pthread_mutex_lock(&wdt->lock);
    wdt->last_heartbeat = _get_current_time();
    pthread_mutex_unlock(&wdt->lock);
}

void _watchdog_force_reset(unsigned int offset) 
{
    LOG_WARN("[WATCHDOG] !!! Alerta: sem comunicação. Resetando radar !!!\n");
    
    _gpio_write(offset, GPIOD_LINE_VALUE_INACTIVE);  // nRESET em LOW  -> Ativa Reset
    usleep(100000);               // Espera 100ms
    _gpio_write(offset, GPIOD_LINE_VALUE_ACTIVE); // nRESET em HIGH -> Libera Radar
    
    LOG_INFO("[WATCHDOG] reset enviado com sucesso\n");
}

void* _watchdog_monitor(void *arg) 
{
    RadarWatchdog *wdt = (RadarWatchdog*)arg;
    
    while (wdt->running) 
    {
        usleep(100000); // Checa o status a cada 100ms
        
        pthread_mutex_lock(&wdt->lock);
        double time_since_last_feed = _get_current_time() - wdt->last_heartbeat;
        pthread_mutex_unlock(&wdt->lock);
        
        if (time_since_last_feed > wdt->timeout) 
        {
            _watchdog_force_reset(wdt->gpio_offset);
            
            pthread_mutex_lock(&wdt->lock);
            wdt->last_heartbeat = _get_current_time() + 3.0; //3s ate proximo reset ser possivel
            pthread_mutex_unlock(&wdt->lock);
        }
    }

    return NULL;
}

void watchdog_start(RadarWatchdog *wdt, unsigned int gpio_offset, double timeout_val) {
    wdt->gpio_offset = gpio_offset;

    _gpio_export();
    usleep(50000); 
    _gpio_config(gpio_offset);

    // Inicializa a estrutura
    wdt->timeout = timeout_val;
    wdt->last_heartbeat = _get_current_time();
    wdt->running = 1;
    pthread_mutex_init(&wdt->lock, NULL);
    
    // Cria a thread de background
    pthread_create(&wdt->thread_id, NULL, _watchdog_monitor, (void*)wdt);
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

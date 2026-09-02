#include "pin-control.h"

void gpio_init_pin(int pin)
{
    _gpio_export();
    _gpio_config(pin);
}

void gpio_write(unsigned int offset, enum gpiod_line_value value) 
{
    if (request) {
        gpiod_line_request_set_value(request, offset, value);
    }
}

double get_current_time() // retorna tempo atual (s)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

void _gpio_export()
{
    chip = gpiod_chip_open("/dev/gpiochip4");
    if (!chip) {
        chip = gpiod_chip_open("/dev/gpiochip0");
        if (!chip) {
            LOG_ERROR("[LED TOGGLE] Erro ao abrir chip de GPIO");
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
        LOG_ERROR("[LED TOGGLE] Erro ao criar configurações");
        return;
    }

    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT); // define como saida
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_ACTIVE); // define nivel logico alto

    line_cfg = gpiod_line_config_new();
    if (!line_cfg) {
        LOG_ERROR("[LED TOGGLE] Erro ao criar configuração da linha");
        gpiod_line_settings_free(settings);
        return;
    }

    if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings) < 0) {
        LOG_ERROR("[LED TOGGLE] Erro ao adicionar configurações da linha");
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
    gpiod_request_config_set_consumer(req_cfg, "ToggleLED");

    // solicita o controle do pino
    request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    if (!request) {
        LOG_ERROR("[LED TOGGLE] Erro ao requisitar linha GPIO");
        gpiod_request_config_free(req_cfg);
        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);
        return;
    }
}
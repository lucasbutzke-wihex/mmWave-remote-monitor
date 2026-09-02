#ifndef PIN_CTRL
#define PIN_CTRL

#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <gpiod.h>
#include <errno.h>

#include "logger.h"

static struct gpiod_chip *chip = NULL;
static struct gpiod_line_settings *settings = NULL;
static struct gpiod_line_config *line_cfg = NULL;
static struct gpiod_request_config *req_cfg = NULL;
static struct gpiod_line_request *request = NULL;

void gpio_init_pin(int pin);

void gpio_write(unsigned int offset, enum gpiod_line_value value);

double get_current_time();

void _gpio_export();

void _gpio_config(unsigned int offset);

#endif // PIN_CTRL
#ifndef LOGGER_H_
#define LOGGER_H_

#include <stddef.h>

typedef enum
{
    LOG_TRACE = 0,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL

} LogLevel;

int logger_init(
    const char *filename,
    size_t max_size,
    int max_backups
);

void logger_log(LogLevel level, const char *fmt, ...);

void logger_shutdown(void);

#endif

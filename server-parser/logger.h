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

void logger_log_loc(LogLevel level, const char *file, int line, const char *func, const char *fmt, ...); //para erros com localização

void logger_shutdown(void);


#define LOG_TRACE(...) logger_log(LOG_TRACE, __VA_ARGS__)

#define LOG_DEBUG(...) logger_log(LOG_DEBUG, __VA_ARGS__)

#define LOG_INFO(...) logger_log(LOG_INFO, __VA_ARGS__)

#define LOG_WARN(...) logger_log(LOG_WARN, __VA_ARGS__)

#define LOG_ERROR(...) logger_log_loc(LOG_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define LOG_FATAL(...) logger_log_loc(LOG_FATAL, __FILE__, __LINE__, __func__, __VA_ARGS__)


#endif

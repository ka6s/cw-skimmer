#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <time.h>

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3
} log_level_t;

/**
 * Initialize logging system
 * @param level Minimum log level to display
 * @param log_file Output file path, or NULL for stdout
 */
void logger_init(log_level_t level, const char *log_file);

/**
 * Close logging system
 */
void logger_close(void);

/**
 * Set current log level
 */
void logger_set_level(log_level_t level);

/**
 * Log a message
 */
void log_msg(log_level_t level, const char *format, ...);

#define LOG_DEBUG(fmt, ...) log_msg(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  log_msg(LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  log_msg(LOG_WARN, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_msg(LOG_ERROR, fmt, ##__VA_ARGS__)

#endif

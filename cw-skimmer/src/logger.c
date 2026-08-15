#include "logger.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static log_level_t current_level = LOG_INFO;
static FILE *log_file = NULL;
static const char *level_names[] = {"DEBUG", "INFO", "WARN", "ERROR"};

void logger_init(log_level_t level, const char *log_path) {
    current_level = level;
    if (log_path) {
        log_file = fopen(log_path, "a");
        if (!log_file) {
            fprintf(stderr, "Failed to open log file: %s\n", log_path);
            log_file = stdout;
        }
    } else {
        log_file = stdout;
    }
    LOG_INFO("Logger initialized at level %s", level_names[level]);
}

void logger_close(void) {
    if (log_file && log_file != stdout) {
        fclose(log_file);
    }
    log_file = NULL;
}

void logger_set_level(log_level_t level) {
    current_level = level;
}

void log_msg(log_level_t level, const char *format, ...) {
    if (level < current_level) {
        return;
    }

    if (!log_file) {
        log_file = stdout;
    }

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    fprintf(log_file, "[%s] %s: ", timestamp, level_names[level]);

    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);

    fprintf(log_file, "\n");
    fflush(log_file);
}

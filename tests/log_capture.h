/**
 * @file log_capture.h
 * @brief Capture the logger's output (stderr) in a test.
 *
 * log_capture_begin() points fd 2 at a temporary file and starts the
 * logger. log_capture_end() stops the logger, which writes every queued
 * line first, restores fd 2 and returns the captured text.
 */

#ifndef WSA_LOG_CAPTURE_H
#define WSA_LOG_CAPTURE_H

#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOG_CAPTURE_MAX (256 * 1024)

static FILE *log_capture_file_;
static int   log_capture_saved_fd_ = -1;
static char  log_capture_buf_[LOG_CAPTURE_MAX];

/** Redirect stderr to a temporary file, then logger_init(). */
static inline void log_capture_begin(void)
{
    fflush(stderr);
    log_capture_file_ = tmpfile();
    log_capture_saved_fd_ = dup(STDERR_FILENO);
    if (!log_capture_file_ || log_capture_saved_fd_ < 0 ||
        dup2(fileno(log_capture_file_), STDERR_FILENO) < 0) {
        perror("log_capture_begin");
        return;
    }
    (void)logger_init();
}

/** logger_shutdown(), restore stderr, return the captured text. */
static inline const char *log_capture_end(void)
{
    size_t n = 0;

    logger_shutdown();
    fflush(stderr);
    if (log_capture_saved_fd_ >= 0) {
        dup2(log_capture_saved_fd_, STDERR_FILENO);
        close(log_capture_saved_fd_);
        log_capture_saved_fd_ = -1;
    }
    if (log_capture_file_) {
        rewind(log_capture_file_);
        n = fread(log_capture_buf_, 1, sizeof(log_capture_buf_) - 1,
                  log_capture_file_);
        fclose(log_capture_file_);
        log_capture_file_ = NULL;
    }
    log_capture_buf_[n] = '\0';
    return log_capture_buf_;
}

/** @return 1 if @p needle occurs in @p log. */
static inline int log_contains(const char *log, const char *needle)
{
    return log && needle && strstr(log, needle) != NULL;
}

#endif /* WSA_LOG_CAPTURE_H */

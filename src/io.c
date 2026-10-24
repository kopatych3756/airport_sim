#include "airport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

static int quiet_mode = 0;
static int log_fd = -1;
static int failed = 0;
int io_failed(void) { return failed; }

static void write_all(int fd, const char *text, size_t length) {
    while (length > 0) {
        ssize_t written = write(fd, text, length);

        if (written < 0) {
            if (errno == EINTR) continue;
            failed = 1;
            return;
        }

        if (written == 0) { failed = 1; return; }
        text += written;
        length -= (size_t)written;
    }
}

void set_quiet_mode(int quiet) {
    quiet_mode = quiet;
}

int open_log_file(const char *path) {
    log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0) {
        perror(path);
        return -1;
    }
    return 0;
}

void close_log_file(void) {
    if (log_fd >= 0) {
        if (close(log_fd)) failed = 1;
        log_fd = -1;
    }
}

void print_line(const char *format, ...) {
    char buffer[1024];
    va_list args;
    int length;

    va_start(args, format);
    length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (length < 0) return;
    if (length >= (int)sizeof(buffer)) length = (int)sizeof(buffer) - 1;

    if (!quiet_mode) {
        write_all(STDOUT_FILENO, buffer, (size_t)length);
        write_all(STDOUT_FILENO, "\n", 1);
    }

    if (log_fd >= 0) {
        write_all(log_fd, buffer, (size_t)length);
        write_all(log_fd, "\n", 1);
    }
}

void event(const char *name, const char *format, ...) {
    char message[800];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    print_line("[T=%04d] [%-9s] %s", current_tick, name, message);
}

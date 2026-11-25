#include "airport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

static char *capture;
static size_t capture_size, capture_used;

void capture_events(char *buffer, size_t size) {
    capture = buffer;
    capture_size = size;
    capture_used = 0;
    if (buffer && size) buffer[0] = 0;
}

static OutputListener output_listener;

void set_output_listener(OutputListener listener) {
    output_listener = listener;
}

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

        if (written == 0) {
            failed = 1;
            return;
        }
        text += written;
        length -= (size_t) written;
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

static void output(const char *event_name, int plane, int runway, const char *format, va_list args) {
    char buffer[1024];
    int length = vsnprintf(buffer, sizeof(buffer), format, args);

    if (length < 0) return;
    if (length >= (int) sizeof(buffer)) length = (int) sizeof(buffer) - 1;

    if (capture) {
        if (capture_used + (size_t) length + 2 >= capture_size) _exit(5);
        memcpy(capture + capture_used, buffer, (size_t) length);
        capture_used += (size_t) length;
        capture[capture_used++] = '\n';
        capture[capture_used] = 0;
        return;
    }
    if (output_listener) output_listener(event_name, plane, runway, buffer);
    if (!quiet_mode) {
        write_all(STDOUT_FILENO, buffer, (size_t) length);
        write_all(STDOUT_FILENO, "\n", 1);
    }

    if (log_fd >= 0) {
        write_all(log_fd, buffer, (size_t) length);
        write_all(log_fd, "\n", 1);
    }
}

void print_line(const char *format, ...) {
    va_list args;
    va_start(args, format);
    output(NULL, -1, -1, format, args);
    va_end(args);
}

static void output_event(const char *name, int plane, int runway, const char *format, ...) {
    va_list args;
    va_start(args, format);
    output(name, plane, runway, format, args);
    va_end(args);
}

static void format_event(const char *name, int plane, int runway, const char *format, va_list args) {
    char message[800];
    vsnprintf(message, sizeof(message), format, args);
    output_event(name, plane, runway, "[T=%04d] [%-9s] %s", airport->current_tick, name, message);
}

void actor_event(int plane, int runway, const char *name, const char *format, ...) {
    va_list args;
    va_start(args, format);
    format_event(name, plane, runway, format, args);
    va_end(args);
}

void event(const char *name, const char *format, ...) {
    va_list args;
    va_start(args, format);
    format_event(name, -1, -1, format, args);
    va_end(args);
}

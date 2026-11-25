#define _GNU_SOURCE
#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t interrupted;

static void interrupt_handler(int signal_number) {
    (void)signal_number;
    interrupted = 1;
}

int setup_signals(void) {
    struct sigaction action = {.sa_handler = interrupt_handler};
    sigemptyset(&action.sa_mask);
    return sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL) ||
           signal(SIGPIPE, SIG_IGN) == SIG_ERR;
}

const char *role_name(enum Role role) {
    static const char *names[] = {"NONE", "AIRCRAFT", "RUNWAY", "WATCH", "CONTROL"};
    return names[role];
}

enum Role parse_role(const char *name) {
    for (enum Role role = AIRCRAFT; role <= CONTROL; ++role)
        if (!strcmp(name, role_name(role))) return role;
    return NONE;
}

int valid_token(const char *token, size_t limit) {
    if (!*token || strlen(token) > limit) return 0;
    for (const unsigned char *p = (const unsigned char *)token; *p; ++p)
        if (*p <= 32 || *p == 127) return 0;
    return 1;
}

int split_line(char *line, char **words, int size) {
    int count = 0;
    char *save;
    for (char *word = strtok_r(line, " ", &save); word; word = strtok_r(NULL, " ", &save)) {
        if (count == size) return -1;
        words[count++] = word;
    }
    return count;
}

int parse_number(const char *text, int max, int *value) {
    if (!*text || strspn(text, "0123456789") != strlen(text)) return -1;
    errno = 0;
    long number = strtol(text, NULL, 10);
    if (errno || number > max) return -1;
    *value = (int)number;
    return 0;
}

int64_t now_ms(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags == -1 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static struct addrinfo *address(const char *host, int port) {
    char service[6];
    snprintf(service, sizeof(service), "%u", (unsigned int)(unsigned short)port);
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM,
                             .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV};
    struct addrinfo *result;
    int status = getaddrinfo(host, service, &hints, &result);
    if (!status) return result;
    fprintf(stderr, "IP %s: %s\n", host, gai_strerror(status));
    errno = EINVAL;
    return NULL;
}

int net_listen(const char *host, int port, int *bound_port) {
    struct addrinfo *addr = address(host, port);
    if (!addr) return -1;
    int fd = socket(addr->ai_family, addr->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int yes = 1;
    struct sockaddr_storage local;
    socklen_t length = sizeof(local);
    if (fd != -1 &&
        (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) ||
         (addr->ai_family == AF_INET6 && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes))) ||
         bind(fd, addr->ai_addr, addr->ai_addrlen) || listen(fd, 128) ||
         getsockname(fd, (struct sockaddr *)&local, &length))) {
        int error = errno;
        close(fd);
        errno = error;
        fd = -1;
    }
    freeaddrinfo(addr);
    if (fd != -1)
        *bound_port = ntohs(local.ss_family == AF_INET ? ((struct sockaddr_in *)&local)->sin_port
                                                       : ((struct sockaddr_in6 *)&local)->sin6_port);
    return fd;
}

int net_connect(const char *host, int port) {
    struct addrinfo *addr = address(host, port);
    if (!addr) return -1;
    int fd = socket(addr->ai_family, addr->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int error = errno;
    if (fd != -1 && connect(fd, addr->ai_addr, addr->ai_addrlen)) {
        error = errno;
        if (error == EINPROGRESS) {
            struct pollfd poll_fd = {fd, POLLOUT, 0};
            int ready = poll(&poll_fd, 1, 3000);
            socklen_t length = sizeof(error);
            if (!ready) error = ETIMEDOUT;
            else if (ready < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length)) error = errno;
        }
        if (error) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(addr);
    errno = error;
    return fd;
}

int net_accept(int listener, int *loopback) {
    struct sockaddr_storage remote;
    socklen_t length = sizeof(remote);
    int fd = accept4(listener, (struct sockaddr *)&remote, &length, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd >= 0) {
        if (remote.ss_family == AF_INET) {
            uint32_t ip = ntohl(((struct sockaddr_in *)&remote)->sin_addr.s_addr);
            *loopback = (ip >> 24) == 127;
        } else {
            *loopback = IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6 *)&remote)->sin6_addr);
        }
    }
    return fd;
}

void channel_close(Channel *channel) {
    if (channel->fd >= 0) close(channel->fd);
    free(channel->output);
    *channel = (Channel){.fd = -1};
}

int channel_pending(const Channel *channel) {
    return channel->output_start < channel->output_end;
}

int channel_send(Channel *channel, const char *format, ...) {
    char line[NET_LINE];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length < 0 || length >= NET_LINE - 1) return -1;
    line[length++] = '\n';

    if (channel->output_end + (size_t)length > channel->output_capacity) {
        size_t pending = channel->output_end - channel->output_start;
        if (pending + (size_t)length > NET_OUTPUT_LIMIT) return -1;
        if (pending) memmove(channel->output, channel->output + channel->output_start, pending);
        channel->output_start = 0;
        channel->output_end = pending;
        if (pending + (size_t)length > channel->output_capacity) {
            size_t capacity = channel->output_capacity ? channel->output_capacity : 2048;
            while (capacity < pending + (size_t)length) capacity *= 2;
            char *output = realloc(channel->output, capacity);
            if (!output) return -1;
            channel->output = output;
            channel->output_capacity = capacity;
        }
    }
    memcpy(channel->output + channel->output_end, line, (size_t)length);
    channel->output_end += (size_t)length;
    return 0;
}

int channel_flush(Channel *channel) {
    while (channel_pending(channel)) {
        ssize_t sent = send(channel->fd, channel->output + channel->output_start,
                            channel->output_end - channel->output_start, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) return 0;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (sent <= 0) return -1;
        channel->output_start += (size_t)sent;
    }
    channel->output_start = channel->output_end = 0;
    return 0;
}

int channel_read(Channel *channel, LineHandler handler, void *context) {
    for (int batch = 0; batch < 4; ++batch) {
        char buffer[NET_LINE];
        ssize_t count = recv(channel->fd, buffer, sizeof(buffer), 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (count < 0) return -1;
        if (!count) return channel->input_size ? -1 : 1;
        for (ssize_t i = 0; i < count; ++i) {
            unsigned char byte = (unsigned char)buffer[i];
            if (byte == '\n') {
                channel->input[channel->input_size] = 0;
                channel->input_size = 0;
                if (handler(context, channel->input)) return 2;
            } else if (byte < 32 || byte == 127 || channel->input_size == sizeof(channel->input) - 1) {
                return -1;
            } else {
                channel->input[channel->input_size++] = (char)byte;
            }
        }
    }
    return 0;
}

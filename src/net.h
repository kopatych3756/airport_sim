#ifndef AIRPORT_NET_H
#define AIRPORT_NET_H

#include <stddef.h>
#include <stdint.h>
#include <signal.h>

#define NET_LINE 1200
#define NET_OUTPUT_LIMIT (128 * 1024)
#define NET_PROTOCOL "1"
#define NET_TOKEN_LIMIT 80

enum Role { NONE, AIRCRAFT, RUNWAY, WATCH, CONTROL };

typedef struct {
    int fd;
    char input[NET_LINE];
    size_t input_size;
    char *output;
    size_t output_start, output_end, output_capacity;
} Channel;

typedef int (*LineHandler)(void *context, char *line);

const char *role_name(enum Role role);
enum Role parse_role(const char *name);
int valid_token(const char *token, size_t limit);
int split_line(char *line, char **words, int size);
int parse_number(const char *text, int max, int *value);
int64_t now_ms(void);
int set_nonblocking(int fd);
int net_listen(const char *host, int port, int *bound_port);
int net_connect(const char *host, int port);
int net_accept(int listener, int *loopback);
void channel_close(Channel *channel);
int channel_send(Channel *channel, const char *format, ...);
int channel_pending(const Channel *channel);
int channel_flush(Channel *channel);
// 0: ещё можно читать, 1: EOF, -1: ошибка, 2: обработчик закончил работу.
int channel_read(Channel *channel, LineHandler handler, void *context);
int setup_signals(void);
extern volatile sig_atomic_t interrupted;
#endif

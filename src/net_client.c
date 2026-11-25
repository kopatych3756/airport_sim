#define _POSIX_C_SOURCE 200809L
#include "airport.h"
#include "net.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef CLIENT_ROLE
#error CLIENT_ROLE is required
#endif

typedef struct {
    Channel wire;
    int welcomed, done, exit_code, result_ok;
} Client;

static int show_line(const char *line) {
    size_t size = strlen(line);
    while (size) {
        ssize_t n = write(STDOUT_FILENO, line, size);
        if (n < 0 && errno == EINTR && !interrupted) continue;
        if (n <= 0) return -1;
        size -= (size_t)n;
        line += n;
    }
    return write(STDOUT_FILENO, "\n", 1) == 1 ? 0 : -1;
}

static int receive_line(void *context, char *line) {
    Client *client = context;
    if (show_line(line)) {
        client->done = 1;
        client->exit_code = 4;
        return 1;
    }

    if (!strncmp(line, "WELCOME ", 8) && !client->welcomed) {
        client->welcomed = 1;
        if ((CLIENT_ROLE == AIRCRAFT && channel_send(&client->wire, "REQUEST")) ||
            (CLIENT_ROLE == RUNWAY && channel_send(&client->wire, "READY"))) {
            client->exit_code = 3;
            client->done = 1;
        }
    } else if (!strcmp(line, "RESULT OK")) {
        client->result_ok = 1;
    } else if (!strncmp(line, "ERROR ", 6) || !strncmp(line, "RESULT ERROR ", 13)) {
        client->exit_code = 2;
    } else if (!strncmp(line, "BYE ", 4)) {
        client->done = 1;
        if (!client->exit_code && CLIENT_ROLE == CONTROL && !client->result_ok) client->exit_code = 3;
    }
    return client->done;
}

static void usage(const char *program) {
    if (CLIENT_ROLE == CONTROL) {
        printf("Usage: %s IP PORT status|start\n"
               "       %s IP PORT off|on AIRCRAFT|RUNWAY|WATCH NAME\n"
               "Use the server's AIRPORT_CONTROL_TOKEN if it is configured.\n", program, program);
    } else {
        printf("Usage: %s IP PORT NAME\n", program);
    }
}

static int queue_control(Channel *wire, int argc, char **argv) {
    const char *command = argv[3];
    if (argc == 4 && (!strcmp(command, "start") || !strcmp(command, "status")))
        return channel_send(wire, "%s", !strcmp(command, "start") ? "START" : "STATUS");

    if (argc != 6 || (strcmp(command, "off") && strcmp(command, "on")) ||
        parse_role(argv[4]) < AIRCRAFT || parse_role(argv[4]) > WATCH ||
        !valid_token(argv[5], NAME_LEN - 1))
        return -1;
    return channel_send(wire, "%s %s %s", !strcmp(command, "off") ? "OFF" : "ON", argv[4], argv[5]);
}

static int queue_registration(Channel *wire, int argc, char **argv) {
    if (CLIENT_ROLE == CONTROL) {
        const char *token = getenv("AIRPORT_CONTROL_TOKEN");
        if (token && !valid_token(token, NET_TOKEN_LIMIT)) return -1;
        int status = token ? channel_send(wire, "HELLO " NET_PROTOCOL " CONTROL %s", token)
                           : channel_send(wire, "HELLO " NET_PROTOCOL " CONTROL");
        return status ? -1 : queue_control(wire, argc, argv);
    }
    if (argc != 4 || !valid_token(argv[3], NAME_LEN - 1)) return -1;
    return channel_send(wire, "HELLO " NET_PROTOCOL " %s %s", role_name(CLIENT_ROLE), argv[3]);
}

static int exchange(Client *client) {
    int64_t registration_deadline = now_ms() + 5000;
    while (!client->done && !interrupted) {
        int waiting = !client->welcomed || CLIENT_ROLE == CONTROL;
        if (waiting && now_ms() >= registration_deadline) {
            fprintf(stderr, "Timed out waiting for the server response.\n");
            return 3;
        }
        short events = POLLIN;
        if (channel_pending(&client->wire)) events |= POLLOUT;
        struct pollfd p = {client->wire.fd, events, 0};
        int ready = poll(&p, 1, 200);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if ((p.revents & (POLLIN | POLLHUP)) && channel_read(&client->wire, receive_line, client)) {
            if (client->done) break;
            fprintf(stderr, "Connection closed before BYE.\n");
            return 3;
        }
        if ((p.revents & POLLOUT) && channel_flush(&client->wire)) break;
        if (p.revents & (POLLERR | POLLNVAL)) break;
    }
    if (interrupted) return 130;
    if (client->done) return client->exit_code;
    fprintf(stderr, "TCP exchange failed.\n");
    return 3;
}

int main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        usage(argv[0]);
        return 0;
    }
    int port;
    Client client = {.wire.fd = -1};
    if (argc < 4 || parse_number(argv[2], 65535, &port) || !port ||
        queue_registration(&client.wire, argc, argv)) {
        usage(argv[0]);
        channel_close(&client.wire);
        return 2;
    }
    if (setup_signals()) {
        perror("signals");
        channel_close(&client.wire);
        return 3;
    }
    client.wire.fd = net_connect(argv[1], port);
    if (client.wire.fd == -1) {
        perror("connect");
        channel_close(&client.wire);
        return interrupted ? 130 : 3;
    }
    int result = exchange(&client);
    channel_close(&client.wire);
    return result;
}

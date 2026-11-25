#define _POSIX_C_SOURCE 200809L
#include "airport.h"
#include "net.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_WATCHES 48
#define MAX_PEERS (MAX_PLANES + MAX_RUNWAYS + MAX_WATCHES + 24)
#define HANDSHAKE_MS 3000
#define DRAIN_MS 2000

typedef struct {
    Channel wire;
    enum Role role;
    int index, loopback, closing, broken;
    int64_t deadline;
    char name[NAME_LEN];
} Peer;

static Peer peers[MAX_PEERS];
static Peer *plane_peer[MAX_PLANES], *runway_peer[MAX_RUNWAYS];
static int plane_requested[MAX_PLANES], runway_ready[MAX_RUNWAYS];
static int plane_disabled[MAX_PLANES], runway_disabled[MAX_RUNWAYS];
static char disabled_watches[MAX_WATCHES][NAME_LEN];
static uint64_t sequence;
static int running, start_requested, quiet;
static const char *control_token;

static void server_line(const char *format, ...) {
    if (quiet) return;
    va_list args;
    va_start(args, format);
    vdprintf(STDOUT_FILENO, format, args);
    va_end(args);
}

static void detach(Peer *peer) {
    if (peer->role == AIRCRAFT && plane_peer[peer->index] == peer)
        plane_peer[peer->index] = NULL;
    if (peer->role == RUNWAY && runway_peer[peer->index] == peer) {
        runway_peer[peer->index] = NULL;
        runway_ready[peer->index] = 0;
    }
}

static void drop(Peer *peer) {
    detach(peer);
    peer->broken = 1;
}

#define SEND(peer, ...) do { \
    if (!(peer)->broken && channel_send(&(peer)->wire, __VA_ARGS__)) drop(peer); \
} while (0)

static void goodbye(Peer *peer, const char *reason) {
    SEND(peer, "BYE %s", reason);
    detach(peer);
    peer->closing = 1;
    peer->deadline = now_ms() + DRAIN_MS;
}

static void publish(const char *kind, int plane, int runway, const char *line) {
    if (!kind) {
        if (*line == '\n') ++line;
        for (int i = 0; i < MAX_PEERS; ++i)
            if (peers[i].role == WATCH && !peers[i].closing && !peers[i].broken)
                SEND(&peers[i], "SUMMARY %s", line);
        return;
    }

    ++sequence;
    if (plane >= 0 && plane_peer[plane])
        SEND(plane_peer[plane], "EVENT %" PRIu64 " %s", sequence, line);
    if (runway >= 0 && runway_peer[runway])
        SEND(runway_peer[runway], "EVENT %" PRIu64 " %s", sequence, line);
    if ((plane < 0 && runway < 0) || !strcmp(kind, "ASSIGN"))
        server_line("%s\n", line);
    for (int i = 0; i < MAX_PEERS; ++i)
        if (peers[i].role == WATCH && !peers[i].closing && !peers[i].broken)
            SEND(&peers[i], "EVENT %" PRIu64 " %s", sequence, line);
}

static void plane_snapshot(Peer *peer, int i) {
    const Plane *p = &airport->planes[i];
    SEND(peer, "AIRCRAFT %s operation=%s type=%s required=%d scheduled=%d duration=%d "
         "priority=%d emergency=%d state=%s fuel=%d burn=%d runway=%s connected=%d requested=%d enabled=%d",
         p->number, p->operation == ARRIVAL ? "ARRIVAL" : "DEPARTURE",
         airport->types[p->type_index].name, airport->types[p->type_index].required_length,
         p->request_tick, p->duration, p->priority, p->emergency, plane_state_name(p->state),
         p->fuel, p->burn, p->runway_index < 0 ? "-" : airport->runways[p->runway_index].name,
         plane_peer[i] != NULL, plane_requested[i], !plane_disabled[i]);
}

static void runway_snapshot(Peer *peer, int i) {
    const Runway *r = &airport->runways[i];
    SEND(peer, "RUNWAY %s length=%d safety=%d closed=%d active=%s finish=%d "
         "next_free=%d completed=%d connected=%d ready=%d enabled=%d",
         r->name, r->length, r->safety, runway_closed(i),
         r->busy ? airport->planes[r->plane_index].number : "-", r->busy ? r->finish_tick : -1,
         r->next_free_tick, r->operations, runway_peer[i] != NULL, runway_ready[i], !runway_disabled[i]);
}

static void snapshot(Peer *peer) {
    SEND(peer, "SNAPSHOT BEGIN seq=%" PRIu64 " tick=%d phase=%s",
         sequence, airport->current_tick, running ? "RUNNING" : "REGISTERING");
    SEND(peer, "AIRPORT seed=%u max_ticks=%d delay_ms=%d emergency_fuel_ticks=%d runways=%d aircraft=%d",
         airport->seed_value, airport->max_ticks, airport->delay_ms, airport->emergency_fuel_ticks,
         airport->runway_count, airport->plane_count);
    SEND(peer, "DISPATCHER %s assignments=%d emergency_assignments=%d", airport->dispatcher.name,
         airport->dispatcher.assignments, airport->dispatcher.emergency_assignments);
    for (int i = 0; i < airport->runway_count; ++i) runway_snapshot(peer, i);
    for (int i = 0; i < airport->plane_count; ++i) plane_snapshot(peer, i);
    if (peer->role == CONTROL) {
        for (int i = 0; i < MAX_PEERS; ++i)
            if (peers[i].role == WATCH && !peers[i].closing && !peers[i].broken)
                SEND(peer, "WATCH %s connected=1 enabled=1", peers[i].name);
        for (int i = 0; i < MAX_WATCHES; ++i)
            if (disabled_watches[i][0])
                SEND(peer, "WATCH %s connected=0 enabled=0", disabled_watches[i]);
    }
    SEND(peer, "SNAPSHOT END seq=%" PRIu64, sequence);
}

static int find_actor(enum Role role, const char *name) {
    int count = role == AIRCRAFT ? airport->plane_count : airport->runway_count;
    for (int i = 0; i < count; ++i) {
        const char *candidate = role == AIRCRAFT ? airport->planes[i].number : airport->runways[i].name;
        if (!strcmp(name, candidate)) return i;
    }
    return -1;
}

static Peer *find_watch(const char *name, int *count) {
    Peer *match = NULL;
    *count = 0;
    for (int i = 0; i < MAX_PEERS; ++i) {
        Peer *p = &peers[i];
        if (p->role != WATCH || p->closing || p->broken) continue;
        ++*count;
        if (!strcmp(name, p->name)) match = p;
    }
    return match;
}

static int watch_blocked(const char *name) {
    for (int i = 0; i < MAX_WATCHES; ++i)
        if (!strcmp(name, disabled_watches[i])) return i;
    return -1;
}

static const char *register_peer(Peer *peer, char **words, int count) {
    if (count < 3 || strcmp(words[0], "HELLO") || strcmp(words[1], NET_PROTOCOL)) return "BAD_HELLO";
    enum Role role = parse_role(words[2]);
    if (role == CONTROL) {
        if (control_token ? count != 4 || strcmp(words[3], control_token)
                          : count != 3 || !peer->loopback) return "CONTROL_DENIED";
        peer->role = role;
        SEND(peer, "WELCOME CONTROL");
        return NULL;
    }
    if (role == NONE || count != 4 || !valid_token(words[3], NAME_LEN - 1)) return "BAD_HELLO";
    const char *name = words[3];
    if (role == WATCH) {
        int watches;
        if (watch_blocked(name) >= 0) return "ADMIN_OFFLINE";
        if (find_watch(name, &watches)) return "DUPLICATE_CLIENT";
        if (watches >= MAX_WATCHES) return "WATCH_LIMIT";
    } else {
        int i = find_actor(role, name);
        if (i < 0) return "UNKNOWN_ACTOR";
        Peer **slot = role == AIRCRAFT ? &plane_peer[i] : &runway_peer[i];
        if (role == AIRCRAFT ? plane_disabled[i] : runway_disabled[i]) return "ADMIN_OFFLINE";
        if (*slot) return "DUPLICATE_CLIENT";
        *slot = peer;
        peer->index = i;
    }

    peer->role = role;
    strcpy(peer->name, name);
    SEND(peer, "WELCOME %s %s", role_name(role), name);
    if (role == WATCH) snapshot(peer);
    if (role == AIRCRAFT) plane_snapshot(peer, peer->index);
    if (role == RUNWAY) runway_snapshot(peer, peer->index);
    server_line("CONNECTED role=%s name=%s\n", role_name(role), name);
    return NULL;
}

static const char *set_enabled(enum Role role, const char *name, int enabled) {
    Peer *target;
    if (role == WATCH) {
        int count;
        target = find_watch(name, &count);
        int blocked = watch_blocked(name);
        if (enabled && blocked >= 0) disabled_watches[blocked][0] = 0;
        if (!enabled && blocked < 0) {
            for (int i = 0; i < MAX_WATCHES; ++i) {
                if (!disabled_watches[i][0]) {
                    blocked = i;
                    break;
                }
            }
            if (blocked < 0) return "WATCH_BLOCK_LIMIT";
            strcpy(disabled_watches[blocked], name);
        }
    } else {
        int i = find_actor(role, name);
        if (i < 0) return "UNKNOWN_ACTOR";
        if (role == AIRCRAFT) {
            plane_disabled[i] = !enabled;
            target = plane_peer[i];
        } else {
            runway_disabled[i] = !enabled;
            target = runway_peer[i];
        }
    }
    if (target && !enabled) goodbye(target, "ADMIN_OFFLINE");
    event("CONTROL", "role=%s name=%s enabled=%d", role_name(role), name, enabled);
    return NULL;
}

static void control(Peer *peer, char **words, int count) {
    const char *error = NULL;
    if (count == 1 && !strcmp(words[0], "STATUS")) {
        snapshot(peer);
    } else if (count == 1 && !strcmp(words[0], "START")) {
        start_requested = 1;
    } else if (count == 3 && (!strcmp(words[0], "OFF") || !strcmp(words[0], "ON"))) {
        enum Role role = parse_role(words[1]);
        if (role == NONE || role == CONTROL || !valid_token(words[2], NAME_LEN - 1)) error = "BAD_TARGET";
        else error = set_enabled(role, words[2], !strcmp(words[0], "ON"));
    } else {
        error = "BAD_COMMAND";
    }
    if (error) SEND(peer, "RESULT ERROR %s", error);
    else SEND(peer, "RESULT OK");
    goodbye(peer, "CONTROL_COMPLETE");
}

static int receive_line(void *context, char *line) {
    Peer *peer = context;
    char *words[5];
    int count = split_line(line, words, 5);
    const char *error = NULL;
    if (peer->role == NONE) {
        error = register_peer(peer, words, count);
    } else if (peer->role == CONTROL) {
        control(peer, words, count);
    } else if (peer->role == AIRCRAFT && count == 1 && !strcmp(words[0], "REQUEST")) {
        int i = peer->index;
        int first = !plane_requested[i];
        plane_requested[i] = 1;
        SEND(peer, "ACK REQUEST %s", first ? "ACCEPTED" : "ALREADY_ACCEPTED");
        event("NETWORK", "aircraft=%s request=%s", peer->name, first ? "accepted" : "already_accepted");
    } else if (peer->role == RUNWAY && count == 1 && !strcmp(words[0], "READY")) {
        runway_ready[peer->index] = 1;
        SEND(peer, "ACK READY");
        event("NETWORK", "runway=%s ready=%d", peer->name, runway_ready[peer->index]);
    } else {
        error = "BAD_COMMAND";
    }
    if (error) {
        SEND(peer, "ERROR %s", error);
        goodbye(peer, error);
    }
    return peer->closing || peer->broken;
}

static void accept_ready(int listener) {
    for (int accepted = 0; accepted < 32; ++accepted) {
        int loopback;
        int fd = net_accept(listener, &loopback);
        if (fd < 0) return;
        int slot = 0;
        while (slot < MAX_PEERS && peers[slot].wire.fd != -1) ++slot;
        if (slot == MAX_PEERS) {
            close(fd);
        } else {
            peers[slot] = (Peer){.wire.fd = fd, .loopback = loopback,
                                 .deadline = now_ms() + HANDSHAKE_MS};
        }
    }
}

static void disconnected(Peer *peer) {
    int was_online = peer->role == RUNWAY && runway_ready[peer->index] && !peer->closing;
    int aircraft_connected = peer->role == AIRCRAFT && !peer->closing;
    drop(peer);
    if (was_online) event("NETWORK", "runway=%s ready=0 disconnected", peer->name);
    if (aircraft_connected) event("NETWORK", "aircraft=%s disconnected", peer->name);
}

static int network_cycle(int listener, int timeout) {
    struct pollfd poll_fds[MAX_PEERS + 1];
    int slots[MAX_PEERS + 1];
    int count = 1;
    int64_t now = now_ms();
    poll_fds[0] = (struct pollfd){listener, POLLIN, 0};
    for (int i = 0; i < MAX_PEERS; ++i) {
        Peer *p = &peers[i];
        if (p->wire.fd < 0) continue;
        int timed = p->closing || p->role == NONE || p->role == CONTROL ||
                    (p->role == AIRCRAFT && !plane_requested[p->index]) ||
                    (p->role == RUNWAY && !runway_ready[p->index]);
        if (p->broken || (p->closing && !channel_pending(&p->wire)) || (timed && now >= p->deadline)) {
            detach(p);
            channel_close(&p->wire);
            *p = (Peer){.wire.fd = -1};
            continue;
        }
        if (timed && p->deadline - now < timeout) timeout = (int)(p->deadline - now);
        short events = p->closing ? 0 : POLLIN;
        if (channel_pending(&p->wire)) events |= POLLOUT;
        slots[count] = i;
        poll_fds[count++] = (struct pollfd){p->wire.fd, events, 0};
    }
    int ready = poll(poll_fds, (nfds_t)count, timeout);
    if (ready < 0) return errno == EINTR ? 0 : -1;
    if (poll_fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    if (poll_fds[0].revents & POLLIN) accept_ready(listener);

    for (int i = 1; i < count; ++i) {
        Peer *peer = &peers[slots[i]];
        short events = poll_fds[i].revents;
        if (!events || peer->broken) continue;
        if (!peer->closing && (events & (POLLIN | POLLHUP))) {
            int result = channel_read(&peer->wire, receive_line, peer);
            if (result == 1 || result == -1) disconnected(peer);
        }
        if (peer->broken) continue;
        if ((events & POLLOUT) && channel_flush(&peer->wire)) disconnected(peer);
        if ((events & (POLLERR | POLLNVAL)) || (peer->closing && (events & POLLHUP)))
            disconnected(peer);
    }
    return 0;
}

static int model_step(int id) {
    int dispatcher = airport->plane_count + airport->runway_count;
    /* Принятый запрос живёт до исхода даже при потере соединения.
       Полосы продолжают обслуживание начатых операций независимо от READY. */
    if (id == dispatcher) {
        dispatcher_step(runway_ready);
    } else if (id >= airport->plane_count || plane_requested[id]) {
        actor_step(id);
    }
    return 0;
}

static const char *serve(int listener, int startup_ms) {
    int64_t deadline = now_ms() + startup_ms;
    int next_tick = 0;
    while (!interrupted) {
        int64_t wait = deadline - now_ms();
        /* Верхний предел также закрывает гонку между проверкой сигнала и poll. */
        int timeout = wait <= 0 || (!running && start_requested) ? 0 : wait > 100 ? 100 : (int)wait;
        if (network_cycle(listener, timeout)) return "NETWORK_FAILURE";
        if (interrupted) break;
        if (now_ms() < deadline && (running || !start_requested)) continue;

        if (!running) {
            running = 1;
            event("SHIFT", "simulation started; seed=%u", airport->seed_value);
        }
        airport->current_tick = next_tick++;
        run_tick(model_step);
        if (check_invariants()) return "INVARIANT_FAILURE";
        if (io_failed()) return "IO_FAILURE";
        if (all_finished()) return "ALL_REQUESTS_COMPLETED";
        if (airport->max_ticks && airport->current_tick >= airport->max_ticks) return "SHIFT_ENDED";
        if (airport->current_tick >= 2000000000) return "TIME_LIMIT";
        deadline = now_ms() + airport->delay_ms;
    }
    return "USER_INTERRUPT";
}

static void shutdown_clients(const char *reason) {
    for (int i = 0; i < MAX_PEERS; ++i) {
        Peer *p = &peers[i];
        if (p->wire.fd != -1 && !p->closing && !p->broken) goodbye(p, reason);
    }
    int64_t limit = now_ms() + DRAIN_MS;
    for (;;) {
        int pending = 0;
        for (int i = 0; i < MAX_PEERS; ++i)
            if (!peers[i].broken && channel_pending(&peers[i].wire)) ++pending;
        int64_t wait = limit - now_ms();
        if (!pending || wait <= 0 || network_cycle(-1, (int)wait)) break;
    }
    for (int i = 0; i < MAX_PEERS; ++i) channel_close(&peers[i].wire);
}

static int valid_config_names(void) {
    for (int i = 0; i < airport->plane_count; ++i)
        if (!valid_token(airport->planes[i].number, NAME_LEN - 1)) return 0;
    for (int i = 0; i < airport->runway_count; ++i)
        if (!valid_token(airport->runways[i].name, NAME_LEN - 1)) return 0;
    for (int i = 0; i < airport->type_count; ++i)
        if (!valid_token(airport->types[i].name, NAME_LEN - 1)) return 0;
    return 1;
}

static void usage(const char *program) {
    printf("Usage: %s --bind IP --port PORT [-c FILE] [--startup-ms N]\n"
           "       [--delay-ms N] [--max-ticks N] [--seed N] [--log FILE|--no-log] [--quiet]\n"
           "Port 0 selects a free port. Set AIRPORT_CONTROL_TOKEN for administration over the network.\n",
           program);
}

int main(int argc, char **argv) {
    const char *host = NULL, *config = "configs/default.conf", *log_file = "tcp_simulation.log";
    int port = -1, startup_ms = 3000, seed = -1, delay = -1, max_ticks = -1;
    for (int i = 1; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--help") || !strcmp(key, "-h")) {
            usage(argv[0]);
            return 0;
        }
        if (!strcmp(key, "--quiet")) {
            quiet = 1;
            continue;
        }
        if (!strcmp(key, "--no-log")) {
            log_file = NULL;
            continue;
        }
        if (++i == argc) {
            usage(argv[0]);
            return 2;
        }
        const char *value = argv[i];
        if (!strcmp(key, "--bind")) host = value;
        else if (!strcmp(key, "--port") && !parse_number(value, 65535, &port)) {}
        else if (!strcmp(key, "--startup-ms") && !parse_number(value, 1000000, &startup_ms)) {}
        else if (!strcmp(key, "--seed") && !parse_number(value, 1000000, &seed)) {}
        else if (!strcmp(key, "--delay-ms") && !parse_number(value, 1000000, &delay)) {}
        else if (!strcmp(key, "--max-ticks") && !parse_number(value, 1000000, &max_ticks)) {}
        else if (!strcmp(key, "--config") || !strcmp(key, "-c")) config = value;
        else if (!strcmp(key, "--log")) log_file = value;
        else {
            fprintf(stderr, "Unknown or invalid option: %s\n", key);
            return 2;
        }
    }

    control_token = getenv("AIRPORT_CONTROL_TOKEN");
    if (!host || port < 0 || (control_token && !valid_token(control_token, NET_TOKEN_LIMIT))) {
        usage(argv[0]);
        return 2;
    }
    init_airport();
    if (load_config(config)) return 2;
    if (seed >= 0) airport->seed_value = (unsigned)seed;
    if (delay >= 0) airport->delay_ms = delay;
    if (max_ticks >= 0) airport->max_ticks = max_ticks;
    srand(airport->seed_value);
    if (generate_flows()) return 2;
    if (!valid_config_names()) {
        fprintf(stderr, "An aircraft, runway or type name contains an invalid protocol character.\n");
        return 2;
    }
    if (setup_signals()) {
        perror("signals");
        return 3;
    }

    int listener = net_listen(host, port, &port);
    if (listener < 0) {
        perror("listen");
        return 3;
    }
    if (log_file && open_log_file(log_file)) {
        close(listener);
        return 2;
    }
    for (int i = 0; i < MAX_PEERS; ++i) peers[i].wire.fd = -1;
    set_quiet_mode(1);
    set_output_listener(publish);
    dprintf(STDOUT_FILENO, "READY bind=%s port=%d aircraft=%d runways=%d startup_ms=%d\n",
            host, port, airport->plane_count, airport->runway_count, startup_ms);
    print_line("=== AIRPORT TCP SIMULATION / VARIANT 31 ===");
    print_line("seed=%u max_ticks=%d delay_ms=%d", airport->seed_value, airport->max_ticks, airport->delay_ms);
    const char *reason = serve(listener, startup_ms);
    close(listener);
    print_termination_event(reason);
    print_summary(reason);
    shutdown_clients(reason);
    close_log_file();
    dprintf(STDOUT_FILENO, "SERVER_END reason=%s tick=%d\n", reason, airport->current_tick);

    if (io_failed()) return 4;
    if (!strcmp(reason, "USER_INTERRUPT")) return 130;
    return !strcmp(reason, "ALL_REQUESTS_COMPLETED") || !strcmp(reason, "SHIFT_ENDED") ? 0 : 3;
}

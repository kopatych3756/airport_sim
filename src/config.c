#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "airport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_operation(const char *text) {
    if (strcmp(text, "ARRIVAL") == 0) return ARRIVAL;
    if (strcmp(text, "DEPARTURE") == 0) return DEPARTURE;
    return -1;
}

static int parse_priority(const char *text) {
    if (strcmp(text, "LOW") == 0) return LOW;
    if (strcmp(text, "NORMAL") == 0) return NORMAL;
    if (strcmp(text, "HIGH") == 0) return HIGH;
    return -1;
}

static int find_runway(const char *name) {
    int i;
    for (i = 0; i < runway_count; ++i) {
        if (strcmp(runways[i].name, name) == 0) return i;
    }
    return -1;
}

static int find_type(const char *name) {
    int i;
    for (i = 0; i < type_count; ++i) {
        if (strcmp(types[i].name, name) == 0) return i;
    }
    return -1;
}

static int find_plane(const char *number) {
    int i;
    for (i = 0; i < plane_count; ++i) {
        if (strcmp(planes[i].number, number) == 0) return i;
    }
    return -1;
}

static int random_range(int min_value, int max_value) {
    if (max_value <= min_value) return min_value;
    return min_value + rand() % (max_value - min_value + 1);
}

static int random_priority(void) {
    int total = priority_weights[LOW] + priority_weights[NORMAL] + priority_weights[HIGH];
    int value;

    if (total <= 0) return NORMAL;

    value = random_range(1, total);
    if (value <= priority_weights[LOW]) return LOW;

    value -= priority_weights[LOW];
    if (value <= priority_weights[NORMAL]) return NORMAL;

    return HIGH;
}

static int add_plane(const char *number, int operation, int request_tick,
                     const char *type_name, int fuel, int burn, int priority) {
    Plane *p;
    int type_index;

    if (plane_count >= MAX_PLANES) {
        fprintf(stderr, "Too many aircraft. Increase MAX_PLANES.\n");
        return -1;
    }

    if (strlen(number) >= NAME_LEN) {
        fprintf(stderr, "Aircraft number is too long: %s\n", number);
        return -1;
    }

    if (find_plane(number) >= 0) {
        fprintf(stderr, "Duplicate aircraft number: %s\n", number);
        return -1;
    }

    type_index = find_type(type_name);
    if (type_index < 0) {
        fprintf(stderr, "Unknown aircraft type: %s\n", type_name);
        return -1;
    }

    p = &planes[plane_count++];
    memset(p, 0, sizeof(*p));
    snprintf(p->number, sizeof(p->number), "%s", number);
    p->operation = operation;
    p->type_index = type_index;
    p->request_tick = request_tick;
    p->fuel = fuel;
    p->burn = burn;
    p->priority = priority;
    p->state = PENDING;
    p->runway_index = -1;
    p->duration = operation == ARRIVAL
                      ? types[type_index].landing_time
                      : types[type_index].takeoff_time;

    return 0;
}

static int number(const char *s, int *v) {
    char *end;
    errno = 0;
    long x = strtol(s, &end, 10);
    if (errno || !*s || *end || x < 0 || x > 1000000) return -1;
    *v = (int)x;
    return 0;
}
int load_config(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }
    FILE *file = fdopen(fd, "r");
    if (!file) { perror("fdopen"); close(fd); return -1; }
    char line[1024];
    int line_no = 0, bad = 0;
    while (fgets(line, sizeof(line), file)) {
        ++line_no;
        if (!strchr(line, '\n') && !feof(file)) { bad = 1; break; }
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        char *tokens[10], *save = NULL;
        int n = 0;
        for (char *t = strtok_r(line, " \t\r\n", &save); t; t = strtok_r(NULL, " \t\r\n", &save)) {
            if (n == 10 || strlen(t) >= NAME_LEN) { bad = 1; break; }
            tokens[n++] = t;
        }
        if (bad) break;
        if (!n) continue;
        int v[10] = {0};
        const char *key = tokens[0];
        int expected = 0, first_numeric = 1;
        if (!strcmp(key, "SEED") || !strcmp(key, "MAX_TICKS") || !strcmp(key, "DELAY_MS") ||
            !strcmp(key, "EMERGENCY_FUEL_TICKS")) expected = 2;
        else if (!strcmp(key, "ARRIVAL_FUEL") || !strcmp(key, "ARRIVAL_BURN")) expected = 3;
        else if (!strcmp(key, "PRIORITY_WEIGHTS")) expected = 4;
        else if (!strcmp(key, "RUNWAY") || !strcmp(key, "CLOSE")) { expected = 4; first_numeric = 2; }
        else if (!strcmp(key, "TYPE")) { expected = 5; first_numeric = 2; }
        else if (!strcmp(key, "FLOW")) { expected = 6; first_numeric = 2; }
        else if (!strcmp(key, "AIRCRAFT")) { expected = 8; first_numeric = 8; }
        if (n != expected) { bad = 1; break; }
        for (int i = first_numeric; i < n; ++i) if (number(tokens[i], &v[i])) bad = 1;
        if (bad) break;
        if (!strcmp(key, "SEED")) seed_value = (unsigned int)v[1];
        else if (!strcmp(key, "MAX_TICKS")) max_ticks = v[1];
        else if (!strcmp(key, "DELAY_MS")) delay_ms = v[1];
        else if (!strcmp(key, "EMERGENCY_FUEL_TICKS")) emergency_fuel_ticks = v[1];
        else if (!strcmp(key, "PRIORITY_WEIGHTS")) {
            if (v[1] + v[2] + v[3] == 0) { bad = 1; break; }
            for (int i = 0; i < 3; ++i) priority_weights[i] = v[i+1];
        } else if (!strcmp(key, "ARRIVAL_FUEL") || !strcmp(key, "ARRIVAL_BURN")) {
            if (v[1] > v[2] || (!strcmp(key, "ARRIVAL_BURN") && v[1] == 0)) { bad = 1; break; }
            if (!strcmp(key, "ARRIVAL_FUEL")) { arrival_fuel_min = v[1]; arrival_fuel_max = v[2]; }
            else { arrival_burn_min = v[1]; arrival_burn_max = v[2]; }
        } else if (!strcmp(key, "RUNWAY")) {
            if (runway_count == MAX_RUNWAYS || find_runway(tokens[1]) >= 0 || !v[2]) { bad = 1; break; }
            Runway *r = &runways[runway_count++];
            memset(r, 0, sizeof(*r));
            strcpy(r->name, tokens[1]);
            r->length = v[2];
            r->safety = v[3];
            r->plane_index = -1;
        } else if (!strcmp(key, "TYPE")) {
            if (type_count == MAX_TYPES || find_type(tokens[1]) >= 0 || !v[2] || !v[3] || !v[4]) { bad = 1; break; }
            PlaneType *t = &types[type_count++];
            strcpy(t->name, tokens[1]);
            t->required_length = v[2];
            t->landing_time = v[3];
            t->takeoff_time = v[4];
        } else if (!strcmp(key, "CLOSE")) {
            int r = find_runway(tokens[1]);
            if (closure_count == MAX_CLOSURES || r < 0 || v[2] >= v[3]) { bad = 1; break; }
            closures[closure_count++] = (Closure){r, v[2], v[3]};
        } else if (!strcmp(key, "FLOW")) {
            int op = parse_operation(tokens[1]);
            if (flow_count == MAX_FLOWS || op < 0 || v[2] > MAX_PLANES || v[4] > v[5]) { bad = 1; break; }
            flows[flow_count++] = (Flow){op, v[2], v[3], v[4], v[5]};
        } else if (!strcmp(key, "AIRCRAFT")) {
            int op = parse_operation(tokens[2]), priority = parse_priority(tokens[7]);
            if (number(tokens[3], &v[3]) || number(tokens[5], &v[5]) || number(tokens[6], &v[6]) ||
                op < 0 || priority < 0 || (op == ARRIVAL && !v[6]) ||
                add_plane(tokens[1], op, v[3], tokens[4], v[5], v[6], priority)) { bad = 1; break; }
        }
    }
    if (ferror(file)) bad = 1;
    if (fclose(file)) bad = 1;
    if (bad) { fprintf(stderr, "%s:%d: invalid configuration\n", path, line_no); return -1; }
    if (!runway_count || !type_count) {
        fprintf(stderr, "Configuration requires RUNWAY and TYPE.\n"); return -1;
    }
    int total = plane_count;
    for (int i = 0; i < flow_count; ++i) total += flows[i].count;
    if (total > MAX_PLANES) { fprintf(stderr, "Too many aircraft (limit %d).\n", MAX_PLANES); return -1; }
    return 0;
}
int generate_flows(void) {
    int f;
    int arrival_number = 0;
    int departure_number = 0;

    for (f = 0; f < flow_count; ++f) {
        int i;
        int tick = flows[f].first_tick;

        for (i = 0; i < flows[f].count; ++i) {
            char number[NAME_LEN];
            int type_index = random_range(0, type_count - 1);
            int fuel = 0;
            int burn = 0;

            if (flows[f].operation == ARRIVAL) {
                snprintf(number, sizeof(number), "GA%03d", ++arrival_number);
                fuel = random_range(arrival_fuel_min, arrival_fuel_max);
                burn = random_range(arrival_burn_min, arrival_burn_max);
            } else {
                snprintf(number, sizeof(number), "GD%03d", ++departure_number);
            }

            if (add_plane(number, flows[f].operation, tick,
                          types[type_index].name, fuel, burn,
                          random_priority()) != 0) {
                return -1;
            }

            if (i + 1 < flows[f].count) {
                tick += random_range(flows[f].min_interval,
                                     flows[f].max_interval);
            }
        }
    }

    return 0;
}

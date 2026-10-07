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

int load_config(const char *path) {
    FILE *file;
    char line[256];

    file = fopen(path, "r");
    if (file == NULL) {
        perror(path);
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char key[32];
        char a[32];
        int x, y, z, w;

        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%31s", key) != 1) continue;

        if (strcmp(key, "SEED") == 0) {
            unsigned int value;
            if (sscanf(line, "%*s %u", &value) == 1) seed_value = value;

        } else if (strcmp(key, "MAX_TICKS") == 0) {
            if (sscanf(line, "%*s %d", &x) == 1) max_ticks = x;

        } else if (strcmp(key, "DELAY_MS") == 0) {
            if (sscanf(line, "%*s %d", &x) == 1) delay_ms = x;

        } else if (strcmp(key, "EMERGENCY_FUEL_TICKS") == 0) {
            if (sscanf(line, "%*s %d", &x) == 1) emergency_fuel_ticks = x;

        } else if (strcmp(key, "PRIORITY_WEIGHTS") == 0) {
            if (sscanf(line, "%*s %d %d %d", &x, &y, &z) == 3) {
                priority_weights[LOW] = x;
                priority_weights[NORMAL] = y;
                priority_weights[HIGH] = z;
            }

        } else if (strcmp(key, "ARRIVAL_FUEL") == 0) {
            if (sscanf(line, "%*s %d %d", &x, &y) == 2) {
                arrival_fuel_min = x;
                arrival_fuel_max = y;
            }

        } else if (strcmp(key, "ARRIVAL_BURN") == 0) {
            if (sscanf(line, "%*s %d %d", &x, &y) == 2) {
                arrival_burn_min = x;
                arrival_burn_max = y;
            }

        } else if (strcmp(key, "RUNWAY") == 0) {
            if (runway_count >= MAX_RUNWAYS) continue;

            if (sscanf(line, "%*s %31s %d %d", a, &x, &y) == 3) {
                Runway *r = &runways[runway_count++];
                memset(r, 0, sizeof(*r));
                snprintf(r->name, sizeof(r->name), "%s", a);
                r->length = x;
                r->safety = y;
                r->plane_index = -1;
            }

        } else if (strcmp(key, "TYPE") == 0) {
            if (type_count >= MAX_TYPES) continue;

            if (sscanf(line, "%*s %31s %d %d %d", a, &x, &y, &z) == 4) {
                PlaneType *t = &types[type_count++];
                snprintf(t->name, sizeof(t->name), "%s", a);
                t->required_length = x;
                t->landing_time = y;
                t->takeoff_time = z;
            }

        } else if (strcmp(key, "CLOSE") == 0) {
            int runway_index;

            if (closure_count >= MAX_CLOSURES) continue;
            if (sscanf(line, "%*s %31s %d %d", a, &x, &y) != 3) continue;

            runway_index = find_runway(a);
            if (runway_index < 0) {
                fprintf(stderr, "Unknown runway in CLOSE: %s\n", a);
                fclose(file);
                return -1;
            }

            closures[closure_count].runway_index = runway_index;
            closures[closure_count].start_tick = x;
            closures[closure_count].end_tick = y;
            ++closure_count;

        } else if (strcmp(key, "FLOW") == 0) {
            int operation;

            if (flow_count >= MAX_FLOWS) continue;
            if (sscanf(line, "%*s %31s %d %d %d %d", a, &x, &y, &z, &w) != 5) continue;

            operation = parse_operation(a);
            if (operation < 0) {
                fprintf(stderr, "Unknown operation in FLOW: %s\n", a);
                fclose(file);
                return -1;
            }

            flows[flow_count].operation = operation;
            flows[flow_count].count = x;
            flows[flow_count].first_tick = y;
            flows[flow_count].min_interval = z;
            flows[flow_count].max_interval = w;
            ++flow_count;

        } else if (strcmp(key, "AIRCRAFT") == 0) {
            char operation_text[32];
            char priority_text[32];
            char type_name[32];
            int request_tick, fuel, burn;
            int operation, priority;

            if (sscanf(line, "%*s %31s %31s %d %31s %d %d %31s",
                       a, operation_text, &request_tick, type_name,
                       &fuel, &burn, priority_text) != 7) {
                continue;
            }

            operation = parse_operation(operation_text);
            priority = parse_priority(priority_text);

            if (operation < 0 || priority < 0 ||
                add_plane(a, operation, request_tick, type_name,
                          fuel, burn, priority) != 0) {
                fclose(file);
                return -1;
            }

        } else {
            fprintf(stderr, "Unknown configuration command: %s\n", key);
            fclose(file);
            return -1;
        }
    }

    fclose(file);

    if (runway_count == 0 || type_count == 0) {
        fprintf(stderr, "Configuration must contain at least one RUNWAY and one TYPE.\n");
        return -1;
    }

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

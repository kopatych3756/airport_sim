#define _POSIX_C_SOURCE 200809L

#include "airport.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *operation_name(int operation) {
    return operation == ARRIVAL ? "LANDING" : "TAKEOFF";
}

static const char *priority_name(int priority) {
    if (priority == HIGH) return "HIGH";
    if (priority == LOW) return "LOW";
    return "NORMAL";
}

static int runway_closed(int runway_index, int tick) {
    int i;

    for (i = 0; i < closure_count; ++i) {
        if (closures[i].runway_index == runway_index &&
            tick >= closures[i].start_tick &&
            tick < closures[i].end_tick) {
            return 1;
        }
    }

    return 0;
}

static void finish_operations(void) {
    int i;

    for (i = 0; i < runway_count; ++i) {
        Runway *r = &runways[i];

        if (r->busy && r->finish_tick == current_tick) {
            Plane *p = &planes[r->plane_index];

            p->state = DONE;
            p->runway_index = -1;

            event("FINISH", "%s completed %s on %s",
                  p->number, operation_name(p->operation), r->name);

            r->busy = 0;
            r->plane_index = -1;
            r->next_free_tick = current_tick + r->safety;
            ++r->operations;

            event("RELEASE", "%s released; next start not before T=%d",
                  r->name, r->next_free_tick);
        }
    }
}

static void print_closure_events(void) {
    int i;

    for (i = 0; i < closure_count; ++i) {
        Runway *r = &runways[closures[i].runway_index];

        if (current_tick == closures[i].start_tick) {
            if (r->busy) {
                event("CLOSE", "%s is closed for new operations; current operation continues",
                      r->name);
            } else {
                event("CLOSE", "%s is closed", r->name);
            }
        }

        if (current_tick == closures[i].end_tick) {
            event("OPEN", "%s is open again", r->name);
        }
    }
}

static void register_requests(void) {
    int i;

    for (i = 0; i < plane_count; ++i) {
        Plane *p = &planes[i];
        PlaneType *t = &types[p->type_index];

        if (p->state != PENDING || p->request_tick != current_tick) continue;

        p->state = WAITING;

        if (p->operation == ARRIVAL) {
            event("REQUEST",
                  "%s requests LANDING: type=%s required=%dm priority=%s fuel=%d burn=%d",
                  p->number, t->name, t->required_length,
                  priority_name(p->priority), p->fuel, p->burn);
        } else {
            event("REQUEST",
                  "%s requests TAKEOFF: type=%s required=%dm priority=%s",
                  p->number, t->name, t->required_length,
                  priority_name(p->priority));
        }

        event("QUEUE", "%s entered the waiting queue", p->number);
    }
}

static void update_emergencies(void) {
    int i;

    for (i = 0; i < plane_count; ++i) {
        Plane *p = &planes[i];
        int critical_fuel;

        if (p->state != WAITING || p->operation != ARRIVAL || p->emergency) {
            continue;
        }

        critical_fuel = p->burn * emergency_fuel_ticks;
        if (p->fuel <= critical_fuel) {
            p->emergency = 1;
            event("EMERGENCY", "%s has critical fuel=%d; emergency priority enabled",
                  p->number, p->fuel);
        }
    }
}

static int best_runway_for_plane(int plane_index) {
    Plane *p = &planes[plane_index];
    int required_length = types[p->type_index].required_length;
    int best_runway = -1;
    int best_extra_length = 1000000000;
    int i;

    for (i = 0; i < runway_count; ++i) {
        Runway *r = &runways[i];
        int extra_length;

        if (r->busy) continue;
        if (runway_closed(i, current_tick)) continue;
        if (current_tick < r->next_free_tick) continue;
        if (r->length < required_length) continue;

        extra_length = r->length - required_length;
        if (best_runway < 0 || extra_length < best_extra_length) {
            best_runway = i;
            best_extra_length = extra_length;
        }
    }

    return best_runway;
}

static int plane_is_better(int first, int second) {
    Plane *a = &planes[first];
    Plane *b = &planes[second];

    if (a->emergency != b->emergency) return a->emergency > b->emergency;
    if (a->priority != b->priority) return a->priority > b->priority;
    if (a->request_tick != b->request_tick) return a->request_tick < b->request_tick;

    return first < second;
}

static void dispatch_planes(void) {
    for (;;) {
        int best_plane = -1;
        int best_runway = -1;
        int i;

        for (i = 0; i < plane_count; ++i) {
            int runway_index;

            if (planes[i].state != WAITING) continue;

            runway_index = best_runway_for_plane(i);
            if (runway_index < 0) continue;

            if (best_plane < 0 || plane_is_better(i, best_plane)) {
                best_plane = i;
                best_runway = runway_index;
            }
        }

        if (best_plane < 0) break;

        {
            Plane *p = &planes[best_plane];
            Runway *r = &runways[best_runway];

            p->state = RUNNING;
            p->runway_index = best_runway;
            ++p->times_started;

            r->busy = 1;
            r->plane_index = best_plane;
            r->finish_tick = current_tick + p->duration;

            ++dispatcher.assignments;
            if (p->emergency) ++dispatcher.emergency_assignments;

            event("ASSIGN", "%s assigned %s to %s%s",
                  dispatcher.name, p->number, r->name,
                  p->emergency ? " (EMERGENCY)" : "");
            event("START", "%s started %s on %s; finish at T=%d",
                  p->number, operation_name(p->operation),
                  r->name, r->finish_tick);
        }
    }
}

static void burn_fuel_and_wait(void) {
    int i;

    for (i = 0; i < plane_count; ++i) {
        Plane *p = &planes[i];

        if (p->state != WAITING) continue;

        ++p->wait_ticks;

        if (p->operation == ARRIVAL) {
            int old_fuel = p->fuel;

            p->fuel -= p->burn;
            if (p->fuel < 0) p->fuel = 0;

            event("FUEL", "%s fuel: %d -> %d",
                  p->number, old_fuel, p->fuel);

            if (p->fuel == 0) {
                p->state = DIVERTED;
                event("DIVERT", "%s left for an alternate airport because fuel was exhausted",
                      p->number);
            }
        }
    }
}

static void count_busy_time(void) {
    int i;

    for (i = 0; i < runway_count; ++i) {
        if (runways[i].busy) ++runways[i].busy_ticks;
    }
}

static int check_invariants(void) {
    int i, j;

    for (i = 0; i < runway_count; ++i) {
        Runway *r = &runways[i];

        if (r->busy) {
            Plane *p;

            if (r->plane_index < 0 || r->plane_index >= plane_count) return -1;

            p = &planes[r->plane_index];
            if (p->state != RUNNING || p->runway_index != i) return -1;
            if (r->length < types[p->type_index].required_length) return -1;
        }
    }

    for (i = 0; i < plane_count; ++i) {
        Plane *p = &planes[i];

        if (p->times_started > 1) return -1;
        if (p->fuel < 0) return -1;

        if (p->state == RUNNING) {
            if (p->runway_index < 0 || p->runway_index >= runway_count) return -1;
            if (!runways[p->runway_index].busy ||
                runways[p->runway_index].plane_index != i) {
                return -1;
            }
        }

        for (j = i + 1; j < plane_count; ++j) {
            if (strcmp(p->number, planes[j].number) == 0) return -1;
        }
    }

    return 0;
}

static int all_finished(void) {
    int i;

    for (i = 0; i < plane_count; ++i) {
        if (planes[i].state != DONE && planes[i].state != DIVERTED) {
            return 0;
        }
    }

    return 1;
}

static void sleep_between_ticks(void) {
    struct timespec ts;

    if (delay_ms <= 0) return;

    ts.tv_sec = delay_ms / 1000;
    ts.tv_nsec = (long)(delay_ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

const char *run_simulation(void) {
    current_tick = 0;
    elapsed_ticks = 0;

    for (;;) {
        if (stop_requested) return "USER_INTERRUPT";

        finish_operations();
        print_closure_events();
        register_requests();
        update_emergencies();
        dispatch_planes();
        burn_fuel_and_wait();
        count_busy_time();

        if (check_invariants() != 0) {
            event("INV-FAIL", "internal invariant check failed");
            return "INVARIANT_FAILURE";
        }

        ++elapsed_ticks;

        if (all_finished()) return "ALL_REQUESTS_COMPLETED";
        if (max_ticks > 0 && elapsed_ticks >= max_ticks) return "SHIFT_ENDED";

        sleep_between_ticks();
        ++current_tick;
    }
}

void print_termination_event(const char *reason) {
    if (strcmp(reason, "ALL_REQUESTS_COMPLETED") == 0) {
        event("COMPLETE", "all aircraft requests have a final outcome");
    } else if (strcmp(reason, "SHIFT_ENDED") == 0) {
        event("SHIFT-END", "shift limit reached; unfinished aircraft are listed in the summary");
    } else if (strcmp(reason, "USER_INTERRUPT") == 0) {
        event("STOP", "simulation interrupted by user or termination signal");
    }
}

void print_summary(const char *reason) {
    int landed = 0;
    int took_off = 0;
    int diverted = 0;
    int waiting = 0;
    int running = 0;
    int pending = 0;
    int emergencies = 0;
    int total_wait = 0;
    int completed = 0;
    int i;

    for (i = 0; i < plane_count; ++i) {
        Plane *p = &planes[i];

        if (p->emergency) ++emergencies;

        if (p->state == DONE) {
            ++completed;
            total_wait += p->wait_ticks;

            if (p->operation == ARRIVAL) ++landed;
            else ++took_off;

        } else if (p->state == DIVERTED) {
            ++diverted;
        } else if (p->state == WAITING) {
            ++waiting;
        } else if (p->state == RUNNING) {
            ++running;
        } else if (p->state == PENDING) {
            ++pending;
        }
    }

    print_line("");
    print_line("=== SUMMARY ===");
    print_line("termination=%s, elapsed_ticks=%d, last_tick=%d",
               reason, elapsed_ticks, current_tick);
    print_line("aircraft_total=%d, landed=%d, took_off=%d, diverted=%d, waiting=%d, operating=%d, pending=%d",
               plane_count, landed, took_off, diverted,
               waiting, running, pending);
    print_line("emergency_escalations=%d, completed_avg_wait=%.2f ticks",
               emergencies, completed ? (double)total_wait / completed : 0.0);
    print_line("dispatcher=%s assignments=%d emergency_assignments=%d",
               dispatcher.name, dispatcher.assignments,
               dispatcher.emergency_assignments);

    for (i = 0; i < runway_count; ++i) {
        double utilization = elapsed_ticks > 0
                               ? 100.0 * runways[i].busy_ticks / elapsed_ticks
                               : 0.0;

        print_line("runway=%s operations=%d busy_ticks=%d utilization=%.1f%%",
                   runways[i].name, runways[i].operations,
                   runways[i].busy_ticks, utilization);
    }

    if (waiting || running || pending) {
        print_line("Unfinished aircraft:");

        for (i = 0; i < plane_count; ++i) {
            if (planes[i].state == WAITING ||
                planes[i].state == RUNNING ||
                planes[i].state == PENDING) {
                print_line("  %s state=%s request_tick=%d fuel=%d",
                           planes[i].number,
                           planes[i].state == WAITING ? "WAITING" :
                           planes[i].state == RUNNING ? "OPERATING" : "PENDING",
                           planes[i].request_tick,
                           planes[i].fuel);
            }
        }
    }
}

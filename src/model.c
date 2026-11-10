#define _POSIX_C_SOURCE 200809L
#include "airport.h"
#include <stdio.h>
#include <string.h>


static const char *operation_name(int op) { return op == ARRIVAL ? "LANDING" : "TAKEOFF"; }

static int closed(int r) {
    for (int i = 0; i < airport->closure_count; ++i)
        if (airport->closures[i].runway_index == r && airport->current_tick >= airport->closures[i].start_tick &&
            airport->current_tick < airport->closures[i].end_tick)
            return 1;
    return 0;
}


static void update_waiting(Plane *p) {
    if (p->state != WAITING) return;
    int dt = airport->current_tick - p->fuel_tick;
    p->fuel_tick = airport->current_tick;
    if (p->operation != ARRIVAL) return;
    long long remaining = (long long) p->fuel - (long long) dt * p->burn;
    int fuel = remaining > 0 ? (int) remaining : 0;
    if (fuel != p->fuel) event("FUEL", "%s fuel: %d -> %d", p->number, p->fuel, fuel);
    p->fuel = fuel;
    if (!p->emergency && p->fuel <= (long long) p->burn * airport->emergency_fuel_ticks) {
        p->emergency = 1;
        event("EMERGENCY", "%s has critical fuel=%d; emergency priority enabled", p->number, p->fuel);
    }
    if (p->fuel == 0) {
        p->state = DIVERTED;
        event("DIVERT", "%s left for alternate airport; model fuel reserve exhausted", p->number);
    }
}

static void update_closures(int runway_index) {
    Runway *r = &airport->runways[runway_index];
    for (int c = 0; c < airport->closure_count; ++c) {
        Closure *cl = &airport->closures[c];
        if (cl->runway_index != runway_index) continue;
        if (!airport->closure_started[c] && airport->current_tick >= cl->start_tick) {
            airport->closure_started[c] = 1;
            event("CLOSE", "%s closed for new operations; active operation continues; scheduled=%d",
                  r->name, cl->start_tick);
        }
        if (!airport->closure_ended[c] && airport->current_tick >= cl->end_tick) {
            airport->closure_ended[c] = 1;
            event("OPEN", "%s closure ended; effective_closed=%d", r->name, closed(runway_index));
        }
    }
}


static void start_operation(int runway_index) {
    Runway *r = &airport->runways[runway_index];
    Plane *p = &airport->planes[r->plane_index];
    p->state = WAITING;
    update_waiting(p);
    if (p->state == DIVERTED || closed(runway_index)) {
        r->busy = 0;
        r->plane_index = -1;
        p->runway_index = -1;
        if (p->state != DIVERTED)
            event("REQUEUE", "%s reservation cancelled: %s closed before START", p->number, r->name);
        return;
    }
    p->state = RUNNING;
    ++p->times_started;
    r->finish_tick = airport->current_tick + p->duration;
    event("START", "%s started %s on %s; finish at T=%d",
          p->number, operation_name(p->operation), r->name, r->finish_tick);
}

static void finish_operation(int runway_index) {
    Runway *r = &airport->runways[runway_index];
    int plane_index = r->plane_index;
    Plane *p = &airport->planes[plane_index];
    p->state = DONE;
    p->runway_index = -1;
    r->busy = 0;
    r->plane_index = -1;
    r->next_free_tick = airport->current_tick + r->safety;
    ++r->operations;
    event("FINISH", "%s completed %s on %s", p->number, operation_name(p->operation), r->name);
    event("RELEASE", "%s released; next start not before T=%d", r->name, r->next_free_tick);
}

static int best_runway(int p) {
    int best = -1;
    for (int r = 0; r < airport->runway_count; ++r) {
        if (airport->runways[r].busy || closed(r) || airport->current_tick < airport->runways[r].next_free_tick ||
            airport->runways[r].length < airport->types[airport->planes[p].type_index].required_length)
            continue;
        if (best < 0 || airport->runways[r].length < airport->runways[best].length) best = r;
    }
    return best;
}

static int better(int a, int b) {
    if (airport->planes[a].emergency != airport->planes[b].emergency)
        return
                airport->planes[a].emergency > airport->planes[b].emergency;
    if (airport->planes[a].priority != airport->planes[b].priority)
        return
                airport->planes[a].priority > airport->planes[b].priority;
    if (airport->planes[a].request_tick != airport->planes[b].request_tick)
        return
                airport->planes[a].request_tick < airport->planes[b].request_tick;
    return a < b;
}

/* Самолёт подаёт запрос и следит за своим запасом топлива. */
static void plane_step(int plane_index) {
    Plane *p = &airport->planes[plane_index];
    if (p->state == PENDING && airport->current_tick >= p->request_tick) {
        p->state = WAITING;
        p->fuel_tick = airport->current_tick;
        event("REQUEST", "%s requests %s type=%s required=%d priority=%d fuel=%d burn=%d scheduled=%d",
              p->number, operation_name(p->operation), airport->types[p->type_index].name,
              airport->types[p->type_index].required_length, p->priority, p->fuel, p->burn, p->request_tick);
        event("QUEUE", "%s entered waiting queue", p->number);
    }
    update_waiting(p);
}

/* Полоса начинает назначенную операцию либо завершает текущую. */
static void runway_step(int runway_index) {
    int r = runway_index;
    update_closures(r);
    if (airport->runways[r].busy) {
        Plane *p = &airport->planes[airport->runways[r].plane_index];
        if (p->state == ASSIGNED) start_operation(r);
        else if (p->state == RUNNING && airport->current_tick >= airport->runways[r].finish_tick) finish_operation(r);
    }
}

/* Диспетчер выдаёт свободные полосы в порядке приоритетов. */
static void dispatcher_step(void) {
    for (int i = 0; i < airport->plane_count; ++i) update_waiting(&airport->planes[i]);
    for (;;) {
        int p = -1;
        for (int i = 0; i < airport->plane_count; ++i)
            if (airport->planes[i].state == WAITING && best_runway(i) >= 0 && (p < 0 || better(i, p))) p = i;
        if (p < 0) break;
        int r = best_runway(p);
        airport->planes[p].state = ASSIGNED;
        airport->planes[p].runway_index = r;
        airport->runways[r].busy = 1;
        airport->runways[r].plane_index = p;
        ++airport->dispatcher.assignments;
        if (airport->planes[p].emergency) ++airport->dispatcher.emergency_assignments;
        event("ASSIGN", "%s assigned %s to %s%s", airport->dispatcher.name, airport->planes[p].number,
              airport->runways[r].name, airport->planes[p].emergency ? " (EMERGENCY)" : "");
    }
}

/* ID самолётов идут первыми, затем ID полос и последний ID диспетчера. */
void actor_step(int id) {
    if (id < airport->plane_count) {
        plane_step(id);
    } else if (id < airport->plane_count + airport->runway_count) {
        runway_step(id - airport->plane_count);
    } else {
        dispatcher_step();
    }
}

int check_invariants(void) {
    for (int i = 0; i < airport->plane_count; ++i) {
        Plane *p = &airport->planes[i];
        if (p->fuel < 0 || p->times_started > 1) return -1;
        if (p->state == RUNNING || p->state == ASSIGNED) {
            int r = p->runway_index;
            if (r < 0 || r >= airport->runway_count || !airport->runways[r].busy || airport->runways[r].plane_index != i
                ||
                airport->runways[r].length < airport->types[p->type_index].required_length)
                return -1;
        }
    }
    for (int r = 0; r < airport->runway_count; ++r)
        if (airport->runways[r].busy) {
            int i = airport->runways[r].plane_index;
            if (i < 0 || i >= airport->plane_count || airport->planes[i].runway_index != r ||
                (airport->planes[i].state != RUNNING && airport->planes[i].state != ASSIGNED))
                return -1;
        }
    return 0;
}

int all_finished(void) {
    for (int i = 0; i < airport->plane_count; ++i)
        if (airport->planes[i].state != DONE && airport->planes[i].state != DIVERTED) return 0;
    return 1;
}

void print_termination_event(const char *reason) {
    if (strcmp(reason, "ALL_REQUESTS_COMPLETED") == 0) {
        event("COMPLETE", "all aircraft requests have a final outcome");
    } else if (strcmp(reason, "SHIFT_ENDED") == 0) {
        event("SHIFT-END", "shift limit reached; unfinished aircraft are listed in the summary");
    } else if (strcmp(reason, "USER_INTERRUPT") == 0) {
        event("STOP", "simulation interrupted by user or termination signal");
    } else {
        event("ERROR", "termination=%s", reason);
    }
}

void print_summary(const char *reason) {
    int counts[6] = {0};
    int landed = 0, took_off = 0;
    const char *names[] = {"PENDING", "WAITING", "ASSIGNED", "OPERATING", "DONE", "DIVERTED"};

    for (int i = 0; i < airport->plane_count; ++i) {
        Plane *p = &airport->planes[i];
        ++counts[p->state];
        if (p->state == DONE) {
            if (p->operation == ARRIVAL) ++landed;
            else ++took_off;
        }
    }
    print_line("\n=== SUMMARY ===");
    print_line("termination=%s, last_tick=%d", reason, airport->current_tick);
    print_line("aircraft_total=%d, landed=%d, took_off=%d, diverted=%d, waiting=%d, operating=%d, pending=%d",
               airport->plane_count, landed, took_off, counts[DIVERTED],
               counts[WAITING] + counts[ASSIGNED], counts[RUNNING], counts[PENDING]);
    print_line("dispatcher=%s assignments=%d emergency_assignments=%d",
               airport->dispatcher.name, airport->dispatcher.assignments, airport->dispatcher.emergency_assignments);
    for (int r = 0; r < airport->runway_count; ++r)
        print_line("runway=%s completed_operations=%d", airport->runways[r].name, airport->runways[r].operations);
    for (int i = 0; i < airport->plane_count; ++i) {
        Plane *p = &airport->planes[i];
        if (p->state != DONE && p->state != DIVERTED)
            print_line("unfinished=%s state=%s fuel=%d", p->number, names[p->state], p->fuel);
    }
}

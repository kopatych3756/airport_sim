#define _POSIX_C_SOURCE 200809L
#include "airport.h"
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>


static pthread_mutex_t world = PTHREAD_MUTEX_INITIALIZER;
static Notification inbox[MAX_PLANES + MAX_RUNWAYS + 1];
static pthread_t threads[MAX_PLANES + MAX_RUNWAYS + 1];
static int ids[MAX_PLANES + MAX_RUNWAYS + 1];
static int stopping, atc_id;
static int closure_started[MAX_CLOSURES], closure_ended[MAX_CLOSURES];
static void lock_world(void) { checked(pthread_mutex_lock(&world), "world lock"); }
static void unlock_world(void) { checked(pthread_mutex_unlock(&world), "world unlock"); }
static const char *operation_name(int op) { return op == ARRIVAL ? "LANDING" : "TAKEOFF"; }
static int closed(int r) {
    for (int i = 0; i < closure_count; ++i)
        if (closures[i].runway_index == r && current_tick >= closures[i].start_tick &&
            current_tick < closures[i].end_tick) return 1;
    return 0;
}


static void update_waiting(Plane *p) {
    if (p->state != WAITING) return;
    int dt = current_tick - p->fuel_tick;
    p->fuel_tick = current_tick;
    if (p->operation != ARRIVAL) return;
    long long remaining = (long long)p->fuel - (long long)dt * p->burn;
    int fuel = remaining > 0 ? (int)remaining : 0;
    if (fuel != p->fuel) event("FUEL", "%s fuel: %d -> %d", p->number, p->fuel, fuel);
    p->fuel = fuel;
    if (!p->emergency && p->fuel <= (long long)p->burn * emergency_fuel_ticks) {
        p->emergency = 1;
        event("EMERGENCY", "%s has critical fuel=%d; emergency priority enabled", p->number, p->fuel);
    }
    if (p->fuel == 0) {
        p->state = DIVERTED;
        event("DIVERT", "%s left for alternate airport; model fuel reserve exhausted", p->number);
    }
}
static void *aircraft_thread(void *arg) {
    int i = *(int *)arg;
    Plane *p = &planes[i];
    for (;;) {
        notification_wait(&inbox[i]);
        lock_world();
        if (stopping) { unlock_world(); break; }
        if (p->state == PENDING && current_tick >= p->request_tick) {
            p->state = WAITING;
            p->fuel_tick = current_tick;
            event("REQUEST", "%s requests %s type=%s required=%d priority=%d fuel=%d burn=%d scheduled=%d",
                  p->number, operation_name(p->operation), types[p->type_index].name,
                  types[p->type_index].required_length, p->priority, p->fuel, p->burn, p->request_tick);
            event("QUEUE", "%s entered waiting queue", p->number);
        }
        update_waiting(p);
        notification_send(&inbox[atc_id]);
        int terminal = p->state == DONE || p->state == DIVERTED;
        unlock_world();
        if (terminal) break;
    }
    return NULL;
}

static void update_closures(int runway_index) {
    Runway *r = &runways[runway_index];
    for (int c = 0; c < closure_count; ++c) {
        Closure *cl = &closures[c];
        if (cl->runway_index != runway_index) continue;
        if (!closure_started[c] && current_tick >= cl->start_tick) {
            closure_started[c] = 1;
            event("CLOSE", "%s closed for new operations; active operation continues; scheduled=%d",
                  r->name, cl->start_tick);
        }
        if (!closure_ended[c] && current_tick >= cl->end_tick) {
            closure_ended[c] = 1;
            event("OPEN", "%s closure ended; effective_closed=%d", r->name, closed(runway_index));
        }
    }
}


static void start_operation(int runway_index) {
    Runway *r = &runways[runway_index];
    Plane *p = &planes[r->plane_index];
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
    r->finish_tick = current_tick + p->duration;
    event("START", "%s started %s on %s; finish at T=%d",
          p->number, operation_name(p->operation), r->name, r->finish_tick);
}

static void finish_operation(int runway_index) {
    Runway *r = &runways[runway_index];
    int plane_index = r->plane_index;
    Plane *p = &planes[plane_index];
    p->state = DONE;
    p->runway_index = -1;
    r->busy = 0;
    r->plane_index = -1;
    r->next_free_tick = current_tick + r->safety;
    ++r->operations;
    event("FINISH", "%s completed %s on %s", p->number, operation_name(p->operation), r->name);
    event("RELEASE", "%s released; next start not before T=%d", r->name, r->next_free_tick);
    notification_send(&inbox[plane_index]);
}

static void *runway_thread(void *arg) {
    int id = *(int *)arg;
    int runway_index = id - plane_count;
    Runway *r = &runways[runway_index];
    for (;;) {
        notification_wait(&inbox[id]);
        lock_world();
        if (stopping) { unlock_world(); break; }
        update_closures(runway_index);
        if (r->busy) {
            Plane *p = &planes[r->plane_index];
            if (p->state == ASSIGNED)
                start_operation(runway_index);
            else if (p->state == RUNNING && current_tick >= r->finish_tick)
                finish_operation(runway_index);
        }
        notification_send(&inbox[atc_id]);
        unlock_world();
    }
    return NULL;
}

static int best_runway(int p) {
    int best = -1;
    for (int r = 0; r < runway_count; ++r) {
        if (runways[r].busy || closed(r) || current_tick < runways[r].next_free_tick ||
            runways[r].length < types[planes[p].type_index].required_length) continue;
        if (best < 0 || runways[r].length < runways[best].length) best = r;
    }
    return best;
}
static int better(int a, int b) {
    if (planes[a].emergency != planes[b].emergency) return planes[a].emergency > planes[b].emergency;
    if (planes[a].priority != planes[b].priority) return planes[a].priority > planes[b].priority;
    if (planes[a].request_tick != planes[b].request_tick) return planes[a].request_tick < planes[b].request_tick;
    return a < b;
}
static void *dispatcher_thread(void *unused) {
    (void)unused;
    for (;;) {
        notification_wait(&inbox[atc_id]);
        lock_world();
        if (stopping) { unlock_world(); break; }
        for (int i = 0; i < plane_count; ++i) update_waiting(&planes[i]);
        for (;;) {
            int p = -1;
            for (int i = 0; i < plane_count; ++i)
                if (planes[i].state == WAITING && best_runway(i) >= 0 && (p < 0 || better(i, p))) p = i;
            if (p < 0) break;
            int r = best_runway(p);
            planes[p].state = ASSIGNED;
            planes[p].runway_index = r;
            runways[r].busy = 1;
            runways[r].plane_index = p;
            ++dispatcher.assignments;
            if (planes[p].emergency) ++dispatcher.emergency_assignments;
            event("ASSIGN", "%s assigned %s to %s%s", dispatcher.name, planes[p].number,
                  runways[r].name, planes[p].emergency ? " (EMERGENCY)" : "");
            notification_send(&inbox[plane_count + r]);
        }
        unlock_world();
    }
    return NULL;
}
static int check_invariants(void) {
    for (int i = 0; i < plane_count; ++i) {
        Plane *p = &planes[i];
        if (p->fuel < 0 || p->times_started > 1) return -1;
        if (p->state == RUNNING || p->state == ASSIGNED) {
            int r = p->runway_index;
            if (r < 0 || r >= runway_count || !runways[r].busy || runways[r].plane_index != i ||
                runways[r].length < types[p->type_index].required_length) return -1;
        }
    }
    for (int r = 0; r < runway_count; ++r) if (runways[r].busy) {
        int i = runways[r].plane_index;
        if (i < 0 || i >= plane_count || planes[i].runway_index != r ||
            (planes[i].state != RUNNING && planes[i].state != ASSIGNED)) return -1;
    }
    return 0;
}
static int all_finished(void) {
    for (int i = 0; i < plane_count; ++i)
        if (planes[i].state != DONE && planes[i].state != DIVERTED) return 0;
    return 1;
}
const char *run_simulation(void) {
    int initialized = 0, created = 0, error = 0;
    const char *reason = "THREAD_FAILURE";
    atc_id = plane_count + runway_count;
    int count = atc_id + 1;
    sigset_t signals;
    sigemptyset(&signals); sigaddset(&signals, SIGINT); sigaddset(&signals, SIGTERM);
    print_line("backend=%s worker_threads=%d (aircraft=%d runways=%d dispatcher=1)", sync_backend(), count, plane_count, runway_count);
    for (int i = 0; i < count; ++i) {
        error = notification_init(&inbox[i]);
        if (error) break;
        ++initialized;
    }
    if (!error) for (int i = 0; i < count; ++i) {
        ids[i] = i;
        if (i < plane_count)
            error = pthread_create(&threads[i], NULL, aircraft_thread, &ids[i]);
        else if (i < atc_id)
            error = pthread_create(&threads[i], NULL, runway_thread, &ids[i]);
        else
            error = pthread_create(&threads[i], NULL, dispatcher_thread, &ids[i]);
        if (error) break;
        ++created;
    }
    if (error) print_line("THREAD-ERROR: %s; cleaning up %d workers", strerror(error), created);
    else {
        for (int i = 0; i < count; ++i) notification_send(&inbox[i]);
        for (;;) {
            int ms = delay_ms > 0 ? delay_ms : 1;
            struct timespec timeout = { ms / 1000, (long)(ms % 1000) * 1000000L };
            int sig = sigtimedwait(&signals, NULL, &timeout);
            int wait_error = errno;
            lock_world();
            if (sig == SIGINT || sig == SIGTERM) {
                reason = "USER_INTERRUPT";
            } else if (sig < 0 && wait_error != EAGAIN && wait_error != EINTR) reason = "THREAD_FAILURE";
            else if (check_invariants()) reason = "INVARIANT_FAILURE";
            else if (io_failed()) reason = "IO_FAILURE";
            else if (all_finished()) reason = "ALL_REQUESTS_COMPLETED";
            else if (max_ticks > 0 && current_tick >= max_ticks) reason = "SHIFT_ENDED";
            else if (current_tick >= INT_MAX - 2000001) reason = "TIME_LIMIT";
            else {
                ++current_tick;
                for (int i = 0; i < count; ++i) notification_send(&inbox[i]);
                unlock_world();
                continue;
            }
            stopping = 1;
            unlock_world();
            break;
        }
    }
    lock_world(); stopping = 1; unlock_world();
    for (int i = 0; i < created; ++i) notification_send(&inbox[i]);
    for (int i = 0; i < created; ++i) checked(pthread_join(threads[i], NULL), "pthread_join");
    for (int i = 0; i < initialized; ++i) notification_destroy(&inbox[i]);
    print_line("joined_threads=%d created_threads=%d", created, created);
    checked(pthread_mutex_destroy(&world), "world destroy");
    return reason;
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

    for (int i = 0; i < plane_count; ++i) {
        Plane *p = &planes[i];
        ++counts[p->state];
        if (p->state == DONE) {
            if (p->operation == ARRIVAL) ++landed;
            else ++took_off;
        }
    }
    print_line("\n=== SUMMARY ===");
    print_line("termination=%s, last_tick=%d", reason, current_tick);
    print_line("aircraft_total=%d, landed=%d, took_off=%d, diverted=%d, waiting=%d, operating=%d, pending=%d",
               plane_count, landed, took_off, counts[DIVERTED],
               counts[WAITING] + counts[ASSIGNED], counts[RUNNING], counts[PENDING]);
    print_line("dispatcher=%s assignments=%d emergency_assignments=%d",
               dispatcher.name, dispatcher.assignments, dispatcher.emergency_assignments);
    for (int r = 0; r < runway_count; ++r)
        print_line("runway=%s completed_operations=%d", runways[r].name, runways[r].operations);
    for (int i = 0; i < plane_count; ++i) {
        Plane *p = &planes[i];
        if (p->state != DONE && p->state != DIVERTED)
            print_line("unfinished=%s state=%s fuel=%d", p->number, names[p->state], p->fuel);
    }
}

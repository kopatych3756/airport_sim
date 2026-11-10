#ifndef AIRPORT_H
#define AIRPORT_H

#include <stddef.h>

#define MAX_RUNWAYS 10
#define MAX_TYPES 20
#define MAX_PLANES 300
#define MAX_CLOSURES 30
#define MAX_FLOWS 20
#define NAME_LEN 32

enum Operation {
    ARRIVAL = 0,
    DEPARTURE = 1
};

enum Priority {
    LOW = 0,
    NORMAL = 1,
    HIGH = 2
};

enum PlaneState {
    PENDING = 0,
    WAITING,
    ASSIGNED,
    RUNNING,
    DONE,
    DIVERTED
};

typedef struct {
    char name[NAME_LEN];
    int required_length;
    int landing_time;
    int takeoff_time;
} PlaneType;

typedef struct {
    char name[NAME_LEN];
    int length;
    int safety;

    int busy;
    int plane_index;
    int finish_tick;
    int next_free_tick;

    int operations;
} Runway;

typedef struct {
    char number[NAME_LEN];
    int operation;
    int type_index;
    int request_tick;
    int fuel;
    int burn;
    int priority;

    int state;
    int emergency;
    int runway_index;
    int duration;
    int times_started;
    int fuel_tick;
} Plane;

typedef struct {
    int runway_index;
    int start_tick;
    int end_tick;
} Closure;

typedef struct {
    int operation;
    int count;
    int first_tick;
    int min_interval;
    int max_interval;
} Flow;

typedef struct {
    char name[NAME_LEN];
    int assignments;
    int emergency_assignments;
} Dispatcher;


typedef struct {
    unsigned seed_value;
    int max_ticks, delay_ms, emergency_fuel_ticks;
    int priority_weights[3], arrival_fuel_min, arrival_fuel_max, arrival_burn_min, arrival_burn_max;
    Runway runways[MAX_RUNWAYS];
    PlaneType types[MAX_TYPES];
    Plane planes[MAX_PLANES];
    Closure closures[MAX_CLOSURES];
    Flow flows[MAX_FLOWS];
    int runway_count, type_count, plane_count, closure_count, flow_count, current_tick;
    int closure_started[MAX_CLOSURES], closure_ended[MAX_CLOSURES];
    Dispatcher dispatcher;
    char events[32768];
} Airport;

extern Airport *airport;

int load_config(const char *path);

int generate_flows(void);

void set_quiet_mode(int quiet);

int open_log_file(const char *path);

void close_log_file(void);

int io_failed(void);

void print_line(const char *format, ...);

void event(const char *name, const char *format, ...);

const char *run_simulation(void);

void print_termination_event(const char *reason);

void print_summary(const char *reason);

/* Один вызов выполняет шаг самолёта, полосы либо диспетчера. */
void actor_step(int id);

int check_invariants(void);

int all_finished(void);

void capture_events(char *buffer, size_t size);

int ipc_worker(const char *session, int id);

const char *ipc_start(void);

int ipc_step(int id);

void ipc_finish(int healthy);

int stop_requested(int delay_ms);

extern const char *ipc_mode, *ipc_session;
extern int ipc_external;
#endif

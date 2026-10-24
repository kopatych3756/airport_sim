#ifndef AIRPORT_H
#define AIRPORT_H

#include <pthread.h>
#ifndef USE_CONDVAR
#include <semaphore.h>
#endif

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

extern unsigned int seed_value;
extern int max_ticks;
extern int delay_ms;
extern int emergency_fuel_ticks;
extern int priority_weights[3];
extern int arrival_fuel_min;
extern int arrival_fuel_max;
extern int arrival_burn_min;
extern int arrival_burn_max;

extern Runway runways[MAX_RUNWAYS];
extern int runway_count;
extern PlaneType types[MAX_TYPES];
extern int type_count;
extern Plane planes[MAX_PLANES];
extern int plane_count;
extern Closure closures[MAX_CLOSURES];
extern int closure_count;
extern Flow flows[MAX_FLOWS];
extern int flow_count;
extern Dispatcher dispatcher;

extern int current_tick;

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

typedef struct {
    pthread_mutex_t mutex;
    int pending;
#ifdef USE_CONDVAR
    pthread_cond_t condition;
#else
    sem_t semaphore;
#endif
} Notification;
int notification_init(Notification *n);
void notification_destroy(Notification *n);
void notification_send(Notification *n);
void notification_wait(Notification *n);
void checked(int error, const char *operation);
const char *sync_backend(void);

#endif

#define _POSIX_C_SOURCE 200809L
#include "airport.h"
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ACTORS (MAX_PLANES+MAX_RUNWAYS+1)

typedef struct {
    int stop;
    pid_t owner_pid;
    Airport state;
} Command;

typedef struct {
    Command command;
    sem_t gate[ACTORS], ack, ready;
} Shared;

const char *ipc_mode = "named", *ipc_session = NULL;
int ipc_external = 0;
static Shared *shared;
static Command local;
static Airport *original_airport;
static sem_t *claims[ACTORS];
static sem_t *gates[ACTORS], *ack = SEM_FAILED, *ready = SEM_FAILED;
static int request_fd = -1, reply_fd = -1;
static int have_request, have_reply;
static char prefix[80];
static pid_t children[ACTORS];
static int count, named, use_fifo, initialized, created, owner, fd = -1;
static int have_shm, have_ack, have_ready;

/* У каждого запуска свои имена: UID пользователя + имя сессии. */
static void name(char *out, const char *suffix, int id) {
    if (id >= 0) {
        snprintf(out, 128, "%s_%s_%d", prefix, suffix, id);
    } else {
        snprintf(out, 128, "%s_%s", prefix, suffix);
    }
}

static struct timespec deadline(int seconds) {
    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time);
    time.tv_sec += seconds;
    return time;
}

static int wait_sem(sem_t *semaphore, int seconds) {
    struct timespec limit = deadline(seconds);
    while (sem_timedwait(semaphore, &limit) != 0) {
        if (errno != EINTR) return -1;
    }
    return 0;
}


static int send_command(int fd, const Command *command) {
    const char *data = (const char *) command;
    size_t remaining = sizeof(*command);
    while (remaining > 0) {
        struct pollfd pipe = {fd, POLLOUT, 0};
        int result = poll(&pipe, 1, 5000);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return -1;
        ssize_t size = write(fd, data, remaining);
        if (size < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (size <= 0) return -1;
        data += size;
        remaining -= (size_t) size;
    }
    return 0;
}

static int receive_command(int fd, Command *command) {
    char *data = (char *) command;
    size_t remaining = sizeof(*command);
    while (remaining > 0) {
        struct pollfd pipe = {fd, POLLIN, 0};
        int result = poll(&pipe, 1, 5000);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return -1;
        ssize_t size = read(fd, data, remaining);
        if (size < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (size <= 0) return -1;
        data += size;
        remaining -= (size_t) size;
    }
    return 0;
}

/* O_RDWR позволяет открыть FIFO до подключения остальных процессов.
 * Направление использования задаётся send_command/receive_command. */
static int open_pipe(const char *suffix) {
    char path[160];
    snprintf(path, sizeof(path), "/tmp%s_%s", prefix, suffix);
    return open(path, O_RDWR | O_NONBLOCK);
}

static int create_pipe(const char *suffix, int *created_flag) {
    char path[160];
    snprintf(path, sizeof(path), "/tmp%s_%s", prefix, suffix);
    if (mkfifo(path, 0600)) return -1;
    *created_flag = 1;
    return open_pipe(suffix);
}

static void remove_pipe(const char *suffix) {
    char path[160];
    snprintf(path, sizeof(path), "/tmp%s_%s", prefix, suffix);
    unlink(path);
}

static int setup_prefix(const char *session) {
    if (!session || !*session || strlen(session) > 40) return -1;
    for (const char *p = session; *p; ++p) {
        int letter = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z');
        int digit = *p >= '0' && *p <= '9';
        if (!letter && !digit && *p != '_') return -1;
    }
    snprintf(prefix, sizeof(prefix), "/airport_%lu_%s", (unsigned long) getuid(), session);
    return 0;
}

static sem_t *open_sem(const char *suffix, int id, unsigned value) {
    char semaphore_name[128];
    name(semaphore_name, suffix, id);
    if (owner) {
        return sem_open(semaphore_name, O_CREAT | O_EXCL, 0600, value);
    }
    return sem_open(semaphore_name, 0);
}

/* В общей памяти создаётся сразу всё состояние аэропорта. */
static int create_memory(void) {
    char memory_name[128];
    name(memory_name, "world", -1);
    fd = shm_open(memory_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    have_shm = 1;
    if (ftruncate(fd, sizeof(Shared))) return -1;
    shared = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shared == MAP_FAILED) {
        shared = NULL;
        return -1;
    }
    memset(shared, 0, sizeof(*shared));
    return 0;
}

static int create_semaphores(void) {
    if (named) {
        ack = open_sem("ack", -1, 0);
        if (ack == SEM_FAILED) return -1;
        have_ack = 1;
        ready = open_sem("ready", -1, 0);
        if (ready == SEM_FAILED) return -1;
        have_ready = 1;
        for (int i = 0; i < count; ++i) {
            gates[i] = open_sem("gate", i, 0);
            if (gates[i] == SEM_FAILED) return -1;
            ++initialized;
            claims[i] = open_sem("claim", i, 1);
            if (claims[i] == SEM_FAILED) return -1;
        }
    } else {
        ack = &shared->ack;
        ready = &shared->ready;
        if (sem_init(ack, 1, 0)) return -1;
        have_ack = 1;
        if (sem_init(ready, 1, 0)) return -1;
        have_ready = 1;
        for (int i = 0; i < count; ++i) {
            gates[i] = &shared->gate[i];
            if (sem_init(gates[i], 1, 0)) return -1;
            ++initialized;
        }
    }
    return 0;
}

static int init_owner(void) {
    owner = 1;
    for (int i = 0; i < ACTORS; ++i) {
        gates[i] = SEM_FAILED;
        claims[i] = SEM_FAILED;
    }
    if (!use_fifo && create_memory()) return -1;
    if (create_semaphores()) return -1;

    if (use_fifo) {
        request_fd = create_pipe("request", &have_request);
        if (request_fd < 0) return -1;
        reply_fd = create_pipe("reply", &have_reply);
        if (reply_fd < 0) return -1;
        local.state = *airport;
        airport = &local.state;
    } else {
        shared->command.state = *airport;
        airport = &shared->command.state;
    }
    return 0;
}

static volatile sig_atomic_t worker_stop;

static void stop_worker(int signal_number) {
    (void) signal_number;
    worker_stop = 1;
}

/* Участник делает только три действия: ждёт, выполняет свой шаг, отвечает. */
static int worker_loop(int id) {
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = stop_worker;
    sigaction(SIGTERM, &action, NULL);

    action.sa_handler = SIG_IGN;
    sigaction(SIGINT, &action, NULL);

    if (named && (claims[id] == SEM_FAILED || sem_trywait(claims[id]))) {
        perror("duplicate actor");
        return 3;
    }
    if (sem_post(ready)) return 3;
    pid_t owner_pid = getppid();
    int idle = 0;

    while (!worker_stop) {
        if (wait_sem(gates[id], 1)) {
            int wait_error = errno;
            if (wait_error != ETIMEDOUT && wait_error != EINTR) return 3;
            if (++idle > 25 && owner_pid <= 1) break;
            if (owner_pid > 1 && kill(owner_pid, 0) && errno == ESRCH) break;
            /* При ошибке координатор удаляет сессию. Остальные участники выходят. */
            if (named) {
                sem_t *probe = open_sem("gate", id, 0);
                if (probe == SEM_FAILED) break;
                sem_close(probe);
            } else if (getppid() == 1) {
                break;
            }
            continue;
        }

        Command *command = use_fifo ? &local : &shared->command;
        if (use_fifo && receive_command(request_fd, command)) return 3;
        idle = 0;
        owner_pid = command->owner_pid;
        if (!command->stop) {
            airport = &command->state;
            capture_events(airport->events, sizeof(airport->events));
            actor_step(id);
        }
        if (use_fifo && send_command(reply_fd, command)) return 3;
        if (sem_post(ack)) return 3;
        if (command->stop) break;
    }
    return 0;
}

int ipc_worker(const char *session, int id) {
    if (strcmp(ipc_mode, "shm") && strcmp(ipc_mode, "fifo")) return 2;
    if (id < 0 || id >= ACTORS || setup_prefix(session)) return 2;
    named = 1;
    use_fifo = !strcmp(ipc_mode, "fifo");
    owner = 0;
    char n[128];
    /* Owner publishes startup instructions only after all objects exist. */
    ack = open_sem("ack", -1, 0);
    ready = open_sem("ready", -1, 0);
    gates[id] = open_sem("gate", id, 0);
    claims[id] = open_sem("claim", id, 0);
    if (ack == SEM_FAILED || ready == SEM_FAILED || gates[id] == SEM_FAILED) {
        perror("worker sem_open");
        return 3;
    }
    if (use_fifo) {
        request_fd = open_pipe("request");
        reply_fd = open_pipe("reply");
        if (request_fd < 0 || reply_fd < 0) return 3;
    } else {
        name(n, "world", -1);
        fd = shm_open(n,O_RDWR, 0600);
        if (fd < 0) return 3;
        shared = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (shared == MAP_FAILED) return 3;
    }
    int result = worker_loop(id);
    sem_close(gates[id]);
    sem_close(claims[id]);
    sem_close(ack);
    sem_close(ready);
    if (shared) munmap(shared, sizeof(Shared));
    if (fd >= 0) close(fd);
    if (request_fd >= 0) close(request_fd);
    if (reply_fd >= 0) close(reply_fd);
    return result;
}

static int transaction(int id, int stop) {
    Command *command = use_fifo ? &local : &shared->command;
    command->stop = stop;
    command->owner_pid = getpid();
    if (!stop) command->state.events[0] = 0;

    if (sem_post(gates[id])) return -1;
    if (use_fifo) {
        if (send_command(request_fd, command)) return -1;
        if (receive_command(reply_fd, command)) return -1;
    }
    if (wait_sem(ack, 5)) return -1;

    if (!stop) {
        char *save = NULL;
        char *line = strtok_r(command->state.events, "\n", &save);
        while (line) {
            print_line("%s", line);
            line = strtok_r(NULL, "\n", &save);
        }
    }
    return 0;
}

/* Уборка вызывается и после обычного завершения, и после ошибки. */
static void cleanup(int healthy) {
    if (healthy) {
        for (int i = 0; i < count; ++i) {
            if (transaction(i, 1)) {
                healthy = 0;
                break;
            }
        }
    }
    for (int i = 0; i < created; ++i) {
        if (!healthy) kill(children[i], SIGTERM);
    }
    for (int i = 0; i < created; ++i) {
        while (waitpid(children[i], NULL, 0) < 0 && errno == EINTR) {
        }
    }

    char object_name[128];
    for (int i = 0; i < initialized; ++i) {
        if (named) {
            sem_close(gates[i]);
            name(object_name, "gate", i);
            sem_unlink(object_name);
            if (claims[i] != SEM_FAILED) {
                sem_close(claims[i]);
                name(object_name, "claim", i);
                sem_unlink(object_name);
            }
        } else {
            sem_destroy(gates[i]);
        }
    }
    if (have_ack) {
        if (named) {
            sem_close(ack);
            name(object_name, "ack", -1);
            sem_unlink(object_name);
        } else {
            sem_destroy(ack);
        }
    }
    if (have_ready) {
        if (named) {
            sem_close(ready);
            name(object_name, "ready", -1);
            sem_unlink(object_name);
        } else {
            sem_destroy(ready);
        }
    }
    if (request_fd >= 0) close(request_fd);
    if (reply_fd >= 0) close(reply_fd);
    if (have_request) remove_pipe("request");
    if (have_reply) remove_pipe("reply");

    /* Сохраняем результаты, чтобы main напечатал итоги после munmap. */
    if (original_airport) {
        *original_airport = *airport;
        airport = original_airport;
    }
    if (shared) munmap(shared, sizeof(Shared));
    if (fd >= 0) close(fd);
    if (have_shm) {
        name(object_name, "world", -1);
        shm_unlink(object_name);
    }
    print_line("IPC_CLEANUP actors=%d children_reaped=%d", count, created);
}

/* Подготовка и запуск. NULL означает, что все участники подключились. */
const char *ipc_start(void) {
    original_airport = airport;
    if (strcmp(ipc_mode, "named") && strcmp(ipc_mode, "unnamed") &&
        strcmp(ipc_mode, "shm") && strcmp(ipc_mode, "fifo")) {
        return "IPC_FAILURE";
    }
    use_fifo = !strcmp(ipc_mode, "fifo");
    named = strcmp(ipc_mode, "unnamed") != 0;
    if (ipc_external && strcmp(ipc_mode, "shm") && !use_fifo) return "IPC_FAILURE";

    char automatic[40];
    snprintf(automatic, sizeof(automatic), "s%ld", (long) getpid());
    const char *session = ipc_session ? ipc_session : automatic;
    if (setup_prefix(session)) return "IPC_FAILURE";
    count = airport->plane_count + airport->runway_count + 1;
    print_line("backend=%s actors=%d session=%s airport_bytes=%zu",
               ipc_mode, count, session, sizeof(Airport));
    if (init_owner()) {
        perror("IPC create");
        return "IPC_FAILURE";
    }

    if (ipc_external) {
        print_line("READY session=%s actors=%d plane_ids=0..%d runway_ids=%d..%d dispatcher_id=%d",
                   session, count, airport->plane_count - 1, airport->plane_count,
                   airport->plane_count + airport->runway_count - 1, count - 1);
        print_line("Start each actor separately: ./build/airport --worker %s %s ID", ipc_mode, session);
    } else {
        for (int i = 0; i < count; ++i) {
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                return "IPC_FAILURE";
            }
            if (pid == 0) _exit(worker_loop(i));
            children[created++] = pid;
        }
    }

    struct timespec limit = deadline(20);
    for (int connected = 0; connected < count;) {
        if (stop_requested(0)) return "USER_INTERRUPT";
        if (wait_sem(ready, 1) == 0) {
            ++connected;
            continue;
        }
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec >= limit.tv_sec) {
            perror("startup timeout");
            return "IPC_FAILURE";
        }
    }
    return NULL;
}

int ipc_step(int id) {
    return transaction(id, 0);
}

void ipc_finish(int healthy) {
    cleanup(healthy);
}

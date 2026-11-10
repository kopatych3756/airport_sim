#define _POSIX_C_SOURCE 200809L
#include "airport.h"
#include <signal.h>

/* Сигналы блокируются в main. Здесь координатор проверяет их без обработчика. */
int stop_requested(int delay_ms) {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    struct timespec timeout = {
        delay_ms / 1000,
        (long)(delay_ms % 1000) * 1000000L
    };
    int signal_number = sigtimedwait(&signals, NULL, &timeout);
    return signal_number == SIGINT || signal_number == SIGTERM;
}

/* Каждый такт имеет понятный порядок. Время не меняется до ответов участников. */
static int run_tick(void) {
    int first_runway = airport->plane_count;
    int dispatcher_id = first_runway + airport->runway_count;

    /* 1. Самолёты подают запросы и расходуют топливо. */
    for (int id = 0; id < first_runway; ++id) {
        if (ipc_step(id)) return -1;
    }
    /* 2. Полосы завершают операции и обновляют закрытия. */
    for (int id = first_runway; id < dispatcher_id; ++id) {
        if (ipc_step(id)) return -1;
    }
    /* 3. Диспетчер назначает полосы. */
    if (ipc_step(dispatcher_id)) return -1;
    /* 4. Полосы начинают новые операции. */
    for (int id = first_runway; id < dispatcher_id; ++id) {
        if (ipc_step(id)) return -1;
    }
    return 0;
}

const char *run_simulation(void) {
    const char *reason = ipc_start();
    int healthy = reason == NULL;

    while (reason == NULL) {
        if (stop_requested(0)) {
            reason = "USER_INTERRUPT";
        } else if (run_tick()) {
            healthy = 0;
            reason = stop_requested(0) ? "USER_INTERRUPT" : "IPC_FAILURE";
        } else if (check_invariants()) {
            reason = "INVARIANT_FAILURE";
        } else if (io_failed()) {
            reason = "IO_FAILURE";
        } else if (all_finished()) {
            reason = "ALL_REQUESTS_COMPLETED";
        } else if (airport->max_ticks > 0 && airport->current_tick >= airport->max_ticks) {
            reason = "SHIFT_ENDED";
        } else if (airport->current_tick >= 2000000000) {
            reason = "TIME_LIMIT";
        } else if (stop_requested(airport->delay_ms)) {
            reason = "USER_INTERRUPT";
        } else {
            ++airport->current_tick;
        }
    }

    ipc_finish(healthy);
    return reason;
}

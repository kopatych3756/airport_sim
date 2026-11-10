#define _POSIX_C_SOURCE 200809L
#include "airport.h"
#include <errno.h>
#include <signal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static Airport initial_airport = {
    .seed_value = 31, .max_ticks = 120, .emergency_fuel_ticks = 3,
    .priority_weights = {10, 65, 25},
    .arrival_fuel_min = 9, .arrival_fuel_max = 22, .arrival_burn_min = 1, .arrival_burn_max = 2,
    .dispatcher = {"ATC", 0, 0}
};
Airport *airport = &initial_airport;

static void print_help(const char *program) {
    printf("  --mode named|unnamed|shm|fifo   IPC implementation\n");
    printf("  --session NAME --external    independently started workers (shm/fifo)\n");
    printf("  --worker shm|fifo NAME ID      worker process entry point\n");
    printf("Usage: %s [options]\n", program);
    printf("  -c, --config FILE   configuration file\n");
    printf("  --seed N            override random seed\n");
    printf("  --max-ticks N       override shift length (0 = unlimited)\n");
    printf("  --delay-ms N        delay between ticks\n");
    printf("  --log FILE          log file\n");
    printf("  --no-log            do not save a log file\n");
    printf("  --quiet             do not print events to terminal\n");
    printf("  -h, --help          show help\n");
}

static int cli_number(const char *text, int *value) {
    char *end;
    long n;
    errno = 0;
    n = strtol(text, &end, 10);
    if (errno || !*text || *end || n < 0 || n > 1000000) {
        fprintf(stderr, "Invalid integer (expected 0..1000000): %s\n", text);
        return -1;
    }
    *value = (int) n;
    return 0;
}

int main(int argc, char **argv) {
#ifdef DEFAULT_MODE
    ipc_mode = DEFAULT_MODE;
#endif
    if (argc == 5 && !strcmp(argv[1], "--worker")) {
        int id;
        if (cli_number(argv[4], &id)) return 2;
        ipc_mode = argv[2];
        return ipc_worker(argv[3], id);
    }
    const char *config_path = "configs/default.conf";
    const char *log_path = "simulation.log";
    int logging_enabled = 1;
    int quiet = 0;

    int cli_seed = -1, cli_ticks = -1, cli_delay = -1;

    int i;
    const char *reason;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            ipc_mode = argv[++i];
        } else if (!strcmp(argv[i], "--session") && i + 1 < argc) {
            ipc_session = argv[++i];
        } else if (!strcmp(argv[i], "--external")) {
            ipc_external = 1;
        } else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) &&
                   i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            if (cli_number(argv[++i], &cli_seed)) return 2;
        } else if (strcmp(argv[i], "--max-ticks") == 0 && i + 1 < argc) {
            if (cli_number(argv[++i], &cli_ticks)) return 2;
        } else if (strcmp(argv[i], "--delay-ms") == 0 && i + 1 < argc) {
            if (cli_number(argv[++i], &cli_delay)) return 2;
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else if (strcmp(argv[i], "--no-log") == 0) {
            logging_enabled = 0;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            print_help(argv[0]);
            return 2;
        }
    }

    if (load_config(config_path) != 0) return 2;

    if (cli_seed >= 0) airport->seed_value = (unsigned int) cli_seed;
    if (cli_ticks >= 0) airport->max_ticks = cli_ticks;
    if (cli_delay >= 0) airport->delay_ms = cli_delay;

    srand(airport->seed_value);
    if (generate_flows() != 0) return 2;

    set_quiet_mode(quiet);
    if (logging_enabled && open_log_file(log_path) != 0) return 2;

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &signals, NULL)) return 3;
    signal(SIGPIPE, SIG_IGN);

    print_line("=== AIRPORT RUNWAY SIMULATION / VARIANT 31 ===");
    print_line("seed=%u max_ticks=%d delay_ms=%d", airport->seed_value, airport->max_ticks, airport->delay_ms);
    print_line("runways=%d aircraft=%d types=%d closures=%d",
               airport->runway_count, airport->plane_count, airport->type_count, airport->closure_count);
    print_line("dispatcher=%s", airport->dispatcher.name);
    print_line("Priority: emergency -> HIGH/NORMAL/LOW -> earlier request.");
    print_line("");

    reason = run_simulation();
    print_termination_event(reason);
    print_summary(reason);

    close_log_file();

    if (io_failed()) return 4;
    if (strcmp(reason, "INVARIANT_FAILURE") == 0 ||
        strcmp(reason, "IPC_FAILURE") == 0 ||
        strcmp(reason, "TIME_LIMIT") == 0)
        return 3;
    if (strcmp(reason, "USER_INTERRUPT") == 0) return 130;
    return 0;
}
